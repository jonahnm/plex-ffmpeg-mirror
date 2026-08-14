/*
 * libvvc_decoder.so - Plex external VVC decoder module.
 *
 * Plex's transcoder discovers codec modules via FFMPEG_EXTERNAL_LIBS and
 * calls dlopen()d av_init_library(info, log_level). The info struct
 * layout was derived from the official libh264_decoder.so:
 *
 *   +0x00  unknown, unused
 *   +0x08  unknown, unused
 *   +0x10  saved by the module
 *   +0x18  version_cb() -> host ffmpeg version string
 *   +0x20  magic_cb()   -> interface magic (0x3c1f66)
 *   +0x28  table_cb()   -> registers the codec descriptor
 *
 * The module registers an FFCodec named "vvc" that decodes via the
 * Fraunhofer reference decoder (libvvdec) instead of the bundled
 * experimental native decoder, which produces artifacts on real-world
 * streams.
 */
#include <stdint.h>
#include <stdio.h>

#include "libavcodec/codec_internal.h"

/* From the mirrored libavcodec build. */
extern const FFCodec ff_vvc_decoder;

/* Plex's transcoder interface magic (see libh264_decoder.so). The
 * version gate is deliberately not enforced: the module is built to
 * match any host of this interface generation. */
#define PLEX_INTERFACE_MAGIC 0x3c1f66

struct plex_library_info {
    void       *p00;                       /* unknown, unused */
    void       *p08;                       /* unknown, unused */
    void       *p10;                       /* saved by the module */
    const char *(*version_cb)(void);       /* +0x18 */
    int        (*magic_cb)(void);          /* +0x20 */
    void       (*table_cb)(const void *);  /* +0x28 */
};

int av_init_library(struct plex_library_info *info, int log_level)
{
    static int loaded = 0;

    if (loaded)
        return -1;
    loaded = 1;

    if (!info || !info->magic_cb)
        return -1;
    if (info->magic_cb() != PLEX_INTERFACE_MAGIC)
        return -1;
    if (info->version_cb)
        fprintf(stderr, "libvvc_decoder: host version '%s'\n",
                info->version_cb());
    if (info->table_cb)
        info->table_cb(&ff_vvc_decoder);
    return 0;
}
