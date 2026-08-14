/*
 * libvvdec-backed VVC decoder for Plex's external decoder module.
 *
 * Decodes via the Fraunhofer reference decoder (libvvdec 2.x) instead
 * of the bundled experimental native decoder, which produces artifacts
 * on real-world streams. Adapted from FFmpeg's libavcodec/libvvdec.c.
 *
 * The module's av_init_library() hands the host transcoder this codec
 * descriptor; the host's codec lookup for "vvc" then resolves to it.
 */
#include <string.h>

#include "libavcodec/avcodec.h"
#include "libavcodec/codec_internal.h"
#include "libavcodec/decode.h"
#include "libavcodec/internal.h"
#include "libavutil/avassert.h"
#include "libavutil/common.h"
#include "libavutil/imgutils.h"
#include "libavutil/opt.h"
#include "libavutil/pixdesc.h"

#include "vvdec.h"

typedef struct VVDecContext {
    vvdecDecoder *dec_ctx;
    uint8_t *buf;
    int      buf_size;
    int      buf_cap;
} VVDecContext;

/* VVC NAL unit type (2-byte header after the start code). */
static int vvc_nal_type(const uint8_t *pkt, int size)
{
    if (size >= 6 && pkt[0] == 0 && pkt[1] == 0 && pkt[2] == 0 && pkt[3] == 1)
        return (pkt[4] & 0x07) << 3 | (pkt[5] >> 5);
    if (size >= 5 && pkt[0] == 0 && pkt[1] == 0 && pkt[2] == 1)
        return (pkt[3] & 0x07) << 3 | (pkt[4] >> 5);
    return -1;
}

/* Non-VCL NALs (OPI, APS, SEI, VPS, SPS, PPS, AUD, ...) must be fed to
 * the decoder together with the following VCL access unit; on their own
 * vvdec rejects them (VVDEC_ERR_DEC_INPUT). */
static int vvc_nal_is_vcl(int type)
{
    switch (type) {
    case 6:   /* OPI */
    case 12:  /* OPI (alternate id) */
    case 15:  /* PREFIX_APS */
    case 16:  /* SUFFIX_APS */
    case 17:  /* PREFIX_SEI */
    case 18:  /* SUFFIX_SEI */
    case 19:  /* VPS */
    case 20:  /* SPS */
    case 21:  /* PPS */
    case 22:  /* PREFIX_OPI */
    case 23:  /* SUFFIX_OPI */
        return 0;
    }
    return type >= 0 && type <= 23;
}

static int vvc_buf_append(VVDecContext *s, const uint8_t *data, int size)
{
    if (s->buf_size + size > s->buf_cap) {
        int ncap = (s->buf_size + size + 65535) & ~65535;
        uint8_t *nbuf = av_realloc(s->buf, ncap);
        if (!nbuf)
            return AVERROR(ENOMEM);
        s->buf = nbuf;
        s->buf_cap = ncap;
    }
    memcpy(s->buf + s->buf_size, data, size);
    s->buf_size += size;
    return 0;
}

/* Convert the vvcC-style CodecPrivate extradata (length-prefixed NALs
 * with a PTL header) into annex-B and feed it to vvdec. Mirrors the
 * logic of FFmpeg's vvc_mp4toannexb bitstream filter. */
