<!-- SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0 -->
# E7: Is qtdemux push-seek still broken on current SFOS GStreamer?

Status: **prepared, not run** (2026-11-08).
Spec: §7 E7, PLY-04, PLY-03, SEL-04, NFR-PERF-03, §6 P3. Device: R2 (POCO, SFOS 5.2), then R1.

## Question

FinTube's measured dead ends (CLAIMED, 2026-09-07): without a byte-seekable upstream qtdemux refused
every flush-seek; with accurate seek each seek took 10–50 s. Does that still hold on SFOS 5.2's
GStreamer? How do matroskademux and qtdemux compare in push mode over a Range-capable loopback
source, for video (H.264 mp4 vs VP9 WebM) and audio (AAC m4a vs Opus WebM, the SEL-04 question)?

## Method

Build/flags: no app build. Needs `python3-gobject` with the Gst 1.0 typelib on the phone (the
script stops with exit 4 if missing). Clips from `tools/experiments/make_clips.sh` (90 s, 2 s GOP):
`h264_1080p.mp4` (fragmented, DASH-like), `h264_1080p_progressive.mp4`, `vp9_1080p.webm`, `aac.m4a`,
`opus.webm`.

1. The maintainer clears the slot; copy tools and clips to the phone.
2. `sh e7_qtdemux_push_seek.sh h264_1080p.mp4 h264_1080p_progressive.mp4 vp9_1080p.webm aac.m4a opus.webm`.
   Preamble: mute + verify. Sinks are `fakesink sync=true` only, no audio output.
3. The script starts two `range_server.py` instances on 127.0.0.1: one with Range (206/416), one
   ignoring Range (200 only, no `Accept-Ranges`). Both are capped at 20 000 kbit/s with 20 ms added
   latency (the spec's Wi-Fi floor; `E7_RATE_KBPS`, `E7_RTT_MS`).
4. For each clip, upstream mode and flag set, `e7_seek.py` prerolls, plays, then issues 20
   flush-seeks to fixed-seed random targets in [5 s, duration − 10 s], 1 s apart, timing each from
   `seek_simple()` to `ASYNC_DONE` and recording whether it was refused and where it landed:
   * modes: `pull-file` (`filesrc ! demux`, baseline), `push-file` (`filesrc ! queue ! demux`),
     `push-http` (`souphttpsrc` Range ! queue ! demux), `push-norange` (no Range);
   * flags: `key` (FLUSH|KEY_UNIT), `snap` (+SNAP_BEFORE), `accurate` (FLUSH|ACCURATE).
   A run stops after 3 seek timeouts (60 s each) so the accurate dead end cannot hold the phone.
   Each run re-checks the SailTune window and the mute first.
5. `summary.txt`: per (clip:mode, flags) n, accepted, done, p50 ms, max ms, max landing error.
6. Copy back; write the dated note, including the GStreamer version from `env.txt`.

### Without python3-gobject

`gst-launch-1.0` cannot seek. If the user declines installing `python3-gobject`:

* `gst-play-1.0` (if present, gst-plugins-base tools) seeks with the arrow keys in an interactive
  `ssh -t` session; with `GST_DEBUG=qtdemux:4,matroskademux:4 gst-play-1.0 --videosink=fakesink
  --audiosink=fakesink <uri>` the debug log timestamps show seek handling. Timing is coarse and
  manual; record it as such.
* Better: the app's player (PLY-04 build) with the timing markers (`seek-start`/`seek-flush`/
  `seek-done`, see `tools/experiments/perf_timing.md`) during §6 P3 gives the same answer for the
  real pipeline, analysed with `perf_summary.py`.

## Pass criterion

`push-http` + `accurate` on the mp4 clips: 20 of 20 seeks accepted and each done in under 2 s.

## What it decides

* PLY-04: if accurate push seek passes, the "never accurate in push mode" rule and the disk-backed
  byte-seekable window for mp4 can be relaxed; if it fails, they stay, and the `key`/`snap` rows give
  the expected mp4 seek cost for NFR-PERF-03.
* SEL-04: if `aac.m4a` push-seeks as reliably as `opus.webm`, the Opus-before-AAC rank can be relaxed.
* `push-norange` shows whether the "no byte-seekable upstream" refusal is still real.

## Result notes

(none yet)
