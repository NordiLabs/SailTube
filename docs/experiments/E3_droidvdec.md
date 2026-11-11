<!-- SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0 -->
# E3: `droidvdec` per device

Status: **partly run** on R2 (inventory only, 2026-11-08); the probe, D3 and D7 are still to run.
Spec: §7 E3, PLY-20, PLY-21, PLT-08, PLT-07, SEL-02, §6 D3 and D7. Devices: R1, R2 and, per the
spec, two more models (Xperia 10 IV/V, a community port). Only R2 (POCO) is known to be available.

## Question

On each device, does `droidvdec` decode H.264 and VP9 at 720p and 1080p, with the first frame
within 2 s, no black or green frames across 10 display off/on cycles, and lower CPU than software
decode? Which parsed caps does it accept?

Unknowns the run must answer (no device data yet): whether the droid plugin and its codec libraries
load at all on the POCO (PLT-08 cites stalls without a frame on some devices); `droidvdec`'s
registered rank (PLT-08 wants it demoted to none at start-up); whether `stream-format=avc` (the
PLY-20 chain) or `byte-stream` is accepted; whether VP9 is offered by the device's codec list.

## Method

Build/flags: part 1 needs no app build; part 2 needs a build with the PLY-20 hardware path and its
probe, with hardware decode switched on in settings. Clips from `tools/experiments/make_clips.sh`:
`h264_720p.mp4`, `h264_1080p.mp4`, `vp9_720p.webm`, `vp9_1080p.webm` (video-only, 2 s GOP).

Part 1, element probe (`tools/experiments/e3_droidvdec_probe.sh`):

1. The maintainer clears the slot; copy tools and clips to the phone.
2. `sh e3_droidvdec_probe.sh h264_720p.mp4 h264_1080p.mp4 vp9_720p.webm vp9_1080p.webm`.
   Preamble: mute + verify. No audio element is used.
3. The script records `gst-inspect-1.0` for `droidvdec`, `droideglsink`, the parsers, demuxers and
   the software decoders present (`avdec_h264`/`openh264dec`, `vp9dec`/`avdec_vp9`), with ranks, and
   copies `/etc/gst-droid/` and `/etc/media_codecs.xml` if readable (file names are per port; the
   run shows what exists).
4. For each clip, caps variant (H.264: `avc` and `byte-stream`, both `alignment=au`; VP9:
   `video/x-vp9`) and decoder (`droidvdec`, then the software decoder):
   * probe run: `filesrc ! demux ! parse ! caps ! DECODER ! fakesink silent=false sync=false`, 3 s
     hard timeout (`gst_watch.py`): first-frame ms, frames, child CPU ms;
   * real-time run: same with `sync=true` for 10 s: CPU ms over wall time.
5. `results.tsv` gets one row per run with verdict `probe-pass` (frames, first ≤ 2000 ms), `late`
   (frames, first > 2000 ms) or `no-frame` (the PLY-20 fallback case).

`droideglsink` is only checked for presence: it needs an EGL consumer (the app's scene graph), so
`gst-launch-1.0` cannot exercise it. Rendering is part 2.

Part 2, in the app (§6 D3 and D7, manual, operator on the phone):

6. Enable hardware decode. Confirm the probe runs once and its result appears in diagnostics
   (DIA-02 "hardware-decode probe result").
7. Play an H.264 and a VP9 1080p video. Expect a frame within 3 s or a fallback notice (PLY-20); never
   a silent black picture.
8. D7: during hardware playback, turn the display off and on 10 times (power key). Watch for black or
   green frames and garbled glyphs (PLT-07). Repeat in software mode. Optionally run
   `sh cpu_sample.sh -c hw-1080p -i 5 -d 120` and the same for software to compare CPU in the app.
9. Also confirm PLY-21/PLT-08 with hardware decode off: the first video's pipeline graph dump
   contains no droid element.

## Pass criterion

Per device and codec/resolution: probe-pass in part 1 or a frame within 2 s in part 2, zero black or
green frames across 10 display cycles, and real-time CPU lower than the software decoder's.

## What it decides

Per-device hardware-decode defaults (PLY-20), whether the explicit chain uses `avc` or `byte-stream`
caps, the SEL-02 codec preference for verified hardware decode (VP9) and its 1080p ceiling.

## Result notes

### R2 (POCO), SFOS 5.2.0.22, 2026-11-08: decoder inventory, run by the maintainer

Observed during the first FULL run of build 2ed333b (the Qt Multimedia path, not the
GStreamer player); `gst-inspect-1.0` is not installed, so droidvdec's caps and rank were not read.

| Decoder family | Present |
|---|---|
| Software H.264 | yes: gstreamer1.0-libav 1.26.11 with Jolla ffmpeg 5.1 (`libavcodec.so.59`) |
| Software VP9 | yes: avdec (libav), also libvpx-vp9 through libgstvpx |
| Software AAC, Opus, MP3 | yes |
| Software HEVC, AV1 (incl. dav1d) | no |
| droidvdec, droidadec (libgstdroid) | yes |

Consequences:
- The interim per-item hardware rule (`selectStreamsWithFallback`, decision log 2026-11-08) is not
  needed on R2: software H.264 exists, so no item falls back to hardware. It stays for devices
  without libav (R1 is still unknown).
- SEL-01/02 unchanged: with software decode the default unmetered ceiling stays 720p.
- With hardware decode off the old path instantiated a droid Codec2 decoder (threads `codec_looper`,
  `CCodecWatchdog`), most likely droidadec for AAC. PLT-08 says no droid element at all, so
  `elementsToDemote()` now lists droidvdec and droidadec; AAC then needs a software decoder (present
  on R2).
- The same run picked "1080p60 H.264 adaptive" for a 60 fps-only video and stalled at 0:00 with a
  single `souphttpsrc` (DASH manifest path). Causes on that path: the app's stored ceiling default
  of 1080 overrode the 720p software default, and the adaptive pair went through one manifest
  source. Batch 2 replaces that path with two source branches (dual plan) and the default ceiling.

Still open on R2: droidvdec caps and rank (needs `gst-inspect-1.0` or the app's diagnostics probe),
the PLY-20 2 s probe, D3, D7 and CPU comparison.
