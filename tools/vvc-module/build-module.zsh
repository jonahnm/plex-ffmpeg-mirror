#!/bin/zsh
# Build libvvc_decoder.so, Plex's external VVC decoder module.
#
# The module exports the av_init_library interface Plex's transcoder
# expects (see module.c) and registers a VVC decoder backed by the
# Fraunhofer reference decoder (libvvdec), built from source with a musl
# toolchain. It does NOT embed libavcodec: the host transcoder provides
# all ffmpeg functionality, the module only adds the decoder.
#
# Plex's transcoder is a musl binary, so the module must be musl-clean:
# no glibc-only symbols, and the C++ runtime linked statically (there is
# no libstdc++.so in the musl runtime). vvdec is C++, so a musl C++
# toolchain is required; the musl.cc prebuilt cross toolchain is
# downloaded when the system lacks x86_64-linux-musl-g++.
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
typeset -gr MUSL_CROSS="${REPO_PATH}/run/musl-cross"

die() {
    echo "FATAL: $1" >&2
    exit 1
}

[[ -d "${SRC_DIR}/libavcodec" ]] \
    || die "ffmpeg source not found in $SRC_DIR"
command -v cmake > /dev/null 2>&1 \
    || die "cmake missing (apt-get install cmake)"

mkdir -p "${REPO_PATH}/run"

# 1. Ensure a musl C++ toolchain. Prefer the system x86_64-linux-musl-g++
#    if present, otherwise download the musl.cc prebuilt cross toolchain.
MUSL_CXX="x86_64-linux-musl-g++"
if ! command -v "$MUSL_CXX" > /dev/null 2>&1; then
    if [[ -x "${MUSL_CROSS}/bin/x86_64-linux-musl-g++" ]]; then
        MUSL_CXX="${MUSL_CROSS}/bin/x86_64-linux-musl-g++"
    else
        echo "== downloading musl-cross toolchain from musl.cc =="
        curl -L --fail -o "${REPO_PATH}/run/musl-cross.tgz" \
            https://musl.cc/x86_64-linux-musl-cross.tgz \
            || die "failed to download musl-cross toolchain"
        mkdir -p "$MUSL_CROSS"
        tar -xzf "${REPO_PATH}/run/musl-cross.tgz" -C "$MUSL_CROSS" --strip-components=1 \
            || die "failed to extract musl-cross toolchain"
        MUSL_CXX="${MUSL_CROSS}/bin/x86_64-linux-musl-g++"
    fi
fi
MUSL_CC="${MUSL_CXX%g++}gcc"
command -v "$MUSL_CC" > /dev/null 2>&1 || MUSL_CC="${MUSL_CXX:h}/x86_64-linux-musl-gcc"
[[ -x "$MUSL_CC" || -n "$(command -v "$MUSL_CC" 2> /dev/null)" ]] \
    || die "musl C compiler not found ($MUSL_CC)"
echo "== musl C++: $MUSL_CXX =="

# The musl.cc gcc driver emits -fno-fat-lto-objects unconditionally for
# C++ (builtin specs), which its cc1plus rejects (no LTO plugin). Wrap
# the compiler to filter the flag.
if [[ "$MUSL_CXX" == "${MUSL_CROSS}/bin/"* ]]; then
    WRAP="${REPO_PATH}/run/musl-gxx-wrap"
    if [[ ! -x "$WRAP" ]]; then
        cat > "$WRAP" <<EOF
#!/bin/zsh
args=()
for a in "\$@"; do
    [[ "\$a" == "-fno-fat-lto-objects" || "\$a" == "-flto" ]] && continue
    args+=("\$a")
done
exec "$MUSL_CXX" "\${args[@]}"
EOF
        chmod +x "$WRAP"
    fi
    MUSL_CXX="$WRAP"
fi

# 2. Build libvvdec (Fraunhofer VVC reference decoder) with musl.
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
    # vvdec's own CMakeLists appends -flto to the release flags, which
    # the musl.cc driver cannot handle (no linker plugin); strip it.
    grep -rl -- "-flto" "$VVDEC_SRC" --include=CMakeLists.txt 2> /dev/null \
        | xargs -r sed -i 's/-flto//g'
    # vvdec writes its static archive into the source tree's lib/
    # directory, which its CMake never creates.
    mkdir -p "${VVDEC_SRC}/lib/release-static"
    # vvdec overrides the archive rule; pin every archiver/ranlib
    # variable so the link step does not end up with *-NOTFOUND
    # commands.
    rm -rf "$VVDEC_BUILD"
    cmake -S "$VVDEC_SRC" -B "$VVDEC_BUILD" \
        -DCMAKE_C_COMPILER="$MUSL_CC" \
        -DCMAKE_CXX_COMPILER="$MUSL_CXX" \
        -DCMAKE_AR=/usr/bin/ar \
        -DCMAKE_C_COMPILER_AR=/usr/bin/ar \
        -DCMAKE_CXX_COMPILER_AR=/usr/bin/ar \
        -DCMAKE_RANLIB=/usr/bin/ranlib \
        -DCMAKE_C_COMPILER_RANLIB=/usr/bin/ranlib \
        -DCMAKE_CXX_COMPILER_RANLIB=/usr/bin/ranlib \
        -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_C_FLAGS_RELEASE="-O3 -fno-lto" \
        -DCMAKE_CXX_FLAGS_RELEASE="-O3 -fno-lto" \
        -DBUILD_SHARED_LIBS=OFF \
        -DCMAKE_INSTALL_PREFIX="$VVDEC_PREFIX" \
        || die "vvdec cmake configure failed"
    # Only the static library is needed; the vvdecapp demo binary fails
    # to link with the musl.cc toolchain (broken multilib -L path).
    cmake --build "$VVDEC_BUILD" --target vvdec -j"$(nproc 2> /dev/null || echo 2)" \
        || die "vvdec build failed"
    cmake --install "$VVDEC_BUILD" \
        || die "vvdec install failed"
fi
echo "libvvdec: ${VVDEC_PREFIX}/lib/libvvdec.a"

# 3. Build the module: wrapper + module glue + libvvdec, with the C++
#    runtime linked statically so the .so loads in the musl transcoder.
echo "== building libvvc_decoder.so =="
"$MUSL_CXX" -shared -fPIC -O2 -o "$OUT" \
    -x c \
    -I"${SRC_DIR}" \
    -I"${VVDEC_PREFIX}/include" \
    -I"${VVDEC_PREFIX}/include/vvdec" \
    -static-libstdc++ -static-libgcc \
    -Wl,--version-script="${SCRIPT_PATH}/version.script" \
    "${SCRIPT_PATH}/module.c" \
    "${SCRIPT_PATH}/libvvdec_codec.c" \
    "${VVDEC_PREFIX}/lib/libvvdec.a" \
    -lm -lpthread \
    || die "module build failed"

nm -D "$OUT" | grep -q "T av_init_library" \
    || die "av_init_library not exported"

# 4. Sanity: the module must load in the musl runtime - no glibc-only
#    NEEDED libs, no glibc-only undefined symbols.
if readelf -d "$OUT" 2> /dev/null | grep -qE "libc\.so\.6|libstdc\+\+\.so\.6"; then
    die "module depends on glibc libraries; musl toolchain did not take effect"
fi
if nm -D "$OUT" 2> /dev/null | grep -E " U " | grep -qE "_dl_find_object|__isoc23_|__libc_single_threaded|fopen64|fseeko64|ftello64"; then
    die "module has glibc-only undefined symbols; musl toolchain did not take effect"
fi
echo "module is musl-clean"
echo "Built: $OUT"
ls -la "$OUT"
