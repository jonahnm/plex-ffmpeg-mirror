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
        return (pkt[4] & 0x03) << 3 | (pkt[5] >> 5);
    if (size >= 5 && pkt[0] == 0 && pkt[1] == 0 && pkt[2] == 1)
        return (pkt[3] & 0x03) << 3 | (pkt[4] >> 5);
    return -1;
}

/* Non-VCL NALs (OPI, APS, SEI, VPS, SPS, PPS) must be fed to the
 * decoder together with the following VCL access unit; on their own
 * vvdec rejects them (VVDEC_ERR_DEC_INPUT). */
static int vvc_nal_is_vcl(int type)
{
    switch (type) {
    case 6:   /* OPI */
    case 12:  /* OPI (alternate id) */
    case 13:  /* DCI */
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
 * with a PTL header) into annex-B and feed it to vvdec. Field layout
 * mirrors FFmpeg's cbs_h2645.c VVCC parsing exactly. */
static void vvc_feed_extradata(AVCodecContext *avctx, VVDecContext *s)
{
    const uint8_t *e = avctx->extradata;
    int esize = avctx->extradata_size;
    uint8_t *out = NULL;
    int out_size = 0;
    int pos = 0, b, i, j;

    if (esize < 8)
        return;

    b = e[pos++];                       /* length_size + ptl_present_flag */
    if (!(b & 1))
        return;                         /* no PTL; not the expected format */

    b = (e[pos] << 8) | e[pos + 1];     /* ols_idx, num_sublayers, ... */
    pos += 2;
    {
        int num_sublayers = (b >> 4) & 7;
        int ncbi, flags, nsubp;

        pos += 1;                       /* bit_depth_minus8 byte */
        ncbi = e[pos] & 0x3f;
        pos += 1;
        pos += 2 + ncbi;                /* profile/tier+level, constraint */
        if (num_sublayers > 1) {
            flags = e[pos++];
            for (i = num_sublayers - 2; i >= 0; i--) {
                if ((flags >> (7 - (num_sublayers - 2 - i))) & 1)
                    pos += 1;           /* sublayer_level_idc */
            }
        }
        nsubp = e[pos++];
        pos += nsubp * 4 + 6;           /* sub-profiles, size/frame rate */
    }

    {
        int num_arrays = e[pos++];
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
            for (j = 0; j < cnt && pos + 2 <= esize; j++) {
                int len = (e[pos] << 8) | e[pos + 1];
                uint8_t *nb;
                pos += 2;
                if (pos + len > esize)
                    break;
                nb = av_realloc(out, out_size + len + 4 + 64);
                if (!nb)
                    goto done;
                out = nb;
                out[out_size]     = 0;   /* 00 00 00 01 start code */
                out[out_size + 1] = 0;
                out[out_size + 2] = 0;
                out[out_size + 3] = 1;
                memcpy(out + out_size + 4, e + pos, len);
                out_size += 4 + len;
                pos += len;
            }
        }
    }

done:
    if (out && out_size > 0) {
        /* Append the parameter sets to the accumulation buffer; they
         * are decoded together with the first VCL access unit. */
        if (vvc_buf_append(s, out, out_size) < 0)
            av_log(avctx, AV_LOG_ERROR, "failed to buffer parameter sets\n");
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
    enum AVPixelFormat pix_fmt;
    int ret, i;

    pix_fmt = vvdec_pix_fmt(dec_frame);
    if (pix_fmt == AV_PIX_FMT_NONE) {
        av_log(avctx, AV_LOG_ERROR, "Unsupported output format.\n");
        return AVERROR(EINVAL);
    }

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
            au.payloadSize     = s->buf_cap;
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
               "Error decoding VVC NAL unit (vvdec ret=%d)\n", ret);
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
