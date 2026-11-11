# E6: remux without ffmpeg (host side)

Date: 2026-11-08. Status: **host done, device pending.** Requirement: DL-03 (spec §3.9). Pass
criterion from the spec: both pairs play and seek, in the system gallery and in SailTube.

## Question

Can the device's own GStreamer join a video-only download and an audio-only download into one
file, with no re-encoding and no bundled ffmpeg? That means an MP4 muxer for H.264+AAC and a
WebM/Matroska muxer for VP9+Opus. If it cannot, the download stays a TrackPair, which SailTube's
dual player plays directly.

## Method

- Implementation: `Core/Downloads/Infrastructure/GstRemuxer.{h,cpp}`. GStreamer is loaded at run
  time with QLibrary through a small copy of its public C ABI. The app does not link GStreamer and
  does not include its headers. The pipeline is built element by element:
  `filesrc ! {qtdemux|matroskademux} ! queue ! {mp4mux|webmmux|matroskamux} ! filesink`, once for
  video and once for audio. The file signature picks the demuxer. File paths are set as `location`
  properties and never go through a pipeline parser. Muxer pads are requested before the pipeline
  starts. Demuxer pads are linked in a `pad-added` handler. Each job runs on its own QThread and
  waits on the bus for EOS or ERROR. The time limit is 60 s plus 10 s per 100 MiB of input. Output
  goes to `out.part` and is renamed on success. On failure or cancel the `.part` is deleted.
- MP4 output gets `faststart=true`, so `moov` comes before `mdat` and the gallery can stream the
  file. mp4mux writes `mdat` to a spool file first. By default that file goes in `g_get_tmp_dir()`,
  which is `/tmp`, which is RAM (PLT-09). So `faststart-file` is set to `out.faststart.part`
  beside the output.
- Host: the `sfos-m1-sailtube` container (the same image as `sfos-m1-native`, plus GStreamer
  devel). Userland is Sailfish OS 5.1.0.11 (Pispala), aarch64. GStreamer is **1.26.11**. The
  headers in that container are not used; only the runtime libraries are loaded.
- Test suite: `tests/remux/run.sh` (`RemuxTest.cpp`, QtCore only). Fixtures are 3.0 s long and
  are encoded at test time by GStreamer. The MP4 fixtures are fragmented, like YouTube's DASH
  tracks. Each output is demuxed again with fakesinks. The test checks for exactly one video
  stream and one audio stream of the expected caps, a duration of 3.0 s ± 0.3 s, `moov` before
  `mdat` for MP4, and that no `.part` file is left behind. The suite also points GLib's `TMPDIR` at
  an empty folder and checks that it is still empty at the end.

## Elements in the container

| | Elements |
|---|---|
| Present | mp4mux, qtmux, qtdemux, webmmux, matroskamux, matroskademux, h264parse, aacparse, vp9parse, opusparse, multifilesrc, avimux, avidemux, vp8enc, vp9enc, opusenc, vorbisenc, videotestsrc, audiotestsrc |
| Absent | x264enc, openh264enc, avenc_h264, v4l2h264enc, avenc_aac, voaacenc, fdkaacenc, faac, droidvenc, droidvdec |

`GstRemuxer::availability()` reports `GStreamer 1.26.11: mp4mux qtmux qtdemux webmmux matroskamux
matroskademux`, so MP4, WebM and Matroska are all available.

## Results

| Case | Result |
|---|---|
| MP4, H.264 + AAC (the DL-03 case) | **SKIPPED on the host.** The container has no H.264 encoder and no AAC encoder to build fixtures with. Covered by the next row for H.264 and by the device steps for AAC. |
| MP4, H.264 + Opus | PASS. The H.264 is a hand-built Baseline stream: 64×64, all macroblocks I_PCM, with VUI timing, timestamped through AVI. Remuxed in about 15–60 ms. Caps out: `video/x-h264, stream-format=avc` and `audio/x-opus`. Duration 3.00 s. Top-level boxes: `ftyp moov uuid mdat` (faststart works). Nothing was written to the temp dir. |
| WebM, VP9 + Opus | PASS. Duration 3.03 s. The output name contains spaces and quotes, which shows paths are never parsed. |
| WebM, VP8 + Opus | PASS. Duration 3.03 s. |
| Matroska, VP9 (WebM) + Opus (MP4) | PASS. Duration 3.00 s. The demuxers were chosen by file signature. |
| MP4 output with VP9 video | Refused with an error: "video/x-vp9 is not muxed into MP4; use WebM or Matroska". No output was written. See the finding below. |
| Missing input file | Fails at once with an error. Nothing is written. |
| Audio-only file given as the video track | Fails in under 25 ms (demuxer not-linked), not by timeout. Nothing is written. |
| Cancel after the muxer starts writing `.part` | Completes with `cancelled`. No output and no `.part` remain. |
| GStreamer library absent (a bogus library name) | `availability().loaded=false`. `remux()` fails in under 20 ms on the caller's thread, both before and after the first probe. |
| Remuxer destroyed with a job running | Returns promptly. The completion is dropped. No `.part` remains. |

