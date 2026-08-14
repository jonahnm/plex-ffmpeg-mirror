#!/bin/zsh
# Build libvvc_decoder.so, Plex's external VVC decoder module.
#
# The module embeds the mirrored FFmpeg libavcodec (with the VVC decoder)
# as a static library and exposes the av_init_library interface Plex's
# transcoder expects (see module.c). It must be built with the same musl
# toolchain as the rest of the deployment, against the static libraries
# produced by the deploy build.
#
# Usage: build-module.zsh [SRC_DIR]
#   SRC_DIR defaults to <repo>/plex-ffmpeg-source/PlexTranscoder and must
#   contain a finished shared build (libavcodec.a, libavutil.a).

typeset -gr SCRIPT_PATH="${0:A:h}"
typeset -gr REPO_PATH="${SCRIPT_PATH:h}"
typeset -gr SRC_DIR="${1:-${REPO_PATH}/plex-ffmpeg-source/PlexTranscoder}"
typeset -gr OUT="${REPO_PATH}/run/libvvc_decoder.so"

die() {
    echo "FATAL: $1" >&2
    exit 1
}

[[ -f "${SRC_DIR}/libavcodec/libavcodec.a" && -f "${SRC_DIR}/libavutil/libavutil.a" ]] \
    || die "static libraries not found in $SRC_DIR (run deploy-plex-vvc.zsh first)"

command -v x86_64-linux-musl-gcc > /dev/null 2>&1 \
    || die "musl toolchain missing (apt-get install musl-tools)"

mkdir -p "${REPO_PATH}/run"

echo "== building libvvc_decoder.so =="
x86_64-linux-musl-gcc -shared -fPIC -O2 -o "$OUT" \
    "${SCRIPT_PATH}/module.c" \
    "${SRC_DIR}/libavcodec/libavcodec.a" \
    "${SRC_DIR}/libavutil/libavutil.a" \
    -lm -lz -lpthread \
    || die "module build failed"

nm -D "$OUT" | grep -q "T av_init_library" \
    || die "av_init_library not exported"
echo "Built: $OUT"
ls -la "$OUT"
