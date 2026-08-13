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

# 1. Build the mirrored source with shared libraries. Pass the fcntl
#    compatibility header so the libraries do not reference fcntl64 (which
#    some glibc versions do not export) and stay loadable on any host.
echo "== building shared ffmpeg libraries (this takes a few minutes) =="
zsh "${SCRIPT_PATH}/build-ffmpeg.zsh" "${SRC_DIR}" -- --enable-shared \
    "--extra-cflags=-include ${REPO_PATH}/tools/fcntl-compat.h" \
    || die "build failed"

# 2. Locate the freshly built libraries.
local avcodec libavcodec libformat
avcodec="$(ls "${SRC_DIR}/libavcodec/libavcodec.so."* 2> /dev/null | grep -v '\.so$' | head -1)"
libformat="$(ls "${SRC_DIR}/libavformat/libavformat.so."* 2> /dev/null | grep -v '\.so$' | head -1)"
[[ -n "$avcodec" && -n "$libformat" ]] || die "shared libraries not produced (check the build log)"
echo "Built: ${avcodec:t} (${libformat:t})"

# 3. Pre-flight: the freshly built libraries must be able to resolve all
# their symbols against this host's glibc (ldd -r exits non-zero when any
# symbol is undefined). Plex builds against an old glibc; a build done on
# a newer glibc will fail to load here with e.g. "fcntl64: symbol not
# found".
check_lib() {
    local lib="$1" ldd_log
    ldd_log="$(mktemp)"
    if ! LD_LIBRARY_PATH="$(dirname "$lib")" ldd -r "$lib" > "$ldd_log" 2>&1; then
        echo "ERROR: $(basename "$lib") cannot load on this host:" >&2
        cat "$ldd_log" >&2
        rm -f "$ldd_log"
        echo "The build used a glibc newer than this system's runtime. Build on this" >&2
        echo "machine itself (same distro/glibc), or in an old container, then retry." >&2
        exit 1
    fi
    rm -f "$ldd_log"
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

echo
echo "Done. Restart Plex Media Server, then refresh the metadata of your"
echo "VVC items (Plex > item > 'Refresh Metadata') or rescan the library."
echo "To revert: delete the installed libraries and restore the *.orig-vvc backups."