static void vvc_feed_extradata(AVCodecContext *avctx, VVDecContext *s)
{
    const uint8_t *e = avctx->extradata;
    int esize = avctx->extradata_size;
    int pos = 0, temp, length_size, ptl_present, num_arrays, i, j;
    uint8_t *out = NULL;
    int out_size = 0;

    if (esize < 3)
        return;

    temp = e[pos++];
    length_size = ((temp & 6) >> 1) + 1;
    ptl_present = temp & 1;
    av_log(avctx, AV_LOG_ERROR,
           "vvcC: temp %d length_size %d ptl %d esize %d\n",
           temp, length_size, ptl_present, esize);

    if (ptl_present) {
        int temp2, num_sublayers, num_bytes_constraint_info;
        int ptl_num_sub_profiles;

        if (pos + 2 > esize)
            goto done;
        temp2 = (e[pos] << 8) | e[pos + 1];
        pos += 2;
        num_sublayers = (temp2 >> 4) & 0x7;

        if (pos >= esize)
            goto done;
        num_bytes_constraint_info = e[pos] & 0x3f;
        pos += 1;                       /* temp3 */
        av_log(avctx, AV_LOG_ERROR,
               "vvcC: nsub %d ncbi %d pos %d\n",
               num_sublayers, num_bytes_constraint_info, pos);
        if (pos + 2 + num_bytes_constraint_info - 1 > esize)
            goto done;
        pos += 1;                       /* temp4: profile/tier */
        pos += 1;                       /* general_level_idc */
        pos += num_bytes_constraint_info - 1;

        if (num_sublayers > 1) {
            int flags, k;
            if (pos >= esize)
                goto done;
            flags = e[pos++];           /* ptl_sublayer_level_present_flag */
            for (k = 0; k < num_sublayers - 1; k++) {
                if (flags & (0x80 >> k))
                    pos += 1;           /* sublayer_level_idc */
            }
        }

        if (pos >= esize)
            goto done;
        ptl_num_sub_profiles = e[pos++];
        av_log(avctx, AV_LOG_ERROR,
               "vvcC: subprof %d pos %d (limit %d)\n",
               ptl_num_sub_profiles, pos, esize);
        if (pos >= 8 && pos <= esize - 16) {
            av_log(avctx, AV_LOG_ERROR,
                   "vvcC: raw2[%d..%d]: %02x %02x %02x %02x %02x %02x %02x %02x "
                   "%02x %02x %02x %02x %02x %02x %02x %02x\n",
                   pos - 8, pos + 7,
                   e[pos - 8], e[pos - 7], e[pos - 6], e[pos - 5],
                   e[pos - 4], e[pos - 3], e[pos - 2], e[pos - 1],
                   e[pos], e[pos + 1], e[pos + 2], e[pos + 3],
                   e[pos + 4], e[pos + 5], e[pos + 6], e[pos + 7]);
        }
        if (pos + 4 * ptl_num_sub_profiles + 6 > esize)
            goto done;
        pos += 4 * ptl_num_sub_profiles; /* general_sub_profile_idc */
        pos += 6;                        /* max width/height, frame rate */
    }

    if (pos >= esize)
        goto done;
    num_arrays = e[pos++];
    av_log(avctx, AV_LOG_ERROR,
           "vvcC: pos after PTL %d, num_arrays %d\n", pos - 1, num_arrays);

    for (i = 0; i < num_arrays && pos + 3 <= esize; i++) {
        int type = e[pos] & 0x1f;
        int cnt;
        pos += 1;
        if (type == 12 || type == 13)   /* VVC_OPI_NUT / VVC_DCI_NUT */
            cnt = 1;
        else {
            cnt = (e[pos] << 8) | e[pos + 1];
            pos += 2;
        }
        av_log(avctx, AV_LOG_ERROR, "vvcC: array %d type %d cnt %d\n",
               i, type, cnt);
        for (j = 0; j < cnt; j++) {
            int nalu_len;
            uint8_t *nb;
            if (pos + 2 > esize)
                goto done;
            nalu_len = (e[pos] << 8) | e[pos + 1];
            pos += 2;
            if (pos + nalu_len > esize)
                goto done;
            nb = av_realloc(out, out_size + nalu_len + 4 + 64);
            if (!nb)
                goto done;
            out = nb;
            out[out_size]     = 0;   /* 00 00 00 01 start code */
            out[out_size + 1] = 0;
            out[out_size + 2] = 0;
            out[out_size + 3] = 1;
            memcpy(out + out_size + 4, e + pos, nalu_len);
            out_size += 4 + nalu_len;
            pos += nalu_len;
        }
    }

done:
    if (out && out_size > 0) {
        vvdecAccessUnit au;
        vvdecFrame *f = NULL;
        int dr;
        memset(&au, 0, sizeof(au));
        au.payload         = out;
        au.payloadUsedSize = out_size;
        dr = vvdec_decode(s->dec_ctx, &au, &f);
        av_log(avctx, AV_LOG_ERROR,
               "extradata feed: %d bytes, head %02x %02x %02x %02x %02x %02x, "
               "types ", out_size, out[0], out[1], out[2], out[3],
               out[4], out[5]);
        for (j = 0; j + 5 < out_size; ) {
            int t;
            while (j + 4 < out_size &&
                   !(out[j] == 0 && out[j + 1] == 0 && out[j + 2] == 0 && out[j + 3] == 1))
                j++;
            if (j + 5 >= out_size)
                break;
            t = (out[j + 4] & 0x03) << 3 | (out[j + 5] >> 5);
            av_log(avctx, AV_LOG_ERROR, "%d ", t);
            j += 6;
        }
        av_log(avctx, AV_LOG_ERROR, "(vvdec ret=%d)\n", dr);
        if (dr == VVDEC_OK && f)
            vvdec_frame_unref(s->dec_ctx, f);
    } else {
        av_log(avctx, AV_LOG_ERROR, "extradata conversion produced nothing\n");
    }
    av_free(out);
}

