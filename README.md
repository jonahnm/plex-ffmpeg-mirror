# plex-ffmpeg-mirror

An automated mirror of Plex's patched FFMPEG source.

A scheduled [GitHub Actions](.github/workflows/refresh.yml) workflow runs daily,
pulls the latest FFMPEG source that Plex publishes in its bundled `LICENSE`
file, and commits any changes back to this repository. No server or cron host
of your own is required -- GitHub runs it for you.

## Layout

```
.github/workflows/refresh.yml   # the scheduled job
tools/fetch-plex-ffmpeg.zsh     # downloads + unpacks the source
tools/apply-vvc-patch.zsh       # reapplies the VVC decoder patch after each sync
tools/vvc-decoder.patch         # adds the native VVC (H.266) decoder to Plex's source
tools/build-ffmpeg.zsh          # builds the source into a working ffmpeg
tools/backfill.zsh              # backfills history from local PMS tarballs
plex-ffmpeg-source/             # the mirrored source (committed by the workflow)
  PlexTranscoder/               # Plex's single ffmpeg GPL source
latest.version                  # mirrored Plex version + ffmpeg source hash
```

Plex's LICENSE attributes its ffmpeg GPL source to "Plex Transcoder" (verified
across every released version); there is no separate "new" transcoder, so the
mirror keeps a single `PlexTranscoder/` folder. If Plex ever lists a second,
distinct source, it lands in `PlexTranscoder-2/`.


## How it works

1. The workflow triggers on a cron schedule (and can be run manually from the
   Actions tab via `workflow_dispatch`).
2. It installs `zsh`/`jq`, then runs `tools/fetch-plex-ffmpeg.zsh`, which:
   - asks the Plex update API for the latest FreeBSD build,
   - extracts it just to read its `Resources/LICENSE`,
   - scrapes the ffmpeg source-archive URLs from that license,
   - downloads and unpacks each into `plex-ffmpeg-source/`.
3. If the working tree changed, the workflow commits and pushes using the
   built-in `GITHUB_TOKEN`.

## VVC (H.266) decoder

Plex's published FFMPEG source ships VVC parsing (`cbs_h266`, `vvc_parser`)
but not the native VVC decoder or VVC-in-Matroska support. After every sync,
`tools/apply-vvc-patch.zsh` reapplies `tools/vvc-decoder.patch`, which adds:

- the native VVC decoder (`libavcodec/vvc/`, ported from FFmpeg 7.0) plus the
  shared `libavcodec/h26x/` DSP code it depends on,
- the `FFRefStructPool` API in `libavcodec/refstruct.[ch]`,
- a `cbs_h266` fix so the slice-header `curr_subpic_idx` derived value is
  actually stored (subpicture streams failed without it),
- VVC-in-Matroska muxing/demuxing (the `V_MPEGI/ISO/VVC` codec tag and the
  VCC CodecPrivate writer, ported from FFmpeg 8.0),
- the VVC decoder is no longer marked `AV_CODEC_CAP_EXPERIMENTAL`, so it
  decodes without `-strict experimental` and no longer prints the
  "experimental codecs are not enabled" warning,
- Plex integration: ffprobe reports VVC streams as `hevc` (Plex's analyzer
  maps ffprobe's `codec_name` to its internal codec enum, which has no VVC
  entry and otherwise shows the codec as "NONE"; the decoder itself is keyed
  by codec id, so decoding and `-c:v vvc` are unaffected).

Verified against the upstream FFmpeg 7.0 VVC conformance suite: all 22
`fate-vvc-conformance-*` streams decode bit-identically, and a VVC stream
muxed to MKV and demuxed back decodes bit-identically. Two upstream caveats
apply to every FFmpeg build (7.0-8.0, including stock): monochrome streams
(e.g. `SCALING_A_1`) differ after a gray-to-YUV color conversion, because
the 6.1-era swscale handles gray range tags differently than FFmpeg 7.0's
(the decoded frames themselves are identical), and the CLI's CFR output
drops some delayed frames at end-of-stream for VVC-in-MKV (delayed frames
carry the current packet's PTS; the decoder never set its own).

The patch is applied with zero fuzz; if Plex's source ever drifts so it no
longer applies, the sync continues with a warning and the patch needs
refreshing from `tools/vvc-decoder.patch`.

## First-time setup

1. Create a new empty repo on GitHub and push these files.
2. In the repo: **Settings -> Actions -> General -> Workflow permissions ->
   Read and write permissions** (lets the job push commits).
3. The schedule starts automatically. To run it immediately, open the
   **Actions** tab, select **Refresh Plex FFMPEG source**, and click **Run
   workflow**.

## Beta (Plex Pass) channel

By default the public **stable** channel is mirrored. To follow the Plex Pass
**beta** channel instead, add a repository secret named `PLEX_TOKEN` (your Plex
account's `X-Plex-Token`): **Settings -> Secrets and variables -> Actions -> New
repository secret**. When set, the workflow queries
`downloads/5.json?channel=plexpass`; the token is sent as an HTTP header, never
in a URL. Remove the secret to go back to stable.

Locally: `PLEX_TOKEN=xxxx zsh tools/fetch-plex-ffmpeg.zsh`.

## Run locally

```zsh
zsh tools/fetch-plex-ffmpeg.zsh                 # -> ./plex-ffmpeg-source
zsh tools/fetch-plex-ffmpeg.zsh /some/folder    # -> /some/folder
```

## Build locally

```zsh
zsh tools/build-ffmpeg.zsh                       # full build (ffmpeg/ffprobe/ffplay)
zsh tools/build-ffmpeg.zsh -j8                   # control parallelism
zsh tools/build-ffmpeg.zsh /some/folder/PlexTranscoder   # build a fetched copy elsewhere
zsh tools/build-ffmpeg.zsh -- --enable-...       # pass extra ./configure args
```

The build runs in-tree; `configure` is run with `--disable-doc`, the VVC
decoder patch is (re)applied first if needed, and on macOS the system
`ar`/`ranlib`/`strip` are preferred over Homebrew's GNU binutils, whose
GNU-format archives and stripped binaries macOS cannot link or run.
