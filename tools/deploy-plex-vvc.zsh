#!/bin/zsh
# Deploy the VVC-enabled ffmpeg shared libraries into a Plex Media Server
# install, so Plex's scanner and transcoder recognize VVC (H.266) streams.
#
# Plex's "Plex Media Scanner" and "Plex Transcoder" link the shared ffmpeg
# libraries shipped in the PMS lib directory (libavcodec.so.60 etc.) and do
# all probing in-process. Replacing those libraries with builds from this
# repo makes Plex:
#   - detect VVC streams (the VVC codec is reported as "hevc", which Plex's
#     codec map already knows, instead of "NONE"),
#   - demux VVC from Matroska (V_MPEGI/ISO/VVC),
#   - decode VVC during transcoding.
#
# Usage: deploy-plex-vvc.zsh [PMS_LIB_DIR]
#   PMS_LIB_DIR defaults to /usr/lib/plexmediaserver/lib (Debian/Ubuntu).
#   Run as root on the Plex host, then restart Plex Media Server and
#   refresh the metadata of your VVC items.
#
# The original Plex libraries are kept as *.orig-vvc next to the originals,
# so this can be reverted by removing the deployed files and restoring the
# backups.

typeset -gr SCRIPT_PATH="${0:A:h}"
typeset -gr REPO_PATH="${SCRIPT_PATH:h}"
typeset -gr SRC_DIR="${REPO_PATH}/plex-ffmpeg-source/PlexTranscoder"
typeset -gr PMS_LIB="${1:-/usr/lib/plexmediaserver/lib}"

die() {
    echo "FATAL: $1" >&2
    exit 1
}

[[ "$(id -u)" == "0" ]] || die "run this script as root (sudo zsh tools/deploy-plex-vvc.zsh)"
[[ -d "$PMS_LIB" ]] || die "PMS library directory not found: $PMS_LIB (pass the right path as \$1)"
[[ -f "$PMS_LIB/libavcodec.so.60" && -f "$PMS_LIB/libavformat.so.60" ]] || \
    die "Expected Plex's ffmpeg libraries (libavcodec.so.60, libavformat.so.60) in $PMS_LIB"

# Plex's binaries are built against musl libc (their configure uses
# x86_64-linux-musl-clang), so the PMS process resolves symbols against
# musl's libc, not the system glibc. Libraries built with glibc headers
# reference symbols musl does not provide (fcntl64, __isoc23_*), so the
# whole build must use the musl toolchain.
typeset -gr MUSL_CC="x86_64-linux-musl-gcc"
if ! command -v "$MUSL_CC" > /dev/null 2>&1; then
    echo "== installing musl-tools =="
    apt-get install -y musl-tools || die "musl-tools is required (apt-get install musl-tools)"
fi

# ffmpeg's configure finds libx264 via pkg-config; make sure it exists.
if ! command -v pkg-config > /dev/null 2>&1; then
    echo "== installing pkg-config =="
    apt-get install -y pkg-config || die "pkg-config is required (apt-get install pkg-config)"
fi

# 1. Build x264 (software H.264 encoder) as a static library. Plex's own
#    transcoder build has no software video encoder (--disable-libx264, only
#    hardware encoders), so without this the server cannot transcode video
#    at all on a GPU-less host. Linking x264 statically into our
#    libavcodec.so.60 avoids any runtime library path issues.
typeset -gr X264_PREFIX="${REPO_PATH}/run/x264"
# --enable-pic is required: the static library is linked into the shared
# libavcodec.so.60, and a non-PIC archive cannot be.
if [[ ! -x "${X264_PREFIX}/bin/x264" || ! -f "${X264_PREFIX}/.deploy-stamp-musl" ]]; then
    echo "== building x264 (static, PIC, musl) =="
    make_dir() { mkdir -p "$1" || die "failed to create $1"; }
    make_dir "${REPO_PATH}/run"
    if [[ ! -d "${REPO_PATH}/run/x264-src" ]]; then
        git clone --depth 1 https://github.com/mirror/x264.git "${REPO_PATH}/run/x264-src" \
            || die "failed to clone x264"
    fi
    ( cd "${REPO_PATH}/run/x264-src" \
        && make distclean > /dev/null 2>&1 || true \
        && CC="$MUSL_CC" ./configure --disable-cli --enable-static --enable-pic \
            --host=x86_64-linux-musl --prefix="$X264_PREFIX" \
        && make -j"$(nproc 2> /dev/null || echo 2)" \
        && make install ) || die "x264 build failed"
    touch "${X264_PREFIX}/.deploy-stamp-musl"
fi
# x264's configure only honors the CC environment variable; verify the
# archive was really built against musl headers (no glibc-only symbols).
if nm "${X264_PREFIX}/lib/libx264.a" 2> /dev/null | grep -qE "__isoc23_|fopen64|fseeko64|ftello64"; then
    die "x264 was built against glibc headers; the musl toolchain did not take effect"
