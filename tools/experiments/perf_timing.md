<!-- SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0 -->
# Timing markers and performance collection (NFR-PERF-02..06, DIA-03, §6 P1–P4, P7, T1)

Status: prepared, not run (2026-11-08). The app does not print these markers yet; the lead adds
them. `perf_summary.py` already parses them (tested by `tests/perf_summary/run.sh`).

## Marker grammar

One line per event, anywhere in a line (journal prefixes are fine):

```
sailtube-timing: <event> t=<ms> [key=value ...]
sailtube-sample: <kind> t=<ms> [key=value ...]
```

* `t` is an integer in milliseconds from a monotonic clock (`QElapsedTimer`, which is
  `CLOCK_MONOTONIC` on Linux). Never wall-clock time. Only differences inside one app process are
  used, so the origin does not matter.
* Values hold no spaces. They never contain cookies, tokens, `pot=`/`sig=` or stream URLs (DIA-02).
* The app prints `sailtube-timing:` only with the DIA-03 timing log on (default off), through
  `qInfo` and into the exported timing file. The `sailtube-sample:` lines come from the device
  samplers (`pss.sh`, `cpu_sample.sh`), whose `t` is ms since boot (`/proc/uptime`).

## Events the app should print

| Event | When exactly | Keys |
|---|---|---|
| `launch` | first line of `main()` | — |
| `tap` | the user's release on a video row or play control that starts a play | `run=<n>` (per-process counter), `net=wifi\|4g\|other` |
| `queue-done` | the resolve job leaves the queue (DIA-02 phase "queue wait") | `run` |
| `post-done` | the `/youtubei/v1/player` answer arrived (phase "POST") | `run` |
| `warmup-start` | the shared resolver starts minting the visitor id and opening its connection (after the first screen is up, off the UI thread) | — |
| `warmup-done` | the visitor id is ready (or the warm-up failed: `ok=0`) | `ok=0\|1`, `visitorMs` |
| `resolve-start` | the session asks the resolver for the play plan | `run` |
| `resolve-done` | the play plan is known | `run`, `path=fast\|extractor\|prefetch\|cache` |
| `first-byte` | the first media buffer of a source branch reached the pipeline (through MediaProxy: first byte from loopback) | `run`, `branch=video\|audio\|muxed` |
| `probe-done` | the first media request answered (phase "probe") | `run` |
| `demux-ready` | the branch's demuxer added its first pad (its headers are parsed: mp4 `moov`, WebM segment info / tracks) | `run`, `branch=video\|audio\|muxed` |
| `start-seek` | the start position (resume, switch) or a remembered speed was sent as a seek from the branch's held decoder, before anything was decoded (PLY-06; absent for a start at 0 at 1×) | `run`, `branch` |
| `first-decoded` | the first decoded video buffer left the video decoder (software decoder or droidvdec) | `run`, `branch=video` |
| `preroll-done` | first pipeline async-done (phase "preroll") | `run` |
| `first-frame` | the first video frame is on screen (scene-graph frame swapped with the video texture, not the decoder output) | `run` |
| `teardown-done` | the previous pipeline reached NULL and was freed on the player's teardown worker (the GUI thread does not wait for it). `t` is the event time; `ms` is the time since that stop / reload | `run` (of the stopped playback), `ms` |
| `seek-start` | a user seek is accepted by the controller (scrubber release, double tap) | `seek=<n>`, `target=<ms>`, `container=webm\|mp4\|muxed\|hls`, `inbuf=0\|1` (target inside the buffered range), `net`, and `burst=<b>` for every request of one double-tap burst |
| `seek-flush` | every flush-seek actually sent to the pipeline | `seek`, `container`, `accurate=0\|1`, `burst=<b>` when part of a burst |
| `seek-done` | playback runs again at the landed position | `seek`, `landed=<ms>` |
| `switch-start` | a quality or dub switch is requested | `switch=<n>`, `kind=quality\|dub`, `net` |
| `switch-resumed` | playback runs again after the switch | `switch` |
| `hidden` | the application became inactive | — |
| `video-decode-stopped` | the video branch is frozen (PLY-09) | `mode=dual\|muxed\|hls` |
| `shown` | the application became active again | — |
| `speculative-start` | E12: a resolve starts on touch-down | `run=<press id>` |
| `speculative-cancel` | E12: the press did not become a tap; the result is dropped | `run` |
| `speculative-used` | E12: the tap reused the speculative result | `run` |

Where they come from: `VideoPlayer::timing(marker, ms)` emits `demux-ready branch=<b>`,
`start-seek branch=<b>`, `first-decoded branch=video` (marker text = event name plus its keys,
`ms` since `load()`) and `teardown-done` (`ms` since that teardown, not since `load()`); the session
prints `sailtube-timing: <event> t=<now> run=<n> <keys>`. Together with `first-byte` they split
`first-byte → preroll-done` into network (`first-byte → demux-ready`: init segment / headers),
seek (`demux-ready → start-seek → first-decoded`: the range request for the start position) and
decode + sink (`first-decoded → preroll-done → first-frame`). With the start seek before preroll
there is no `seek-start`/`seek-done` pair for the resume position any more.

