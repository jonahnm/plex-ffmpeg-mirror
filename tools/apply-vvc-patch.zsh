#!/bin/zsh
# Reapply the VVC decoder patch after each fresh Plex FFMPEG source sync.
#
# Plex's published FFMPEG source (FFmpeg 6.1.3-based) ships VVC parsing
# support (cbs_h266, vvc_parser, vvc_mp4toannexb_bsf) but no native VVC
# decoder. This patch restores it:
#   - the native VVC decoder (libavcodec/vvc/, ported from FFmpeg 7.0),
#   - the shared H.265/6 DSP code it uses (libavcodec/h26x/, x86/...),
#   - the FFRefStructPool API in libavcodec/refstruct.[ch],
#   - a cbs_h266 fix: the slice-header derived value curr_subpic_idx was
#     only computed into a local variable, never stored (breaks streams
#     with subpictures).
# See tools/vvc-decoder.patch.
#
# Usage: apply-vvc-patch.zsh [OUT_PATH] [PATCH_FILE]
#   OUT_PATH   where the fresh source was unpacked (default: repo/plex-ffmpeg-source)
#   PATCH_FILE path to the patch (default: tools/vvc-decoder.patch)
#
# Idempotent: a marker file is dropped inside OUT_PATH once applied, and the
# fresh source replaces OUT_PATH entirely on every sync, so the marker cannot
# survive a refresh. If the patch no longer applies to the freshly downloaded
# source, a warning is printed and the sync continues without VVC support
# (the patch then needs refreshing).

typeset -gr SCRIPT_PATH="${0:A:h}"
typeset -gr REPO_PATH="${SCRIPT_PATH:h}"
typeset -gr OUT_PATH="${1:-${REPO_PATH}/plex-ffmpeg-source}"
typeset -gr PATCH_FILE="${2:-${SCRIPT_PATH}/vvc-decoder.patch}"
typeset -gr MARKER="${OUT_PATH}/.vvc-decoder-patch-applied"

if [[ -f "$MARKER" ]]; then
    echo "VVC decoder patch already applied; skipping"
    exit 0
fi
if [[ ! -f "$PATCH_FILE" ]]; then
    echo "VVC patch skipped: $PATCH_FILE not found"
    exit 0
fi
if [[ ! -d "$OUT_PATH" ]]; then
    echo "VVC patch skipped: $OUT_PATH does not exist"
    exit 0
fi

# The patch paths are relative to the repository root
# (plex-ffmpeg-source/PlexTranscoder/...). Where they resolve depends on
# where the source was unpacked:
#   - default layout (OUT_PATH == repo/plex-ffmpeg-source): from the repo root
#     (git apply, or patch -p1 outside a git work tree);
#   - custom unpack folder (e.g. "zsh tools/fetch-plex-ffmpeg.zsh /some/dir"):
#     apply inside OUT_PATH, stripping the leading plex-ffmpeg-source/
#     component (patch -p2).
#
# Each path first checks whether the patch applies cleanly; if not, whether
# it is already applied (reverse check) so re-runs on a patched tree are a
# silent no-op; otherwise the patch needs refreshing.
warn_unapplied() {
    echo "WARNING: VVC decoder patch does not apply cleanly to the fresh source." >&2
    echo "Plex may have changed files the patch touches; refresh tools/vvc-decoder.patch." >&2
}

applied=0
if [[ "$OUT_PATH" == "${REPO_PATH}/plex-ffmpeg-source" ]]; then
    cd "$REPO_PATH" || exit 1
    if git rev-parse --is-inside-work-tree > /dev/null 2>&1; then
        if git apply --check "$PATCH_FILE" 2> /dev/null; then
            git apply "$PATCH_FILE" || { echo "VVC patch: git apply failed" >&2; exit 1 }
            applied=1
        elif git apply --reverse --check "$PATCH_FILE" 2> /dev/null; then
            echo "VVC decoder patch already applied; skipping"
        else
            warn_unapplied
        fi
    else
        if patch -p1 --fuzz=0 --dry-run -s < "$PATCH_FILE" > /dev/null 2>&1; then
            patch -p1 --fuzz=0 -s < "$PATCH_FILE" > /dev/null || { echo "VVC patch: patch(1) failed" >&2; exit 1 }
            applied=1
        elif patch -p1 --fuzz=0 --dry-run -s -R < "$PATCH_FILE" > /dev/null 2>&1; then
            echo "VVC decoder patch already applied; skipping"
        else
            warn_unapplied
        fi
    fi
else
    cd "$OUT_PATH" || exit 1
    if patch -p2 --fuzz=0 --dry-run -s < "$PATCH_FILE" > /dev/null 2>&1; then
        patch -p2 --fuzz=0 -s < "$PATCH_FILE" > /dev/null || { echo "VVC patch: patch(1) failed" >&2; exit 1 }
        applied=1
    elif patch -p2 --fuzz=0 --dry-run -s -R < "$PATCH_FILE" > /dev/null 2>&1; then
        echo "VVC decoder patch already applied; skipping"
    else
        warn_unapplied
    fi
fi

(( applied )) || exit 0
touch "$MARKER"
echo "Applied VVC decoder patch to $OUT_PATH"