fi
echo "x264: musl build confirmed"
export PKG_CONFIG_PATH="${X264_PREFIX}/lib/pkgconfig${PKG_CONFIG_PATH:+:$PKG_CONFIG_PATH}"

# 2. Build libvvdec (Fraunhofer VVC reference decoder) with musl; it
#    replaces the bundled experimental native vvc decoder in the
#    transcoder libraries.
typeset -gr VVDEC_SRC="${REPO_PATH}/run/vvdec-src"
typeset -gr VVDEC_BUILD="${REPO_PATH}/run/vvdec-build"
typeset -gr VVDEC_PREFIX="${REPO_PATH}/run/vvdec-prefix"
typeset -gr VVDEC_TAG="v2.1.0"
typeset -gr MUSL_CROSS="${REPO_PATH}/run/musl-cross"
VVDEC_CXX="x86_64-linux-musl-g++"
if ! command -v "$VVDEC_CXX" > /dev/null 2>&1; then
    if [[ ! -x "${MUSL_CROSS}/bin/x86_64-linux-musl-g++" ]]; then
        echo "== downloading musl-cross toolchain from musl.cc =="
        curl -L --fail -o "${REPO_PATH}/run/musl-cross.tgz" \
            https://musl.cc/x86_64-linux-musl-cross.tgz \
            || die "failed to download musl-cross toolchain"
        mkdir -p "$MUSL_CROSS"
        tar -xzf "${REPO_PATH}/run/musl-cross.tgz" -C "$MUSL_CROSS" --strip-components=1 \
            || die "failed to extract musl-cross toolchain"
    fi
    VVDEC_CXX="${MUSL_CROSS}/bin/x86_64-linux-musl-g++"
fi
VVDEC_CC="${VVDEC_CXX%g++}gcc"
# The musl.cc gcc driver emits -fno-fat-lto-objects unconditionally for
# C++ (builtin specs), which its cc1plus rejects (no LTO plugin). Wrap
# the compiler to filter the flag.
WRAP="${REPO_PATH}/run/musl-gxx-wrap"
if [[ ! -x "$WRAP" ]]; then
    cat > "$WRAP" <<EOF
#!/bin/zsh
args=()
for a in "\$@"; do
    [[ "\$a" == "-fno-fat-lto-objects" || "\$a" == "-flto" ]] && continue
    args+=("\$a")
done
exec "$VVDEC_CXX" "\${args[@]}"
EOF
    chmod +x "$WRAP"
fi
VVDEC_CXX="$WRAP"

if [[ -d "$VVDEC_SRC" && -z "$(ls -A "$VVDEC_SRC" 2> /dev/null)" ]]; then
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
    grep -rl -- "-flto" "$VVDEC_SRC" --include=CMakeLists.txt 2> /dev/null \
        | xargs -r sed -i 's/-flto//g'
    mkdir -p "${VVDEC_SRC}/lib/release-static"
    rm -rf "$VVDEC_BUILD"
    cmake -S "$VVDEC_SRC" -B "$VVDEC_BUILD" \
        -DCMAKE_C_COMPILER="$VVDEC_CC" \
        -DCMAKE_CXX_COMPILER="$VVDEC_CXX" \
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
    cmake --build "$VVDEC_BUILD" --target vvdec -j"$(nproc 2> /dev/null || echo 2)" \
        || die "vvdec build failed"
    cmake --install "$VVDEC_BUILD" \
        || die "vvdec install failed"
fi
echo "libvvdec: ${VVDEC_PREFIX}/lib/libvvdec.a"

# 2b. Install the libvvdec-based vvc decoder into the mirrored source
#     and hook it into the build.
cp "${SCRIPT_PATH}/vvc-module/libvvdec_ffmpeg.c" "${SRC_DIR}/libavcodec/libvvdec.c"
if ! grep -q "libvvdec.o" "${SRC_DIR}/libavcodec/Makefile"; then
    echo "OBJS += libvvdec.o" >> "${SRC_DIR}/libavcodec/Makefile" \
        || die "failed to patch libavcodec/Makefile"
fi

# 2c. Build the mirrored source with shared libraries and libx264,
#     forcing a clean rebuild. The native vvc decoder is disabled; the
#     libvvdec-based one (registered post-configure) replaces it.
echo "== building shared ffmpeg libraries (this takes a few minutes) =="
zsh "${SCRIPT_PATH}/build-ffmpeg.zsh" "${SRC_DIR}" -c -- --enable-shared \
    --enable-gpl --enable-libx264 --enable-eae --cc="$MUSL_CC" \
    --disable-decoder=vvc \
    --extra-cflags="-I${VVDEC_PREFIX}/include/vvdec -I${VVDEC_PREFIX}/include" \
    --extra-ldflags="-L${VVDEC_PREFIX}/lib" \
    --extra-libs="-static-libstdc++ -lvvdec -lpthread" \
    || die "build failed"