static void libvvdec_flush(AVCodecContext *avctx)
{
    VVDecContext *s = avctx->priv_data;
    vvdecFrame *dec_frame = NULL;

    s->buf_size = 0;
    while (vvdec_flush(s->dec_ctx, &dec_frame) == VVDEC_OK && dec_frame) {
        vvdec_frame_unref(s->dec_ctx, dec_frame);
        dec_frame = NULL;
    }
}

static av_cold int libvvdec_decode_close(AVCodecContext *avctx)
{
    VVDecContext *s = avctx->priv_data;

    if (s->dec_ctx) {
        vvdec_decoder_close(s->dec_ctx);
        s->dec_ctx = NULL;
    }
    av_freep(&s->buf);
    s->buf_size = s->buf_cap = 0;

    return 0;
}

static av_cold int libvvdec_decode_init(AVCodecContext *avctx)
{
    VVDecContext *s = avctx->priv_data;
    vvdecParams *params;

    params = vvdec_params_alloc();
    if (!params)
        return AVERROR(ENOMEM);
    vvdec_params_default(params);
    params->logLevel = VVDEC_ERROR;

    s->dec_ctx = vvdec_decoder_open(params);
    vvdec_params_free(params);
    if (!s->dec_ctx) {
        av_log(avctx, AV_LOG_ERROR, "vvdec_decoder_open() failed\n");
        return AVERROR_EXTERNAL;
    }

    /* Matroska keeps the VVC parameter sets in the CodecPrivate
     * (extradata), length-prefixed; convert to annex-B and feed vvdec
     * before any VCL NAL. */
    if (avctx->extradata && avctx->extradata_size > 0)
        vvc_feed_extradata(avctx, s);

    return 0;
}

static enum AVPixelFormat vvdec_pix_fmt(const vvdecFrame *dec_frame)
{
    switch (dec_frame->colorFormat) {
    case VVDEC_CF_YUV400_PLANAR:
        if (dec_frame->bitDepth <= 8)
            return AV_PIX_FMT_GRAY8;
        if (dec_frame->bitDepth == 9)
            return AV_PIX_FMT_GRAY9;
        if (dec_frame->bitDepth == 10)
            return AV_PIX_FMT_GRAY10;
        if (dec_frame->bitDepth == 12)
            return AV_PIX_FMT_GRAY12;
        if (dec_frame->bitDepth == 14)
            return AV_PIX_FMT_GRAY14;
        break;
    case VVDEC_CF_YUV420_PLANAR:
        if (dec_frame->bitDepth <= 8)
            return AV_PIX_FMT_YUV420P;
        if (dec_frame->bitDepth == 9)
            return AV_PIX_FMT_YUV420P9;
        if (dec_frame->bitDepth == 10)
            return AV_PIX_FMT_YUV420P10;
        if (dec_frame->bitDepth == 12)
            return AV_PIX_FMT_YUV420P12;
        if (dec_frame->bitDepth == 14)
            return AV_PIX_FMT_YUV420P14;
        break;
    case VVDEC_CF_YUV422_PLANAR:
        if (dec_frame->bitDepth <= 8)
            return AV_PIX_FMT_YUV422P;
        if (dec_frame->bitDepth == 9)
            return AV_PIX_FMT_YUV422P9;
        if (dec_frame->bitDepth == 10)
            return AV_PIX_FMT_YUV422P10;
        if (dec_frame->bitDepth == 12)
            return AV_PIX_FMT_YUV422P12;
        if (dec_frame->bitDepth == 14)
            return AV_PIX_FMT_YUV422P14;
        break;
    case VVDEC_CF_YUV444_PLANAR:
        if (dec_frame->bitDepth <= 8)
            return AV_PIX_FMT_YUV444P;
        if (dec_frame->bitDepth == 9)
            return AV_PIX_FMT_YUV444P9;
        if (dec_frame->bitDepth == 10)
            return AV_PIX_FMT_YUV444P10;
        if (dec_frame->bitDepth == 12)
            return AV_PIX_FMT_YUV444P12;
        if (dec_frame->bitDepth == 14)
            return AV_PIX_FMT_YUV444P14;
        break;
    }

    return AV_PIX_FMT_NONE;
}

