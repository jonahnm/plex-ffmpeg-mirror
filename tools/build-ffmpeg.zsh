#!/bin/zsh
# Build Plex's patched FFMPEG source (with the native VVC decoder) into a
# usable ffmpeg / ffprobe / ffplay. The build happens in-tree in the source
# directory; binaries land next to the sources.
#
# Usage:
#   tools/build-ffmpeg.zsh [SRC_DIR] [configure args...]
#
# Options:
#   -j N, --jobs N    parallel jobs (default: number of CPU cores)
#   -c, --clean       run "make distclean" before configuring
#   -h, --help        show this help
#
# SRC_DIR defaults to <repo>/plex-ffmpeg-source/PlexTranscoder and must
# contain a checked-out FFMPEG tree. The VVC decoder patch is (re)applied
# first if needed, so this also works on a freshly fetched source. Any extra
# arguments are passed through to ./configure (--disable-doc is always added).
#
# On macOS, the system ar/ranlib/strip are preferred over Homebrew binutils,
# whose GNU-format archives and stripped binaries macOS cannot link or run.

typeset -gr SCRIPT_PATH="${0:A:h}"
typeset -gr REPO_PATH="${SCRIPT_PATH:h}"
typeset -gr SELF="${0:A}"

typeset -g SRC_DIR="${REPO_PATH}/plex-ffmpeg-source/PlexTranscoder"
typeset -g JOBS=""
typeset -gi CLEAN=0
typeset -ga CONFIGURE_ARGS

usage() {
    sed -n '2,26p' "$SELF"
    exit 0
}

while [[ $# -gt 0 ]]; do
    case "$1" in
        -h|--help) usage ;;
        -j|--jobs) JOBS="${2:?option -j requires a value}"; shift 2 ;;
        -j[0-9]*) JOBS="${1#-j}"; shift ;;
        -c|--clean) CLEAN=1; shift ;;
        --) shift; CONFIGURE_ARGS+=("$@"); break ;;
        -*) CONFIGURE_ARGS+=("$1"); shift ;;
        *) SRC_DIR="$1"; shift ;;
    esac
done

die() {
    echo "FATAL: $1" >&2
    exit 1
}

[[ -d "$SRC_DIR" ]] || die "Source directory not found: $SRC_DIR (run tools/fetch-plex-ffmpeg.zsh first)"
[[ -f "$SRC_DIR/configure" ]] || die "$SRC_DIR does not look like an FFMPEG tree"

# Make sure the VVC decoder is present (no-op if already applied).
zsh "${REPO_PATH}/tools/apply-vvc-patch.zsh" "${SRC_DIR:h}" || die "Failed to apply the VVC decoder patch"

cd "$SRC_DIR" || die "Could not change into $SRC_DIR"

# Parallelism.
if [[ -z "$JOBS" ]]; then
    if command -v sysctl > /dev/null 2>&1 && JOBS="$(sysctl -n hw.ncpu 2> /dev/null)"; then
        [[ "$JOBS" =~ ^[0-9]+$ ]] || JOBS=""
    fi
    if [[ -z "$JOBS" ]] && command -v nproc > /dev/null 2>&1; then
        JOBS="$(nproc)"
    fi
    [[ -n "$JOBS" ]] || JOBS=2
fi

# macOS: Homebrew's GNU binutils (ar/ranlib/strip) shadow the system tools and
# produce archives and stripped binaries macOS refuses to link or run. Prefer
# /usr/bin when a non-system tool is first on PATH.
typeset -ga TOOL_ARGS
if [[ "$(uname)" == "Darwin" && -d /usr/bin ]]; then
    for tool in ar ranlib strip; do
        resolved="$(command -v "$tool" 2> /dev/null)"
        if [[ -n "$resolved" && "$resolved" != "/usr/bin/$tool" && -x "/usr/bin/$tool" ]]; then
            echo "Using system $tool (/usr/bin/$tool) instead of $resolved"
            TOOL_ARGS+=( "--$tool=/usr/bin/$tool" )
        fi
    done
fi

if (( CLEAN )); then
    echo "== make distclean =="
    make distclean > /dev/null 2>&1
fi

echo "== configure =="
# Plex's configure script uses bash-only pattern substitutions (e.g.
# ${cfg/_*/_decoder}) despite its #!/bin/sh shebang. macOS /bin/sh is bash,
# but Debian's /bin/sh is dash, which rejects those. Run it with bash
# explicitly when available.
if command -v bash > /dev/null 2>&1; then
    bash ./configure --disable-doc "${TOOL_ARGS[@]}" "${CONFIGURE_ARGS[@]}" || die "configure failed"
else
    ./configure --disable-doc "${TOOL_ARGS[@]}" "${CONFIGURE_ARGS[@]}" || die "configure failed"
fi

# Post-configure hook: when the tree contains libavcodec/libvvdec.c (the
# libvvdec-based replacement for the native vvc decoder, installed by
# deploy-plex-vvc.zsh), register it in the generated codec list. The
# native vvc decoder is disabled via --disable-decoder=vvc, so
# ff_vvc_decoder is only defined by libvvdec.c.
if [[ -f "libavcodec/libvvdec.c" && -f "libavcodec/codec_list.c" ]] \
    && ! grep -q "ff_vvc_decoder" "libavcodec/codec_list.c"; then
    echo "== registering libvvdec vvc decoder in codec_list.c =="
    python3 - << 'PYEOF'
p = "libavcodec/codec_list.c"
s = open(p).read()
s = s.replace("extern const FFCodec ff_av1_decoder;",
              "extern const FFCodec ff_av1_decoder;\nextern const FFCodec ff_vvc_decoder;", 1)
s = s.replace("&ff_av1_decoder,",
              "&ff_av1_decoder,\n&ff_vvc_decoder,", 1)
open(p, "w").write(s)
PYEOF
fi

echo "== make -j${JOBS} =="
make -j"$JOBS" || die "make failed"

# On Apple Silicon every binary must carry at least an ad-hoc signature;
# sign whatever was produced if codesign is available.
if [[ "$(uname)" == "Darwin" ]] && command -v codesign > /dev/null 2>&1; then
    for bin in ffmpeg ffprobe ffplay; do
        [[ -x "$bin" ]] && codesign --force --sign - "$bin" > /dev/null 2>&1
    done
fi

echo
echo "== result =="
# In a shared build the binaries need the built libraries on the library
# path (they have no rpath pointing at the build tree).
if [[ -f ffbuild/config.mak ]] && grep -q "CONFIG_SHARED=yes" ffbuild/config.mak; then
    libpath="$(find "$PWD" -maxdepth 2 -name 'lib*.so.*' -o -maxdepth 2 -name 'lib*.dylib' 2>/dev/null | xargs -n1 dirname 2>/dev/null | sort -u | paste -sd: -)"
    LD_LIBRARY_PATH="${libpath}${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
    export LD_LIBRARY_PATH
fi
"$PWD/ffmpeg" -version 2>&1 | head -1
if grep -q "CONFIG_VVC_DECODER=yes" ffbuild/config.mak; then
    echo "VVC decoder: enabled"
else
    echo "VVC decoder: NOT enabled" >&2
fi
"$PWD/ffmpeg" -hide_banner -codecs 2> /dev/null | grep -E "vvc" | sed 's/^/  /'
echo
echo "Built: $PWD/ffmpeg  $PWD/ffprobe"