# 3. Locate the freshly built libraries.
local avcodec libavcodec libformat
avcodec="$(ls "${SRC_DIR}/libavcodec/libavcodec.so."* 2> /dev/null | grep -v '\.so$' | head -1)"
libformat="$(ls "${SRC_DIR}/libavformat/libavformat.so."* 2> /dev/null | grep -v '\.so$' | head -1)"
[[ -n "$avcodec" && -n "$libformat" ]] || die "shared libraries not produced (check the build log)"
echo "Built: ${avcodec:t} (${libformat:t})"

# Sanity check: the deployed library must be a musl build (NEEDED libc.so,
# not glibc's libc.so.6) and contain the H.264 encoder.
if ! readelf -d "$avcodec" 2> /dev/null | grep -q '\[libc\.so\]'; then
    die "libavcodec is not a musl build (Plex's runtime cannot load it)"
fi
grep -q "libx264" "$avcodec" \
    || die "libx264 encoder not found in the build"
grep -q "eac3_eae" "$avcodec" \
    || die "EAE codecs not found in the build (Plex's -eae_prefix option needs them)"
echo "libx264 encoder: enabled"
echo "EAE codecs: enabled"

# 4. Pre-flight: the freshly built libraries must be able to resolve all
# their symbols against the host's libc (musl, since Plex ships musl
# binaries). ldd follows the ELF's interpreter, so it exercises the same
# loader PMS uses; grep for unresolved-symbol diagnostics rather than
# relying on exit codes, which differ between loaders.
check_lib() {
    local lib="$1" out
    out="$(LD_LIBRARY_PATH="$(dirname "$lib")" ldd -r "$lib" 2>&1)"
    if grep -qE "not found|undefined|Error loading" <<< "$out"; then
        echo "ERROR: $(basename "$lib") cannot load on this host:" >&2
        echo "$out" >&2
        echo "The libraries must be built with the musl toolchain to run in" >&2
        echo "Plex's runtime; check the build used x86_64-linux-musl-gcc." >&2
        exit 1
    fi
}
check_lib "$avcodec"
check_lib "$libformat"

# 4. Back up Plex's originals and install ours. Plex's libs may be
# symlinks into versioned files, so dereference when backing up and remove
# the symlink before installing.
install_lib() {
    local src="$1" dst_name="$2"
    if [[ ! -e "${PMS_LIB}/${dst_name}.orig-vvc" ]]; then
        cp -aL "${PMS_LIB}/${dst_name}" "${PMS_LIB}/${dst_name}.orig-vvc"
        echo "Backed up ${PMS_LIB}/${dst_name} -> ${dst_name}.orig-vvc"
    fi
    rm -f "${PMS_LIB}/${dst_name}"
    cp -aL "$src" "${PMS_LIB}/${dst_name}"
    ln -sf "${dst_name}" "${PMS_LIB}/${dst_name%%.*}.so"
    echo "Installed ${PMS_LIB}/${dst_name}"
}
install_lib "$avcodec"  "libavcodec.so.60"
install_lib "$libformat" "libavformat.so.60"

# 5. Build and install the VVC external decoder module so the transcoder
#    has a decoder implementation for VVC.
if zsh "${SCRIPT_PATH}/vvc-module/build-module.zsh" "${SRC_DIR}"; then
    codec_dir="$(ls -d "${PLEX_MEDIA_SERVER_APPLICATION_SUPPORT_DIR:-/var/lib/plexmediaserver/Library/Application Support/Plex Media Server}"/Codecs/*-linux-x86_64 2> /dev/null | head -1)"
    if [[ -n "$codec_dir" ]]; then
        cp -f "${REPO_PATH}/run/libvvc_decoder.so" "$codec_dir/"
        chown plex:plex "$codec_dir/libvvc_decoder.so"
        echo "Installed $codec_dir/libvvc_decoder.so"
    else
        echo "WARNING: Codecs directory not found - install libvvc_decoder.so manually" >&2
    fi
else
    echo "WARNING: libvvc_decoder.so build failed" >&2
fi

echo
echo "Done. Next steps:"
echo "  1. Restart Plex Media Server (sudo systemctl restart plexmediaserver)."
echo "  2. Plex > Settings > Transcoder > enable 'Disable video stream"
echo "     copying' so VVC is transcoded (libx264) instead of direct-played"
echo "     to clients that cannot decode it."
echo "  3. Refresh the metadata of your VVC items, then play."
echo "To revert: delete the installed libraries and restore the *.orig-vvc backups."