`queue-done`, `post-done`, `probe-done`, `preroll-done` and `first-frame` are the five DIA-02
phases, so one line per phase satisfies DIA-03's acceptance. `perf_summary.py` uses `tap`,
`resolve-done`, `first-frame`, `launch`, the seek, switch, background and speculative events.

### Reading the split on mp4 (R2 run 5, host-reproduced in `tests/player_gst`)

* `first-byte → demux-ready` of a DASH mp4 branch (disk-backed, qtdemux in pull mode) used to hold
  two extra range requests: qtdemux reads the `mfro` box at the end of a fragmented file before its
  first pad, and the download buffer serves that with a request to the end of the file and one back
  (run 5: 400–820 ms for video vs 10–50 ms for the WebM audio). The engine now keeps the upstream
  size from the demuxer until its first pad (`BranchPlan::hideSizeUntilDemuxReady`); the branch
  makes one request and demux-ready follows the first byte. Evidence:
  `PLAYER_GST_ONLY=ply04_fragmentedMp4RequestLog sh tests/player_gst/run.sh` prints every range
  request per mode.
* `start-seek branch=video → start-seek branch=audio` on a dual mp4 video + WebM audio resume is
  the video's landing: PLY-03 sends the audio to where the video's key-unit seek landed. Deep in a
  DASH mp4 without an `mfra` index, qtdemux reaches the target by parsing moof after moof, each one
  waiting for the download buffer's sequential download (run 5: 3.7 s for 8:55). `first-decoded`
  coincides with the audio start seek because both follow that landing. The first frame no longer
  waits for the audio branch itself (it is posted once the video branch's start seek is out).
* The `sailtube: decoders video= audio=` line (first preroll) also carries `unrouted=<caps,...>`
  (streams that matched no route and went to a fakesink) and `video-output=on|off` (whether frames
  reach the mailbox at that preroll). No new timing marker was added.

## What perf_summary.py computes

`python3 -I tools/experiments/perf_summary.py [--tsv] [--keep-warmup] [--baseline OFF.log] LOG...`

| Metric | Pairs | Budget (spec §4.1) | Check |
|---|---|---|---|
| `ttff.fast.wifi` / `ttff.fast.4g` | `tap` → `first-frame` by `run`, path from `resolve-done` | p50 ≤ 1200 / p95 ≤ 2500; p50 ≤ 2000 / p95 ≤ 4000 | NFR-PERF-02 |
| `ttff.prefetch` | same, `path=prefetch` | p50 ≤ 600 | NFR-PERF-02, §6 P2 |
| `ttff.extractor` | same, `path=extractor` | p50 ≤ 6000 / p95 ≤ 12000 | NFR-PERF-02 |
| `ttff.early.<group>` | taps ≤ 5 s after `launch` | group budget + 500 ms | NFR-PERF-02 |
| `fast-path-share` | fast vs extractor taps | ≥ 90 % | §6 P1 |
| `seek.inbuf` | `seek-start` → `seek-done` by `seek`, `inbuf=1` | p50 ≤ 300 | NFR-PERF-03 |
| `seek.out.<webm\|mp4>.<wifi\|4g>` | same, `inbuf=0` | max ≤ 1500 / 3000 (WebM), ≤ 2000 / 4000 (mp4) | NFR-PERF-03, PLY-04 |
| `seek.burst-flushes` | `seek-flush` per `burst` | each burst exactly 1 | NFR-PERF-03, PLY-03 |
| `seek.mp4-accurate` | `seek-flush container=mp4 accurate=1` | 0 | PLY-04 |
| `switch.wifi` | `switch-start` → `switch-resumed` | max ≤ 2000 | NFR-PERF-04 |
| `background.decode-stop` | `hidden` → `video-decode-stopped` | max ≤ 1800 (1 s + 800 ms grace) | NFR-PERF-06, PLY-09 |
| `pss.playback-1080p-sw`, `pss.feed` | `pss` samples | max ≤ 300 MB / 180 MB (10^6 bytes) | NFR-PERF-05 |
| `spool.<context>` | `spool` samples | max ≤ 512 MiB | NFR-PERF-05, RES-18 |
| `cpu.background-*` | first/last `cpu` sample | mean ≤ 8 % of one core over ≥ 600 s | NFR-PERF-06 |
| `wakeups-per-s.<context>` | context switches per second | informational | NFR-PERF-06 (timer check is not decided by this number) |
| `speculative.waste`, `e12.saved-p50.*` | E12 | waste ≤ 30 %, saved ≥ 150 ms | E12, RES-11 |

Interpretations chosen (the spec leaves them open): nearest-rank percentiles; the first run of
each series is the discarded warm-up (spec §0), except early taps, which are one per launch; a
percentile budget with fewer than 20 runs is `INCONCLUSIVE`; single-number budgets apply to every
run (checked on max), as PLY-04's acceptance reads "each completes within"; MB = 10^6 bytes.

## Device procedure (maintainer, with the phone owner's yes)

All device scripts run `exp_begin` (mute + verify, window guard) first; see
`docs/experiments/README.md`.

1. **Enable the timing log** (DIA-03 setting) in the build under test. Questions for the lead: the
   timing file path (proposal: `~/.cache/org.nordilabs.sailtube/SailTube/timing.log`) and how it is
   exported (T1). Fallback: `journalctl --user -o cat | grep sailtube-` if the user journal holds the
   app's `qInfo` output; that is unverified on SFOS 5.x and the system journal needs root.
2. **P1**: one warm-up tap, then 20 fresh popular videos on Wi-Fi; repeat on 4G (`net=` comes from
   the app's network class). Then 20 cold launches with a tap within 5 s for `ttff.early.*` (optional,
   long).
3. **P2**: a playlist of 21+ items; let each start by autoplay → `ttff.prefetch`.
4. **P3**: 1 + 20 in-buffer seeks, 1 + 10 out-of-buffer scrubber seeks and 3 double-tap bursts on a
   WebM and on an mp4 video, on Wi-Fi (and 4G when available).
5. **P4**: 1 + 3 quality switches and 2 dub switches on Wi-Fi.
6. **NFR-PERF-05**: during 1080p software playback,
   `sh pss.sh -c playback-1080p-sw -i 5 -d 300 -s <spool dir>`; on the feed, `sh pss.sh -c feed -d 120`.
   Question for the lead: the spool directory path (RES-18).
7. **P7 / NFR-PERF-06**: start a video with Opus audio, start
   `sh cpu_sample.sh -c background-opus -i 10 -d 600`, lock the screen at once, unlock after the
   sampler ends. The `hidden` → `video-decode-stopped` pair is in the timing log. Repeat with
   `-c background-muxed` and `-c background-hls`. The sampler wakes at most once per interval
   (minimum 1 s), so it cannot add a > 1 Hz wake-up of its own to the app.
8. **T1**: copy the timing file, `pss.log` and `cpu.log` back to the host and run
   `python3 -I tools/experiments/perf_summary.py timing.log pss.log cpu.log > summary.txt`. Attach
   both to the release notes.

Open questions: whether PSS should include helper children (the Python resolver and QuickJS
children reuse the app binary; `pgrep -o` picks the oldest process, the GUI); how to verify "no app
timer above 1 Hz" (the context-switch rate counts audio-thread wake-ups too; a QML/Qt timer audit
or `perf` may be needed); how to observe "network stops within the read-ahead cap" per process
(`/proc/<pid>/net/dev` is per namespace, not per process; the spool size line is the proxy here).

