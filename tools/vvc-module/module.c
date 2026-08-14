/*
 * libvvc_decoder.so - Plex external VVC decoder module.
 *
 * Plex's server only considers a video codec "convertible" when a decoder
 * module named lib<codec>_decoder.so exists in its Codecs directory, and
 * the transcoder loads that module to decode. The interface was
 * reverse-engineered from Plex's own libh264_decoder.so:
 *
 *   dlopen("libvvc_decoder.so") -> dlsym("av_init_library")
 *   av_init_library(info, log_level):
 *     info->+0x10  saved by the module (unused here)
 *     info->+0x18  callback returning the host's FFmpeg version string
 *     info->+0x20  callback returning the interface magic (0x3c1f66)
 *     info->+0x28  callback receiving the module's codec descriptor
 *
 * This module embeds the mirrored FFmpeg libavcodec (built with the VVC
 * decoder) and hands the host the ff_vvc_decoder descriptor.
 */
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "libavcodec/codec_internal.h"

/* From the mirrored libavcodec build. */
extern const FFCodec ff_vvc_decoder;

/* Plex's transcoder build hash; the host's version callback must match. */
#define PLEX_FFMPEG_VERSION "4987be2-8e3461d4c82193c06183af3f"
#define PLEX_INTERFACE_MAGIC 0x3c1f66

__asm__(".symver av_init_library,av_init_library@@vvc_decoder_1");

struct plex_library_info {
    void       *p10;      /* saved by the module */
    const char *(*version_cb)(void);
    int        (*magic_cb)(void);
    void       (*table_cb)(void *);
};

int av_init_library(struct plex_library_info *info, int log_level)
{
    static int loaded = 0;
    const char *host_version;

    if (loaded)
        return -1;
    loaded = 1;

    if (!info || !info->version_cb || !info->magic_cb)
        return -1;
    if (info->magic_cb() != PLEX_INTERFACE_MAGIC)
        return -1;
    host_version = info->version_cb();
    if (!host_version || strcmp(host_version, PLEX_FFMPEG_VERSION) != 0)
        return -1;
    if (info->table_cb)
        info->table_cb((void *)&ff_vvc_decoder);
    return 0;
}
