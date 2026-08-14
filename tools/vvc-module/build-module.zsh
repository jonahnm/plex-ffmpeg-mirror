#!/bin/zsh
# Build libvvc_decoder.so, Plex's external VVC decoder module.
#
# The module exports the av_init_library interface Plex's transcoder
# expects (see module.c) and registers a VVC decoder backed by the
# Fraunhofer reference decoder (libvvdec), built from source with the
# musl toolchain. It does NOT embed libavcodec: the host transcoder
# provides all ffmpeg functionality, the module only adds the decoder.
#
# Usage: build-module.zsh [SRC_DIR]
#   SRC_DIR defaults to <repo>/plex-ffmpeg-source/PlexTranscoder (the
#   mirrored ffmpeg tree, used for headers only).

typeset -gr SCRIPT_PATH="${0:A:h}"
typeset -gr REPO_PATH="${SCRIPT_PATH:h:h}"
typeset -gr SRC_DIR="${1:-${REPO_PATH}/plex-ffmpeg-source/PlexTranscoder}"
typeset -gr OUT="${REPO_PATH}/run/libvvc_decoder.so"
typeset -gr VVDEC_SRC="${REPO_PATH}/run/vvdec-src"
typeset -gr VVDEC_BUILD="${REPO_PATH}/run/vvdec-build"
typeset -gr VVDEC_PREFIX="${REPO_PATH}/run/vvdec-prefix"
typeset -gr VVDEC_TAG="v2.1.0"

die() {
    echo "FATAL: $1" >&2
    exit 1
}

[[ -d "${SRC_DIR}/libavcodec" ]] \
    || die "ffmpeg source not found in $SRC_DIR"

command -v x86_64-linux-musl-gcc > /dev/null 2>&1 \
    || die "musl toolchain missing (apt-get install musl-tools)"
command -v cmake > /dev/null 2>&1 \
    || die "cmake missing (apt-get install cmake)"

mkdir -p "${REPO_PATH}/run"

# 1. Build libvvdec (Fraunhofer VVC reference decoder) with musl.
#    vvdec is C++; prefer the musl C++ wrapper, fall back to the system
#    g++ (glibc) with an explicit archiver when it is unavailable.
MUSL_CXX="x86_64-linux-musl-g++"
EXTRA_LIBS=()
if command -v "$MUSL_CXX" > /dev/null 2>&1; then
    echo "== libvvdec C++ compiler: $MUSL_CXX =="
else
    echo "WARNING: $MUSL_CXX not found; using system g++ (glibc objects)."
    MUSL_CXX="g++"
    EXTRA_LIBS=(-lstdc++)
fi
if [[ -d "$VVDEC_SRC" && -z "$(ls -A "$VVDEC_SRC" 2> /dev/null)" ]]; then
    echo "== removing empty vvdec checkout =="
    rmdir "$VVDEC_SRC"
fi
if [[ ! -d "$VVDEC_SRC" ]]; then
    echo "== cloning libvvdec ${VVDEC_TAG} =="
    git clone --depth 1 --branch "$VVDEC_TAG" \
        https://github.com/fraunhoferhhi/vvdec.git "$VVDEC_SRC" \
        || die "failed to clone vvdec"
fi
if [[ ! -f "${VVDEC_PREFIX}/lib/libvvdec.a" ]]; then
    echo "== building libvvdec (musl, static) =="
    # vvdec writes its static archive into the source tree's lib/
    # directory, which its CMake never creates.
    mkdir -p "${VVDEC_SRC}/lib/release-static"
    # vvdec overrides the archive rule; pin every archiver/ranlib
    # variable so the link step does not end up with *-NOTFOUND
    # commands.
    rm -rf "$VVDEC_BUILD"
    cmake -S "$VVDEC_SRC" -B "$VVDEC_BUILD" \
        -DCMAKE_C_COMPILER=x86_64-linux-musl-gcc \
        -DCMAKE_CXX_COMPILER="$MUSL_CXX" \
        -DCMAKE_AR=/usr/bin/ar \
        -DCMAKE_C_COMPILER_AR=/usr/bin/ar \
        -DCMAKE_CXX_COMPILER_AR=/usr/bin/ar \
        -DCMAKE_RANLIB=/usr/bin/ranlib \
        -DCMAKE_C_COMPILER_RANLIB=/usr/bin/ranlib \
        -DCMAKE_CXX_COMPILER_RANLIB=/usr/bin/ranlib \
        -DCMAKE_BUILD_TYPE=Release \
        -DBUILD_SHARED_LIBS=OFF \
        -DCMAKE_INSTALL_PREFIX="$VVDEC_PREFIX" \
        || die "vvdec cmake configure failed"
    cmake --build "$VVDEC_BUILD" -j"$(nproc 2> /dev/null || echo 2)" \
        || die "vvdec build failed"
    cmake --install "$VVDEC_BUILD" \
        || die "vvdec install failed"
fi
echo "libvvdec: ${VVDEC_PREFIX}/lib/libvvdec.a"

# 2. Build the module: wrapper + module glue + libvvdec, no libavcodec.
echo "== building libvvc_decoder.so =="
x86_64-linux-musl-gcc -shared -fPIC -O2 -o "$OUT" \
    -I"${SRC_DIR}" \
    -I"${VVDEC_PREFIX}/include/vvdec" \
    -Wl,--version-script="${SCRIPT_PATH}/version.script" \
    "${SCRIPT_PATH}/module.c" \
    "${SCRIPT_PATH}/libvvdec_codec.c" \
    "${VVDEC_PREFIX}/lib/libvvdec.a" \
    -lm -lpthread "${EXTRA_LIBS[@]}" \
    || die "module build failed"

nm -D "$OUT" | grep -q "T av_init_library" \
    || die "av_init_library not exported"
echo "Built: $OUT"
ls -la "$OUT"