## Events added by the host harness (tools/perf_playback.sh, docs/PERF_PLAYBACK.md)

Printed by `tools/perf_playback/PerfPlayback.cpp` only (the app does not print them yet). Every
line also carries `run=<n>` (unique in one harness log, so `tap`/`first-frame` pairing by `run`
still works), `round=<r>`, `video=<id>` and `mode=cold|warm|cached`. In the harness `t` is ms from
the round's `tap`, and `first-frame` is the first decoded frame at the appsink (no scene graph on
the host), not the on-screen swap.

| Event | When exactly | Keys |
|---|---|---|
| `visitor-done` | the visitor id mint answered (only when the resolve had to mint one) | `ok=true\|false` |
| `rung-probe-done` | the host's own probe of a delivered rung other than the component's probed top (RES-06) finished | `verdict=accept\|reject\|unusable\|none\|timeout`, `rungs`, `redirects` |
| `proxy-open` | both branches are registered with MediaProxy and the player is about to load | — |
| `proxy-request` | the player's HTTP source reached the loopback proxy (first 200/206 served) | `branch=video\|audio` |
| `upstream-first-byte` | the proxy received its first media byte from the CDN | `branch`, `sinceRequestMs` |
| `first-parsed-sample` | fallback when no decodable rung is offered: first buffer after the demuxer (parsebin) | `branch` |

Keys added to existing events: `post-done ok= reused=true|false` (the `/player` POST went over a
pooled connection), `probe-done verdict= redirects=` (CDN redirects the probe followed),
`resolve-done path= totalMs= visitorMs= playerMs= parseMs= probeMs= reused=0|1` (the component's
`StreamTimings`). Non-timing lines start with `sailtube-perf:` (`start`, `video`, `rungs`,
`primed`, `result ... ok=0|1 reason=<class>`, the summary) and are ignored by `perf_summary.py`.
