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
} VVDecContext;

static void libvvdec_flush(AVCodecContext *avctx)
{
    VVDecContext *s = avctx->priv_data;
    vvdecFrame *dec_frame = NULL;

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
    params->logLevel      = VVDEC_SILENT;
    params->removePadding = true;

    s->dec_ctx = vvdec_decoder_open(params);
    vvdec_params_free(params);
    if (!s->dec_ctx) {
        av_log(avctx, AV_LOG_ERROR, "vvdec_decoder_open() failed\n");
        return AVERROR_EXTERNAL;
    }

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
        au.payload        = pkt->data;
        au.payloadUsedSize = pkt->size;
        ret = vvdec_decode(s->dec_ctx, &au, &dec_frame);
    } else {
        ret = vvdec_flush(s->dec_ctx, &dec_frame);
    }

    if (ret != VVDEC_OK && ret != VVDEC_TRY_AGAIN && ret != VVDEC_EOF) {
        av_log(avctx, AV_LOG_ERROR, "Error decoding VVC NAL unit.\n");
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
