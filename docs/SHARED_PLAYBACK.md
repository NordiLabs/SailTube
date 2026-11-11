# Proposal: shared instant-playback component (SailTune ↔ SailTube)

Status: APPROVED by the user on 2026-11-08 and implemented: SailTune produced the component, SailTube pins it
(`Shared/SailTune/`, 5611210 since 2026-11-10) and adapts it in `Core/Player`. The SailTune side is not changed by
SailTube; open requests are in `docs/SHARED_REQUESTS.md`.

## Problem

Both apps need the same tap-to-stream path (INSTANT_PLAYBACK_SPEC.md): warm resolver, direct
`/youtubei/v1/player` fast path with the client definition read from bundled yt-dlp, probe with the
real first request, loopback fetcher in ~10 MiB Range chunks with resume-on-403, resolve cache with
prefetch, breaker, yt-dlp fallback chain. SailTune's streaming component built it first for audio.
Building it twice would waste work and drift.

## Proposal

1. **One producer.** SailTune's streaming component owns the implementation. SailTube does not write a
   second resolver or fetcher.
2. **Boundary.** The component is Qt Core/Network only (no QML, no QtMultimedia, no app types), in a
   directory SailTune keeps self-contained (for example `Shared/Playback/` or
   `Core/Streaming/Instant/`), with a stable C++ interface:
   - `StreamResolver::resolve(videoId, StreamRequest) → StreamSet`. A `StreamRequest` holds the
     audio-only/video flag, quality ceiling, preferred codecs, dub language and HW-decode flag
     (these are part of the cache key). A `StreamSet` holds ranked video-only formats, ranked
     audio-only formats, the muxed fallback, HLS for live, `expire`, `loudnessDb`, the resolve
     path used and timings.
   - `StreamFetcher::open(format) → local URL or appsrc feeder`: bounded Range chunks, byte-offset
     resume, read-ahead cap, host allowlist.
   - `prefetch(videoId, request)`, `invalidate(...)`, diagnostics counters (fast / yt-dlp / failed,
     median timings).
   - Pure decision helpers (format ladder, error classification, breaker) testable offline.
3. **Consumption.** SailTube pins the files with its existing `Shared/SHARED_MANIFEST.json` +
   `tools/shared_sync.py` (already used for SailTune's resolver runtime). No edits in SailTube; drift
   is reported by `--check --upstream`.
4. **Video additions.** The audio-first implementation must not block video: the format ladder takes
   a "video-only formats wanted" flag, and the fetcher serves two streams at once (video + audio).
   SailTube asks for these through the maintainer if SailTune's first version lacks them.
5. **Reverse direction.** SailTube's playback component builds the GStreamer video pipeline (dual
   `uridecodebin`/`appsrc` branches, per-container seeking, hw decode, background freeze). It is kept
   free of SailTube types, so SailTune can pin it later if it moves its audio off QtMultimedia
   (COMPARISON §2.2 recommends that).
6. **Later option.** If both apps pin many files, move the component to its own repository
   (same owner, PolyForm-Noncommercial) that both pin. Not needed now.
