/*
 * libvvdec-backed VVC decoder for Plex's external decoder module.
 *
 * Adapted from FFmpeg's libavcodec/libvvdec.c. Registers an FFCodec
 * named "vvc" that decodes via the Fraunhofer reference decoder
 * (libvvdec) instead of the bundled experimental native decoder, which
 * produces artifacts on real-world streams.
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
#include "libavutil/imgutils.h"
#include "libavutil/opt.h"
#include "libavutil/pixdesc.h"

#include "vvdec.h"

typedef struct VVDecContext {
    vvdecDecHandle dec_ctx;
    int threads;
} VVDecContext;

static void libvvdec_flush(AVCodecContext *avctx)
{
    VVDecContext *s = avctx->priv_data;

    vvdecFlush(s->dec_ctx);
}

static av_cold int libvvdec_decode_close(AVCodecContext *avctx)
{
    VVDecContext *s = avctx->priv_data;

    if (s->dec_ctx) {
        vvdecDecClose(s->dec_ctx);
        s->dec_ctx = NULL;
    }

    return 0;
}

static av_cold int libvvdec_decode_init(AVCodecContext *avctx)
{
    VVDecContext *s = avctx->priv_data;
    vvdec_params_t params = { 0 };
    int ret;

    ret = vvdecCreate(&s->dec_ctx);
    if (ret != VVDEC_OK) {
        av_log(avctx, AV_LOG_ERROR, "vvdecCreate() failed\n");
        return AVERROR_EXTERNAL;
    }

    params.threads         = s->threads;
    params.frameProcessing = 1;
    params.errorHandling   = 1;

    ret = vvdecInit(s->dec_ctx, &params);
    if (ret != VVDEC_OK) {
        av_log(avctx, AV_LOG_ERROR, "vvdecInit() failed\n");
        libvvdec_decode_close(avctx);
        return AVERROR_EXTERNAL;
    }

    return 0;
}

static enum AVPixelFormat vvdec_pix_fmt(const vvdecAccessUnit *au)
{
    switch (au->chromaFormat) {
    case VVDEC_CHROMA_400:
        if (au->bitDepth <= 8)
            return AV_PIX_FMT_GRAY8;
        if (au->bitDepth == 9)
            return AV_PIX_FMT_GRAY9;
        if (au->bitDepth == 10)
            return AV_PIX_FMT_GRAY10;
        if (au->bitDepth == 12)
            return AV_PIX_FMT_GRAY12;
        if (au->bitDepth == 14)
            return AV_PIX_FMT_GRAY14;
        break;
    case VVDEC_CHROMA_420:
        if (au->bitDepth <= 8)
            return AV_PIX_FMT_YUV420P;
        if (au->bitDepth == 9)
            return AV_PIX_FMT_YUV420P9;
        if (au->bitDepth == 10)
            return AV_PIX_FMT_YUV420P10;
        if (au->bitDepth == 12)
            return AV_PIX_FMT_YUV420P12;
        if (au->bitDepth == 14)
            return AV_PIX_FMT_YUV420P14;
        break;
    case VVDEC_CHROMA_422:
        if (au->bitDepth <= 8)
            return AV_PIX_FMT_YUV422P;
        if (au->bitDepth == 9)
            return AV_PIX_FMT_YUV422P9;
        if (au->bitDepth == 10)
            return AV_PIX_FMT_YUV422P10;
        if (au->bitDepth == 12)
            return AV_PIX_FMT_YUV422P12;
        if (au->bitDepth == 14)
            return AV_PIX_FMT_YUV422P14;
        break;
    case VVDEC_CHROMA_444:
        if (au->bitDepth <= 8)
            return AV_PIX_FMT_YUV444P;
        if (au->bitDepth == 9)
            return AV_PIX_FMT_YUV444P9;
        if (au->bitDepth == 10)
            return AV_PIX_FMT_YUV444P10;
        if (au->bitDepth == 12)
            return AV_PIX_FMT_YUV444P12;
        if (au->bitDepth == 14)
            return AV_PIX_FMT_YUV444P14;
        break;
    }

    return AV_PIX_FMT_NONE;
}

static int libvvdec_copy_frame(AVCodecContext *avctx, AVFrame *frame,
                               const vvdecAccessUnit *au)
{
    const AVPixFmtDescriptor *desc;
    enum AVPixelFormat pix_fmt;
    int ret, i;

    pix_fmt = vvdec_pix_fmt(au);
    if (pix_fmt == AV_PIX_FMT_NONE) {
        av_log(avctx, AV_LOG_ERROR, "Unsupported output format.\n");
        return AVERROR(EINVAL);
    }
    desc = av_pix_fmt_desc_get(pix_fmt);

    frame->format = pix_fmt;
    frame->width  = au->width;
    frame->height = au->height;

    ret = ff_get_buffer(avctx, frame, 0);
    if (ret < 0)
        return ret;

    for (i = 0; i < au->planes; i++) {
        int h = frame->height;
        int plane_size;

        if (i)
            h = AV_CEIL_RSHIFT(h, desc->log2_chroma_h);
        plane_size = frame->linesize[i] * h;

        av_assert0(plane_size <= frame->buf[i]->size);
        memcpy(frame->data[i], au->picture + au->cts[i], plane_size);
    }

    return 0;
}

static int libvvdec_decode_frame(AVCodecContext *avctx, AVFrame *frame,
                                 int *got_frame, AVPacket *pkt)
{
    VVDecContext *s = avctx->priv_data;
    vvdecAccessUnit au;
    int ret;

    if (pkt->size) {
        ret = vvdecDecode(s->dec_ctx, pkt->data, pkt->size, 0, NULL);
        if (ret != VVDEC_OK) {
            av_log(avctx, AV_LOG_ERROR, "Error decoding VVC NAL unit.\n");
            return AVERROR_EXTERNAL;
        }
    } else {
        vvdecFlush(s->dec_ctx);
    }

    ret = vvdecNextFrame(s->dec_ctx, &au);
    if (ret != VVDEC_OK) {
        if (ret == VVDEC_EOF || ret == VVDEC_TRY_AGAIN)
            return 0;
        return AVERROR_EXTERNAL;
    }

    ret = libvvdec_copy_frame(avctx, frame, &au);
    if (ret < 0)
        return ret;

    frame->pts = au->pts;

    *got_frame = 1;
    vvdecAccessUnitFree(s->dec_ctx, &au);

    return pkt->size;
}

const FFCodec ff_vvc_decoder = {
    .p.name         = "vvc",
    .p.long_name    = NULL_IF_CONFIG_SMALL("VVC (Versatile Video Coding) via libvvdec"),
    .p.type         = AVMEDIA_TYPE_VIDEO,
    .p.id           = AV_CODEC_ID_VVC,
    .priv_data_size = sizeof(VVDecContext),
    .init           = libvvdec_decode_init,
    .decode         = libvvdec_decode_frame,
    .flush          = libvvdec_flush,
    .close          = libvvdec_decode_close,
    .p.capabilities = AV_CODEC_CAP_DR1 | AV_CODEC_CAP_DELAY | AV_CODEC_CAP_FRAME_THREADS,
    .caps_internal  = FF_CODEC_CAP_INIT_CLEANUP,
};