**Finding (GStreamer 1.26.11):** feeding mp4mux 4:2:0 VP9 that came from qtdemux crashed the whole
process. The crash was a SIGSEGV in `strcmp` called from `gst_video_chroma_site_from_string`, on the
`mp4mux:src` thread. 4:4:4 VP9 muxed without problems. The likely cause is that qtdemux's VP9 caps
carry no `chroma-site`. Because a crash takes the app down with it, GstRemuxer never links VP8 or
VP9 into an MP4 muxer. DL-03 already sends those codecs to WebM or Matroska, so this costs nothing.

## Decision

- If `availability()` reports the muxer for the pair's container, the engine remuxes:
  - H.264 + AAC → MP4, with faststart;
  - VP9 or VP8 + Opus → WebM;
  - anything else that is supported → Matroska.
- In all other cases the engine keeps the **TrackPair**: GStreamer missing, muxer missing, remux
  failed or timed out. SailTube's dual player plays the pair. A remux failure never loses the
  download.
- At startup the app must set `TMPDIR` before GStreamer or GLib initialise (PLT-09). The remuxer
  itself never writes outside the output folder.

## Device results

**R2 (POCO X3 Pro, SFOS 5.2), POCO run 2, build 5293123, 2026-11-08** (run by the
maintainer):

- Download of *CHARGE - Blender Open Movie* (4:23) at 360p as video: finished in about 10 s as
  `CHARGE - Blender Open Movie [UXqq0ZvbOnk].mp4`, 12.5 MB, with its saved `.jpg` thumbnail, in
  `AppDataLocation/SailTube/downloads` (the DL-06 default; the name follows DL-05).
- The output has one `avc1` video track and one `mp4a` audio track, `moov` before `mdat`, major
  brand `mp42`, and a GStreamer signature in the header. So the separately downloaded DASH H.264
  and AAC tracks were remuxed on the device by `GstRemuxer` into one MP4, with no ffmpeg: **E6's
  H.264+AAC to MP4 path works on R2.**
- The watch page showed "Downloading 100 %" (DL-04).
- Not checked: playback and seeking of the file in the system Gallery (the maintainer does not
  open personal apps) and in SailTube (the new local player, PLY-26, is not in that build).

## Flat files for every MP4-family download

SailTune found on SFOS 5.2 that QMediaPlayer cannot play a DASH-fragmented file from disk (ftyp
"dash", moov + sidx + moof/mdat: "No valid frames decoded before end of stream"). Downloads therefore
never keep DASH fragments: a single-track MP4/M4A download (itag 140 audio, a video-only track) that
scans as fragmented is rewritten by a one-track remux (qtdemux -> mp4mux, faststart) into
`<name>.flat`, checked flat, then renamed over the original. Pair remuxes were already flat. If the
remux fails or no MP4 muxer exists, the file is kept with the remux notice. WebM/Matroska downloads
are not ISO BMFF and are not touched. Host evidence: `tests/remux` "flatten" cases (boxes after:
`ftyp moov uuid mdat`).

## Device steps still pending (R2 POCO X3 Pro on SFOS 5.2, and R1 on 5.1)

1. Check the muxers on SFOS 5.1 and on 5.2: log `GstRemuxer::availability().detail` (or
   `gst-inspect-1.0` for mp4mux, qtmux, qtdemux, webmmux, matroskamux, matroskademux). Record the
   GStreamer version on each.
2. Done at 360p on R2 (see above). Still to do: a **1080p H.264 + AAC** pair (itags 137 + 140),
   recording the time taken and the peak `/tmp` use, which must stay under 1 MiB (PLT-09).
3. Download a real **VP9 + Opus** pair (for example 248 + 251) and remux it to WebM.
4. Play both files and seek in the **system gallery (Videos app)** and in **SailTube** (spec §6
   W2). Repeat in flight mode.
5. Recheck the VP9 crash finding on 5.2. Verify that an H.264 High-profile 4:2:0 input muxes
   without the same crash. On the host, only Baseline I_PCM H.264 could be tested.
6. If both play and seek, mark E6 passed. If not, record which step failed. The engine still falls
   back to the TrackPair.
