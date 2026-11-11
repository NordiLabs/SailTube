# Video playback on Sailfish OS 5.x

Status 2026-11-11: this note describes the platform facts and the phase-1 path through Qt Multimedia (§1 to §6). The
app now plays through the app's own GStreamer player (`Core/Player`, FULL only); how it works, what it measures and
what is left is in `docs/PERF_PLAYBACK.md` and `docs/ARCHITECTURE.md`. The playbin3 trick of §4 is
kept for the reduced STORE player that is planned (STORE ships no video playback today). The device steps of §6 are
mapped to `docs/DEVICE_CHECKLIST.md` (table "Slice steps in PLAYBACK.md").

What can decode what, which streams SailTube picks, and what the phone still has to confirm. Facts are tagged
**SDK** (read from the SFOS 5.1.0.11 aarch64 SDK target in the `sfos-m1-native` container on 2026-11-08),
**DOC** (public documentation), **SPEC** (analyst requirements draft) or **ASSUMED** (needs the device).

## 1. The platform pipeline

- Qt Multimedia 5.6.2 (`qt5-qtmultimedia-5.6.2+git32`) with the GStreamer media-service plugin
  (`libgstmediaplayer.so`). It builds a **`playbin`** element per player and accepts one URI. It sets the
  User-Agent and other request headers of a `QNetworkRequest` on the HTTP source. **SDK/DOC**
- `VideoOutput` on Sailfish renders through `nemo-qtmultimedia-plugins-gstvideotexturebackend`
  (declarative video backend), which takes frames from `droideglsink` as EGL images when the hardware decoder
  is used. `VideoOutput` reads the player from a `mediaObject` property; `PlaybackSession` exposes one. **SDK**
- GStreamer **1.26.11** (core, base, good, bad). **SDK**
- Present in the SDK target: `qtdemux`, `matroskademux`, `hlsdemux` (legacy, in `libgsthls`),
  `dashdemux2`/`hlsdemux2`/`mssdemux2` (`libgstadaptivedemux2`), `playbin3`, `souphttpsrc`, `vp8dec`/`vp9dec`
  (`libgstvpx`, libvpx 1.16), `opusdec`, `vorbisdec`. **Absent:** legacy `dashdemux`, `gst-libav`
  (`avdec_*`), `dav1d`, `aom`, `openh264`. ffmpeg 5.1 libraries exist (browser) but no GStreamer wrapper. **SDK**
- The hardware path is `droidvdec`/`droidadec` from **gst-droid**, an adaptation package on the device, not in
  the SDK. Its sink caps list exactly the codecs that the Android vendor media stack exposes. **DOC/ASSUMED**

## 2. POCO X3 Pro (Snapdragon 860, "vayu")

| Codec | Expected decoder | Basis | Status |
|---|---|---|---|
| H.264 (avc1) up to 1080p60 | `droidvdec` hardware | SoC video block decodes H.264/HEVC/VP9 up to 4K; gst-droid exposes `video/x-h264` on every supported port | **ASSUMED**, check §6 step V1 |
| VP9 (vp09) up to 1080p | `droidvdec` hardware **if** the port's media codec list exposes VP9; otherwise `vp9dec` software (fine to 720p30) | SoC capability; gst-droid caps are per port | **ASSUMED** |
| HEVC | `droidvdec` hardware | not offered by YouTube for normal videos | irrelevant |
| AV1 (av01) | **none**: SoC has no AV1 block, SDK has no `dav1d`/`aom`/`avdec_av1` | SDK | excluded (SEL-01) |
| AAC (mp4a) | `droidadec` or a software AAC decoder | SailTune plays m4a on device | likely |
| Opus | `opusdec` software | SDK | yes |

Gst-libav may be installed on a given device even though it is not in the SDK; the capabilities page shows the
real list. The display is 1080×2400, so 1080p is the useful ceiling.

## 3. Stream choice (what the code does)

`Core/Media/Domain/StreamSelection.cpp`, unit-tested in `tests/domain`:

1. Video codecs: only those the probe found a decoder for. Hardware first in automatic mode; among hardware
   codecs H.264, then VP9, HEVC, AV1. Software decoders are limited to 720p (SEL-02). HDR is never chosen.
2. Height: the tallest rung at or below the ceiling (default 1080p; `Settings > Maximum quality`); if none,
   the smallest above it. At equal height the ≤30 fps rung wins; `1080p60` style entries are explicit choices
   in the quality menu (SEL-02/03).
3. **Adaptive** (separate video and audio) is used when it is taller than the muxed stream and the adaptive
   pipeline is available. Pairs are MP4-only: H.264 (or AV1, where decodable) video with AAC audio, joined by a
   generated DASH manifest (§4). YouTube serves VP9 only in WebM, so VP9 is reachable only as a progressive or
   future WebM-adaptive stream.
4. **Progressive** muxed stream (in practice itag 18, 360p H.264 + AAC) is always kept as the fallback.
5. **Audio-only:** best audio-only stream (original language, non-DRC, AAC then Opus, bitrate), then the other
   codec, then the smallest muxed stream without a video surface.
6. **Live:** an HLS variant within the ceiling, then the master manifest (legacy `hlsdemux` handles muxed
   variants). Upcoming events are not resolved.

## 4. Adaptive streams through Qt Multimedia (experiment E13)