static int libvvdec_copy_frame(AVCodecContext *avctx, AVFrame *frame,
                               const vvdecFrame *dec_frame)
{
    const AVPixFmtDescriptor *desc;
    enum AVPixelFormat pix_fmt;
    int ret, i;

    pix_fmt = vvdec_pix_fmt(dec_frame);
    if (pix_fmt == AV_PIX_FMT_NONE) {
        av_log(avctx, AV_LOG_ERROR, "Unsupported output format.\n");
        return AVERROR(EINVAL);
    }
    desc = av_pix_fmt_desc_get(pix_fmt);

    frame->format = pix_fmt;
    frame->width  = dec_frame->width;
    frame->height = dec_frame->height;

    /* Public API only: the internal ff_get_buffer is not exported by
     * the host's libavcodec, so a module referencing it fails to load. */
    ret = av_frame_get_buffer(frame, 0);
    if (ret < 0)
        return ret;

    for (i = 0; i < FFMIN(dec_frame->numPlanes, (uint32_t)3); i++) {
        const vvdecPlane *plane = &dec_frame->planes[i];
        int h       = plane->height;
        int w       = plane->width;
        int psize   = plane->bytesPerSample;
        int src_str = plane->stride;
        int dst_str = frame->linesize[i];
        const uint8_t *src = plane->ptr;
        uint8_t *dst = frame->data[i];
        int y;

        for (y = 0; y < h; y++)
            memcpy(dst + y * dst_str, src + y * src_str, (size_t)w * psize);
    }

    return 0;
}

static int libvvdec_decode_frame(AVCodecContext *avctx, AVFrame *frame,
                                 int *got_frame, AVPacket *pkt)
{
    VVDecContext *s = avctx->priv_data;
    vvdecAccessUnit au;
    vvdecFrame *dec_frame = NULL;
    int ret;

    memset(&au, 0, sizeof(au));
    if (pkt->size) {
        int type = vvc_nal_type(pkt->data, pkt->size);
        int ret2 = vvc_buf_append(s, pkt->data, pkt->size);
        if (ret2 < 0)
            return ret2;
        if (vvc_nal_is_vcl(type)) {
            au.payload         = s->buf;
            au.payloadUsedSize = s->buf_size;
            ret = vvdec_decode(s->dec_ctx, &au, &dec_frame);
            s->buf_size = 0;
        } else {
            return 0;
        }
    } else {
        ret = vvdec_flush(s->dec_ctx, &dec_frame);
    }

    if (ret != VVDEC_OK && ret != VVDEC_TRY_AGAIN && ret != VVDEC_EOF) {
        av_log(avctx, AV_LOG_ERROR,
               "Error decoding VVC NAL unit (vvdec ret=%d, %zu bytes)\n",
               ret, pkt->size);
        av_log(avctx, AV_LOG_ERROR,
               "input head: %02x %02x %02x %02x %02x %02x %02x %02x "
               "%02x %02x %02x %02x %02x %02x %02x %02x\n",
               pkt->data[0], pkt->data[1], pkt->data[2], pkt->data[3],
               pkt->data[4], pkt->data[5], pkt->data[6], pkt->data[7],
               pkt->data[8], pkt->data[9], pkt->data[10], pkt->data[11],
               pkt->data[12], pkt->data[13], pkt->data[14], pkt->data[15]);
        return AVERROR_EXTERNAL;
    }

    if (!dec_frame)
        return 0;

    ret = libvvdec_copy_frame(avctx, frame, dec_frame);
    if (ret < 0) {
        vvdec_frame_unref(s->dec_ctx, dec_frame);
        return ret;
    }

    if (dec_frame->ctsValid)
        frame->pts = dec_frame->cts;

    *got_frame = 1;
    vvdec_frame_unref(s->dec_ctx, dec_frame);

    return pkt->size;
}

const FFCodec ff_vvc_decoder = {
    .p.name         = "vvc",
    .p.long_name    = "VVC (Versatile Video Coding) via libvvdec",
    .p.type         = AVMEDIA_TYPE_VIDEO,
    .p.id           = AV_CODEC_ID_VVC,
    .priv_data_size = sizeof(VVDecContext),
    .init           = libvvdec_decode_init,
    .cb.decode      = libvvdec_decode_frame,
    .cb_type        = FF_CODEC_CB_TYPE_DECODE,
    .flush          = libvvdec_flush,
    .close          = libvvdec_decode_close,
    .bsfs           = "vvc_mp4toannexb",
    .p.capabilities = AV_CODEC_CAP_DR1 | AV_CODEC_CAP_DELAY | AV_CODEC_CAP_FRAME_THREADS,
    .caps_internal  = FF_CODEC_CAP_INIT_CLEANUP,
};