Problem: Qt 5.6 plays one URI and asks for `playbin`; separate audio/video streams need a demuxer that joins
them under one clock. Joining them ourselves (two players) cannot keep lip-sync.

Approach:

1. For the chosen video and audio formats, read the first 64 KiB (more if needed, at most 1 MiB) with a
   `Range` request carrying the format's headers, and walk the top-level ISO BMFF boxes: `ftyp`, `moov`
   (initialisation range `0 … end of moov`) and `sidx` (index range). YouTube's adaptive MP4 files are
   on-demand DASH files, so `sidx` follows `moov`.
2. Write a static `isoff-on-demand` MPD with one video and one audio AdaptationSet, `BaseURL` = the signed
   stream URL and `SegmentBase indexRange`/`Initialization range`, into the private cache (0600). It is deleted
   on the next attempt, on stop and at start.
3. Give the pipeline `file:///…/x.mpd`. `dashdemux2` fetches fragment byte ranges as listed in each `sidx`, so
   requests are small bounded ranges (seeking works per fragment).
4. `dashdemux2` is designed for playbin3 (the legacy `dashdemux` is not shipped). For an adaptive attempt the
   session re-registers playbin3's GType under the factory name `playbin` (`gst_element_register` replaces the
   registry feature) and recreates its `QMediaPlayer`, because Qt 5.6 creates the pipeline in the player's
   constructor. Progressive, live and audio-only attempts switch back to the stock playbin, which Qt 5.6 fully
   understands. `Settings > Adaptive streams`: Off / Automatic / Always use playbin3 (diagnostics); changes
   apply at the next start.

First observation (build container, no display, 2026-11-08): with playbin3 under the `playbin` name, Qt
5.6's backend creates a `GstPlayBin3` and logs GLib criticals because it connects to `video-changed`,
`audio-changed` and `text-changed`, which playbin3 does not have. Its stream bookkeeping (video available,
track counts) therefore does not update. This is why the switch is limited to adaptive attempts; whether the
picture still reaches `VideoOutput` is step V3.

Risks to check on the device:

- Qt 5.6's backend reads some playbin properties (`n-video`, `current-*`) and stream-changed signals that
  playbin3 lacks (observed above). If the video sink does not attach, the session's watchdog falls back to
  the progressive stream on the stock playbin.
- `dashdemux2` downloads through its own HTTP helper, not through Qt's request, so the format's User-Agent is
  not applied to fragment requests. URLs from the JS-less and TV clients have so far not been bound to the UA
  (SailTune audio plays through souphttpsrc), but this is unverified for `dashdemux2`.
- Requirement RES-16 (draft) reports googlevideo refusing GStreamer's libsoup source in another project
  ("unfixable 403", CLAIMED). SailTune's own field data shows a different pattern: 403 for ~2 s right after a
  full-profile extraction, then 206. SailTube retries once after 4 s. Experiment **E5** settles which HTTP stack
  is needed; if souphttpsrc fails, the media fetcher in ARCHITECTURE §5 replaces it.

## 5. Hardware decoding switch (PLT-08)

`Settings > Hardware decoding` (default off since the SET-01 decision of 2026-11-08; PLY-21). Off sets the `droidvdec` feature rank to none before the first
pipeline exists, so autoplugging never picks it, and the selection treats every codec as software (720p cap).
Without gst-libav on the device, Off can leave H.264 without any decoder; the capabilities page then shows
H.264 "Not available" and selection reports "None of the offered video codecs can be decoded". When no software decoder can play an item (H.264-only videos), the player uses the hardware decoder for that item
and says so in the diagnostics (interim rule, decision log 2026-11-08).

## 6. Device verification for this slice (POCO X3 Pro, SFOS 5.2)

Each step names what to record. Results go to a dated note in `docs/` (method, device, OS, result, decision).

| Step | Action | Record |
|---|---|---|
| V0 | Install the RPM, start SailTube. | Starts; `journalctl --user -f | grep sailtube` shows the probe line (`gstreamer 1.26 loaded=1 playbin3-switch=… adaptive=… h264hw=… vp9hw=…`). |
| V1 | Settings > Video capabilities. | Screenshot: H.264/VP9/AV1 rows, elements list, hardware decoder formats (droidvdec caps). Decides SEL defaults (E3). |
| V2 | Search "sailfish os", scroll to load more. | Results with thumbnails; time to first results. |
| V3 | Open a normal 10-min video (Wi-Fi). | Tap-to-first-frame (stopwatch), stream label under the title (expect `1080p H.264 adaptive`), lip-sync by eye, CPU (`top`). |
| V4 | Seek 5× with the slider; pause/resume. | Lands near target, A/V stay in sync. |
| V5 | Quality menu: pick 360p, 720p, 1080p60 (if offered). | Position kept, label changes. |
| V6 | Set Adaptive streams = Off, restart, play. | Progressive 360p plays: isolates E13. |
| V7 | Pulley: Play audio only; lock the screen 5 min. | Audio continues; MPRIS controls on the lock screen work. |
| V8 | Display off/on 5× during video. | No black or green frames (PLT-07). |
| V9 | A running live stream. | Plays via HLS. |
| V10 | Settings > Hardware decoding off, restart, play 720p. | Plays in software or reports no decoder. |
| V11 | Leave the video page while playing, reopen from "Now playing". | No re-resolve, playback continues. |

Not covered by the slice and therefore not to be tested yet: background video freeze (PLY-09), speed, captions,
chapters ticks, downloads, subscriptions.
