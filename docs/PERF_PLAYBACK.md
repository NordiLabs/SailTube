<!-- SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0 -->
# Host playback performance: tap to picture against live YouTube (NFR-PERF-02)

Status: measured on the host on 2026-11-08 (no phone); the re-pin and the pre-buffer A/B are later sections (2026-11-10 and 2026-11-11). Harness: `tools/perf_playback.sh`
(source `tools/perf_playback/PerfPlayback.cpp`). Marker grammar: `tools/experiments/perf_timing.md`
(the harness adds `visitor-done`, `rung-probe-done`, `proxy-open`, `proxy-request`,
`upstream-first-byte`, `first-parsed-sample`). The numbers below are for the component pinned at
`fd49627`; the before/after of the re-pin to SailTune `5611210` is in "Re-pin 2026-11-10".

## Summary

| Tap to picture (ms) | median | p90 | p95 | max | rounds ok / skipped |
|---|---:|---:|---:|---:|---|
| **cold** (new process: no visitor id, no connection, no cache) | **533** | 779 | 966 | 1517 | 38 / 2 |
| **warm** (visitor id minted, `/player` connection pooled) | **376** | 525 | 611 | 714 | 40 / 0 |
| **cached** (the resolver's TTL cache answers: prefetch / speculative resolve) | **101** | 235 | 303 | 362 | 40 / 0 |

On the host, every NFR-PERF-02 Wi-Fi budget (p50 ≤ 1.2 s, p95 ≤ 2.5 s; prefetched p50 ≤ 0.6 s) and
the user's goals (cold < 2 s, warm < 1 s) are met with room to spare. The phone adds what the host
does not have (see Caveats): mobile RTTs on every handshake, an ARM CPU, the QML/thread hop and the
scene-graph frame. Of the user's goals, **warm < 1 s on R2 over 4G is the tight one**: the warm path
still has about ten network round trips in it, and the three cheapest cuts remove most of them
(prefetch/speculative resolve, one TLS handshake fewer on the media side, no second probe).

Where the time goes, ranked (pooled cold + warm):
1. **`/player` POST**: 161 ms cold, 127 ms warm (32 % / 36 %). Server time plus one round trip.
2. **Visitor id mint**: 151 ms, cold only (29 %). DNS + TCP + TLS + one POST.
3. **First media bytes**: 79–102 ms (17 % / 29 %; 91 % of a cached tap). A *new* TLS
   connection to the CDN node from MediaProxy, then the time to first byte.
4. **Probe of the top audio + video**: 74–78 ms (15 % / 25 %). Two *more* new TLS connections to
   the same CDN, closed again right after the headers.
5. **Host rung probe**: 52–74 ms in the 40 % of rounds where SailTube's ladder picks a rung other
   than the one the component probed (5–8 % on average).
6. Demux + VP9 decode of the first frame: 6–8 ms. Loopback hop (player → MediaProxy): 5 ms.
   Resolve hand-over: 2 ms. Negligible on the host.

## Method

* One binary links the pinned SailTune component (`FastStreamResolver`, `MediaProxy`, sources as
  in `Shared/SharedPlayback.pri`) and the app's `VideoPlayer`/`GstEngine` (as in
  `tests/player_gst/run.sh`). No SailTube app code, no adapter (`SharedStreamSource*` is not used).
* Per round: `tap` (t = 0) → `resolve()` with `StreamRequest{audioOnly=false, maxVideoHeight=720,
  codecPreference={opus}, hardwareDecode=false}` → rung choice → host probe of a rung the component
  did not probe (`probeAddress`, as RES-06 and the adapter do; the address that answered is used and
  remembered per process) → `MediaProxy::open` for video and audio → `VideoPlayer::load` with a
  dual plan (VP9 WebM + Opus WebM, both through the loopback proxy, `fakesink sync=true` audio,
  the engine's appsink video, software decode) → `first-frame`. The two first fetches start in
  parallel (the player starts both sources at once; `proxy-request.video` and `.audio` arrive within
  1 ms of each other).
* Rung choice (SEL-02 on the host): the tallest VP9 rung at or below the 720p ceiling, ≤ 30 fps
  first at equal height, then bitrate; the first Opus rung. The container has no software H.264
  decoder; the phone's software path would take H.264 at the same height. The "first parsed
  sample" fallback (souphttpsrc ! parsebin ! fakesink) exists for videos without a VP9 + Opus pair;
  it was never needed.
* Stage times come from the component's `Options::log` lines (time-stamped on arrival), one
  `MediaProxy` per branch (so its log lines can be told apart; each stream has its own upstream
  connection either way), and the player's `firstByte`, `timing("preroll")` and `firstFrame`
  signals. `StreamTimings` are printed as keys on `resolve-done`.
* Modes: **cold** = one child process per round (fresh DNS, TLS, visitor, cache, breaker).
  **warm** = one process primed with another video (`jNQXAC9IVRw`), the measured video's cache
  entry invalidated before each round. **cached** = right after the warm round of the same video,
  no invalidation (path `cache`). Cold rounds run first, then warm/cached interleaved per video.
* Data: run 1 (3 rounds) + run 2 (5 rounds) × 5 videos × 3 modes = 120 rounds, pooled. Nearest-rank
  percentiles. Run 3: 3 rounds at a 360p ceiling (low-rung comparison).
* A bot wall, an unplayable video, a probe refusal or a timeout skips that round with
  `sailtube-perf: result ... ok=0 reason=<class>`; the run exits 0 when at least 3 videos produced
  numbers.

### Videos

| Id | Title (from the `/player` answer) | Rung played at 720p ceiling |
|---|---|---|
| `aqz-KE-bpKQ` | Big Buck Bunny 60fps 4K - Official Blender Foundation Short Film | 302 VP9 720p60 1420k + 251 Opus 128k |
| `eRsGyueVLvQ` | Sintel - Open Movie by Blender Foundation | 247 VP9 1280×546 24 fps 587k + 251 Opus |
| `R6MlUcmOul8` | Tears of Steel - Blender VFX Open Movie | 247 VP9 534p24 604k + 251 Opus |
| `WhWc3b3KhnY` | Spring - Blender Open Movie | 247 VP9 536p24 534k + 251 Opus |
| `Y-rmzh0PI3c` | Cosmos Laundromat - First Cycle. Official Blender Foundation release. | 247 VP9 536p24 696k + 251 Opus |

Warm-up video (not measured): `jNQXAC9IVRw` ("Me at the zoo"). The widescreen films' "720p" rungs
are 1280 wide and 534–546 tall; the ceiling compares heights, as the component does.

### Environment and caveats

* Host: Apple M1 Max (10 cores), container `sfos-m1-sailtube` (Sailfish OS SDK userland, aarch64,
  OrbStack VM), GStreamer 1.26.11, Qt 5.6.3, gcc 13.4 `-O2`. Desktop-class CPU: TLS, JSON parsing
  (`parseMs` 0–2 ms here) and libvpx software decode are far faster than on R2.
* Network: the Mac's uplink through OrbStack's NAT, not phone Wi-Fi or 4G. Measured from the
  container: TCP connect 18–70 ms, TLS done 80–200 ms, `generate_204` first byte 96–213 ms. A
  phone on 4G typically has 2–3× the RTT and more jitter; every handshake below scales with it.
* `first-frame` here is the first decoded frame at the appsink. On the phone it is the scene-graph
  swap (one vsync or more later) and, with hardware decode, droidvdec's start-up.
* Not in these numbers: process start / GStreamer registry load (done before the round), the QML
  tap → C++ hop and the resolver thread hop (`ThreadedStreamSource`), PulseAudio start, the
  extractor fallback.
* The CDN edge is warm for these popular films after the first round; per-round variation is
  mostly network (cold max 1517 ms was one slow `/player`).

## Stage table (ms from the tap, runs 1 + 2 pooled)

| Stage | cold median | cold p90 | warm median | warm p90 | cached median | cached p90 |
|---|---:|---:|---:|---:|---:|---:|
| `visitor-done` | 151 | 263 | — | — | — | — |
| `post-done` (`/player` answered, parsed) | 328 | 523 | 127 | 191 | — | — |
| `probe-done` (top audio + video probed) | 419 | 595 | 231 | 295 | — | — |
| `resolve-done` | 422 | 596 | 232 | 300 | 2 | 4 |
| `rung-probe-done` (16 of 40 rounds) | 438 | 787 | 294 | 415 | — | — |
| `proxy-open` (player load) | 435 | 661 | 254 | 381 | 3 | 4 |
| `proxy-request.video` (player reached the proxy) | 440 | 666 | 271 | 388 | 8 | 36 |
| `upstream-first-byte.video` | 525 | 772 | 368 | 509 | 90 | 213 |
| `upstream-first-byte.audio` | 524 | 765 | 353 | 472 | 89 | 157 |
| `first-byte.video` (player's signal) | 525 | 773 | 370 | 511 | 90 | 213 |
| `first-byte.audio` | 525 | 767 | 360 | 474 | 92 | 177 |
| **`first-frame`** | **533** | **779** | **376** | **525** | **101** | **235** |
| `preroll-done` | 531 | 763 | 382 | 579 | 101 | 267 |

Run 2 alone (5 rounds): cold 516 / 699, warm 376 / 611, cached 101 / 240 (median / p90).

## Critical-path segments and share of the total

A stage that did not happen (no mint when warm, no network on a cache hit) has zero width, so the
segments add up to `first-frame`. Share = sum of the segment over all rounds / sum of the totals.

| Segment | cold median | p90 | share | warm median | p90 | share | cached median | p90 | share |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| Visitor id mint | 151 | 263 | 29 % | 0 | 0 | 0 % | 0 | 0 | 0 % |
| `/player` POST (+ parse) | 161 | 253 | 32 % | 127 | 191 | 36 % | 0 | 0 | 0 % |
| Probe of top audio + video | 74 | 155 | 15 % | 78 | 135 | 25 % | 0 | 0 | 0 % |
| Resolve hand-over | 2 | 4 | 0 % | 2 | 4 | 1 % | 2 | 4 | 3 % |
| Host rung probe | 1 | 66 | 5 % | 1 | 85 | 8 % | 0 | 1 | 0 % |
| First media bytes (both branches) | 79 | 185 | 17 % | 102 | 158 | 29 % | 92 | 226 | 91 % |
| Demux + VP9 decode to appsink | 8 | 12 | 1 % | 6 | 16 | 2 % | 6 | 17 | 7 % |
| **Tap to picture** | **533** | **779** | 100 % | **376** | **525** | 100 % | **101** | **235** | 100 % |

Inside "first media bytes": loopback (`proxy-open` → `proxy-request`) 5 ms; upstream TLS + time to
first byte 66–92 ms; proxy → player first byte 0–1 ms. When the host rung probe runs, it costs a
median 52 ms (cold) / 74 ms (warm), p90 ~200 ms.

Other observations:

* The cold `/player` POST rides the visitor mint's connection (`reused=true` in all rounds) and
  still costs ~35 ms more than warm (161 vs 127 ms): a young TCP connection (slow start) for an
  answer of a few hundred KB.
* Redirects: the smoke runs before run 1 saw the probe follow 1–2 googlevideo redirects and MediaProxy one;
  the pooled runs saw none. A redirect costs one more TLS handshake (~60–100 ms here) per branch.
* **Probe refusals: 2 of 55 cold rounds** (runs 1–3) got HTTP 403 on the component's probe of a
  freshly issued address (`probe_rejected/http_403`); 0 of 95 warm/cached rounds. In the app that
  tap goes to the extractor (NFR-PERF-02 extractor budget: 6 s), and each refusal is a breaker
  strike (two in ten minutes switch the fast path off for 30 min).
* Low-rung start (run 3, 360p ceiling: VP9 243/244, 210–334 kbit/s): cold 605 / 698, warm 339 / 485,
  cached 96 / 175 (median / p90): no measurable gain over 720p on this network. Time to first
  frame is latency-bound (TTFB), not bandwidth-bound: the first frame needs the init segment and
  the first cluster, which arrive 6–8 ms after the first byte.

## Budgets

| Budget (perf_timing.md / spec §4.1) | Host result | Verdict on the host | Phone headroom left |
|---|---|---|---|
| NFR-PERF-02 fast path Wi-Fi: p50 ≤ 1.2 s, p95 ≤ 2.5 s | warm p50 376, p95 611; cold p50 533, p95 966 | pass | ~0.8 s at p50 (warm) |
| NFR-PERF-02 4G: p50 ≤ 2.0 s, p95 ≤ 4.0 s | (no 4G here) | — | see estimate below |
| NFR-PERF-02 prefetched item: p50 ≤ 0.6 s | cached p50 101, p95 303 | pass | ~0.5 s |
| NFR-PERF-02 first tap ≤ 5 s after launch: budget + 0.5 s | cold p50 533 (pre-warm not modelled) | pass | — |
| User goal: tap to picture < 2 s cold on R2 | cold p50 533, p95 966 | pass on host | ~1.4 s |
| User goal: < 1 s warm on R2 | warm p50 376, p95 611 | pass on host | ~0.6 s |

Rough phone estimate (INFERENCE, not measured): the warm critical path has about 10 round trips
(`/player` 1 + server time; probe: TCP + TLS + request ≈ 3–4; rung probe 3–4 when needed; media
fetch 3–4) and the cold one about 14–17 (plus DNS and the mint). At a 60–80 ms 4G RTT plus
~100–150 ms of server time, ~150–300 ms of phone CPU, thread hops and the first scene-graph frame,
warm lands near 0.9–1.2 s and cold near 1.5–2.0 s on 4G; on good Wi-Fi (20–30 ms RTT) about 0.6 s
and 0.9 s. So warm < 1 s on 4G needs the round trips below removed; cold < 2 s needs the pre-warm.

## Optimisation candidates (ranked by expected saving on R2)

Done since this table was written: prefetch and speculative resolve feeding the TTL cache (RES-11, E12; used on the phone in POCO run 6), visitor and connection pre-warm (RES-21), and parallel first bytes of both branches. The rows below are what is left; their numbers stay.

| # | Candidate | Stage it removes (host numbers) | Expected saving | Notes / owner |
|---|---|---|---|---|
| 3 | **One CDN handshake instead of three: reuse the probe's TLS connection for playback's first chunk (or skip the separate probe and let the first chunk be the probe)** | probe 74–78 ms + part of first media bytes (TLS ~40–60 ms) | host 70–110 ms; R2 4G 200–300 ms (2 handshakes ≈ 4–6 RTT) | The probe opens a new connection per rung and closes it after the headers; MediaProxy opens another one to the same host. Shared component (request below). |
| 4 | **Probe the rung the host will play** (pass the SEL-02 video order / codec into `StreamRequest`, or let the host name the rungs to probe) | host rung probe: 52–74 ms in 40 % of taps, p90 ~200 ms | host mean ~25 ms, worst ~200 ms; R2 4G 150–250 ms in those taps | On the phone SEL-02 prefers H.264 for software decode while the component's ladder orders by height, fps, bitrate: the mismatch will be at least as frequent. Shared component + adapter. |
| 5 | **Retry a fresh-address 403 in the probe once (~500 ms) before `ProbeRejected`** | the 4 % of cold taps that fall to the extractor (≥ 6 s) and their breaker strikes | mean ~0.2 s per cold tap; removes a path to the 30 min breaker | MediaProxy already retries fresh-address refusals every 500 ms for 6 s; the probe does not. Shared component. |
| 7 | Low-rung start, then switch up | none measurable (360p vs 720p within noise) | ~0 on Wi-Fi; on 4G maybe the transfer of the first cluster (tens of ms) | First frame is latency-bound. Not worth a forced quality switch for start-up; keep SEL-02's mobile ceiling (480p). |
| 8 | Small first ranges | none: the first byte arrives at TTFB whatever the range size | ~0 | The 10 MiB chunk does not delay the first byte; it only bounds memory. |
| 9 | Low start buffer | demux + decode + preroll is 6–8 ms after the last first byte | ~0 on the host; check on R2 with droidvdec / pulsesink | The player already prerolls on the first frame. Re-measure on the phone (BufferingPolicy, queue2 thresholds). |

Combined: 3 + 4 cut an unpredicted warm
tap from ~10 round trips to ~5 (`/player` + one CDN handshake + request), which is what brings warm
under 1 s on 4G.

## Hooks requested (measured around them for now)

Shared component (SailTune, pinned; requests for `docs/SHARED_REQUESTS.md` via the lead):
* A typed timing hook (`std::function<void(Phase, int ms, ...)>`) instead of parsing `Options::log`
  text: the harness keys on the strings `phase visitor ok`, `phase player ok`,
  `phase probe redirect hop` and `phase probe verdict`, which can change without notice.
* `StreamTimings`: the probe's redirect count, connect/TLS vs time-to-first-byte split, and
  `reusedConnection` for the visitor POST.
* `MediaProxy`: a stream identity (token prefix or branch tag) in its log lines, or a per-stream
  first-byte callback; the harness uses one proxy per branch to tell them apart.
* Candidate 3 (probe connection handed to MediaProxy, or TLS session resumption across
  `HttpsConnection`s to the same host), candidate 4 (host-chosen probe rungs / video codec order in
  `StreamRequest`), candidate 5 (fresh-address 403 retry in the probe).

Player (`VideoPlayer`): nothing blocking. `firstByte` arrives within 1 ms of the proxy's upstream
first byte; `first-frame` is the decoder output on the host. Nice to have: a `first-sample`
(first buffer after the demuxer) marker per branch, so a device without a decoder for the chosen
codec can still be timed through the real player instead of a separate parsebin pipeline.

## Re-pin 2026-11-10 (shared component `fd49627` → `5611210`)

What the re-pin changes on the path the harness measures: `MediaProxy` now fetches the first
256 KiB of a stream at `open()` over a connection of its own (pre-buffer) and its last 16 KiB over
another (tail cache), answers the player's first request from that memory, asks upstream in
growing chunks (512 KiB, then x4 up to 10 MiB) and keeps idle upstream connections for the next
job. The resolver (visitor mint, `/player`, parser, ladder, probe) is unchanged; its new
`peekCached` is not called by the harness.

Method: the same command twice, 25 minutes apart, with the app's player at `f9af7d6` in both
builds and only the pinned component different:
`tools/perf_playback.sh --videos aqz-KE-bpKQ,eRsGyueVLvQ,R6MlUcmOul8,WhWc3b3KhnY,Y-rmzh0PI3c,M7lc1UVf-VE,YE7VzlLtp-4`
(the five films above plus the two ids the maintainer saw with probe redirects), 3 rounds, so
7 videos x 3 rounds x 3 modes = 63 rounds per run. All 126 rounds produced a picture (no skip, no
probe refusal). Container load average 3.9 (before) and 3.5 (after) from other builds.

| Tap to picture (ms) | before `fd49627` median | p90 | after `5611210` median | p90 |
|---|---:|---:|---:|---:|
| **cold** | **1149** | 2804 | **1534** | 2383 |
| **warm** | **681** | 913 | **920** | 1349 |
| **cached** | **204** | 330 | **245** | 345 |

Both runs are slower than 2026-11-08 (533 / 376 / 101): the uplink was slower on this day, and
seven videos instead of five.

Where the difference is (median / p90, ms):

| Segment | Code changed by the re-pin | cold before | cold after | warm before | warm after | cached before | cached after |
|---|---|---:|---:|---:|---:|---:|---:|
| Visitor id mint | no | 223 / 352 | 307 / 595 | — | — | — | — |
| `/player` POST (`playerMs`) | no | 197 / 374 | 288 / 395 | 132 / 271 | 160 / 243 | — | — |
| Probe of top audio + video (`probeMs`) | no | 294 / 1206 | 286 / 1073 | 249 / 455 | 295 / 658 | — | — |
| `resolve-done` (from the tap) | no | 807 / 1572 | 988 / 1714 | 393 / 612 | 445 / 882 | 2 / 4 | 3 / 7 |
| `proxy-open` → first bytes of both branches | **yes** | 206 / 506 | 260 / 398 | 132 / 265 | 209 / 415 | 233 / 1128 | 237 / 330 |
| First bytes → first frame | no | 27 / 78 | 29 / 61 | 10 / 15 | 11 / 38 | 7 / 15 | 11 / 19 |

Reading:

* **No gain on the host.** The network was slower during the second run: the stages whose code
  did not change are 30–90 ms slower at the median (visitor mint +84, `/player` +91 cold / +28
  warm), and they make up +181 ms (cold) and +52 ms (warm) of the difference up to `resolve-done`.
* The one segment the re-pin changes, `proxy-open` → first bytes, did not get faster: +54 ms
  cold, +77 ms warm, +4 ms cached at the median; its p90 is better cold and cached and worse warm.
  With 21 rounds per mode and ~1 s loss outliers in both runs (an open → first bytes of
  1.1–1.7 s in 4 of the 63 rounds before and 3 after), one run each cannot tell the pin from the
  network. In cached mode, where nothing but this segment runs, before and after are equal
  (233 vs 237 ms).
* **Why the pre-buffer cannot help here:** the player's first request reaches the proxy 9 ms
  (before) and 11 ms (after) after `open()` on the host (median over the 63 rounds), so a
  pre-buffer started at `open()` is at most that far ahead, and it needs the same TLS handshake
  and time to first byte as the request it replaces. It pays off where the backend takes long to ask (SailTune's audio backend; on the
  phone, the `proxy-open` → `proxy-request` gap, to be measured there), for a seek soon after the
  start (a small first chunk on a pooled warm connection instead of a new handshake) and for a
  stream opened ahead of use (Shorts `likelyNext`: the held jobs have their first 256 KiB in memory
  before the swipe). None of the three is in this harness.
* **Cost to watch on the phone:** every `open()` now starts two connections per stream (start and
  tail) and fetches up to 272 KiB per stream whether or not it is played: four TLS handshakes per
  start instead of two, on the resolver's worker thread. The tail is not read at start by the
  demuxers SailTube uses (INFERENCE from the container layouts: WebM cues and MP4 `sidx` / `moov`
  are at the front; not measured); its connection is reused by the first seek within 30 s.
  `SailTuneBackend::Config::proxy.tailBytes = 0` (or `prebufferBytes = 0`) switches each off
  without touching the pin (docs/SHARED_REQUESTS.md S-28 asks for a per-stream choice).
* Candidate 8 (small first ranges) is now upstream behaviour (512 KiB first chunk): as predicted,
  no effect on the first byte. Candidate 3 (one CDN handshake instead of three) is not what the
  re-pin does: the probe's connections are still closed after the headers (S-21 stays open in
  part).

### The redirect cases

Redirects belong to the CDN node, not to a video: 13 of the 42 cold + warm rounds of the first run
and 5 of 42 of the second had one or two probe redirects (first run: `aqz-KE-bpKQ` 5 rounds,
`R6MlUcmOul8` 4, and one each for `M7lc1UVf-VE`, `YE7VzlLtp-4`, `WhWc3b3KhnY`, `Y-rmzh0PI3c`;
second run: `R6MlUcmOul8` 2, one each for `YE7VzlLtp-4`, `Y-rmzh0PI3c`, `WhWc3b3KhnY`). MediaProxy
followed none in either run: it is handed the address the probe ended at. A round with a probe
redirect paid one more handshake per hop: `probeMs` median 329 ms against 241 without (first run),
448 against 280 (second run).

| Tap to picture (ms), 3 rounds: median (max) | cold before | cold after | warm before | warm after | cached before | cached after |
|---|---:|---:|---:|---:|---:|---:|
| `M7lc1UVf-VE` | 1284 (3788) | 1983 (3189) | 899 (913) | 1023 (1349) | 277 (317) | 296 (352) |
| `YE7VzlLtp-4` | 1485 (1657) | 2021 (2383) | 879 (1105) | 936 (1096) | 153 (320) | 250 (333) |

What makes these two ids slow is less the redirects than the **host rung probe**: they are two of
the four videos (with `aqz-KE-bpKQ` and `R6MlUcmOul8`) where the ladder plays a rung other than
the one the component probed, so every cold and warm tap pays a second probe (24 rung probes per
run: median 213 ms, p90 565, max 1936 before; 248 / 427 / 1193 after). The slowest round of each
run is `M7lc1UVf-VE` cold with a rung probe of 1.9 s (before: 3788 ms, with one probe hop and a
1.2 s component probe) and 1.2 s (after: 3189 ms, no redirect, a 1.3 s visitor mint). That is
candidate 4 / S-22 (probe the rung the host will play), which the re-pin does not touch. The
re-pin does not change how redirects are followed in the probe either; the proxy's pre-buffer
follows one too and keeps the address for the stream.

What would settle the pre-buffer question: an interleaved run (old and new pin alternating per
round, so both see the same network), or the phone's timing markers with `prebufferBytes` /
`tailBytes` on and off.

## How to run

```
tools/perf_playback.sh                       # 5 videos × 3 rounds × cold/warm/cached, container sfos-m1-sailtube
tools/perf_playback.sh --rounds 5 --videos aqz-KE-bpKQ,R6MlUcmOul8,WhWc3b3KhnY
tools/perf_playback.sh --height 360 --modes warm,cached
tools/perf_playback.sh --local ...           # build and run here (inside the container, or a Linux host with the dev packages)
tools/perf_playback.sh --build-only          # build only: no request is made
```

Options: `--primer <id>` (warm-up video, must not be in the list), `--round-timeout MS`,
`--pause MS`. `SAILTUBE_PERF_CONTAINER` and `DOCKER_HOST` select the container (OrbStack's socket
is used when present). The script copies only `Core/Player`, `Shared/SailTune/Core/SharedPlayback`,
`Shared/SailTune/Core/Beta` (one header the pinned `HttpsConnection.cpp` includes since `5611210`)
and the harness into `$SAILTUBE_PERF_DIR` in the container (default `/tmp/sailtube-perf`; the build goes to `<that>-build`; name another directory when several runs share the
container). A full default run takes about 4 minutes; the output ends with the summary tables and
`sailtube-perf: videos-with-numbers=K of N`. Feed the log to
`tools/experiments/perf_summary.py` for the NFR-PERF-02 pairs (`run` is unique per log).

## Pre-buffer on or off, interleaved (2026-11-11)

`tools/perf_playback_ab.sh`, one pass on one network: 5 videos, the arms alternating (`SAILTUBE_PREBUFFER=1|0`
switches the pinned proxy's 256 KiB start pre-buffer and 16 KiB tail cache). Cold: one fresh process per video and
arm, 2 rounds, the arm that runs first alternating from video to video. Warm: one process per round and arm (a
primer, then the 5 videos back to back), 2 rounds, the order alternating. 40 starts, all with a picture, no rate
limiting. Player at a4441a2. Summary by `tools/experiments/prebuffer_ab.py`.

| Tap to picture, ms | on median | on p90 | off median | off p90 | paired, off − on (median) | off faster |
|---|---:|---:|---:|---:|---:|---:|
| cold (n = 10 per arm) | 786 | 1803 | 959 | 1174 | −89 | 6 of 10 |
| warm (n = 10 per arm) | 624 | 755 | 281 | 810 | −136 | 7 of 10 |

- **Warm: slower with the pre-buffer on, in both rounds.** Per invocation the medians were 653 and 624 ms on,
  281 and 320 ms off; round 1 ran on first, round 2 off first. On: 417, 828, 258, 755, 653, 668, 406, 320, 624,
  649. Off: 281, 267, 658, 1933, 253, 320, 388, 264, 810, 244. About 300 ms, the size of the warm gap of the
  re-pin's before/after runs (681 → 920), which had been put down to the network.
- **Cold: no difference shown.** The pair differences run from −1373 to +580 ms (mean −64); the unpaired medians
  favour on, the paired ones off. The spread between two cold starts of one video is larger than any effect.
- The piece the pre-buffer can shorten, proxy open to first media byte, is 45 → 52 ms warm and 52 → 81 ms cold
  (on → off): a few tens of milliseconds at best, on a host where the player asks 10 ms after the open.
- The slowdown is not in that piece. The videos of a warm round follow each other at once, so the previous video's
  pre-buffer and tail requests are likely still running when the next start begins; this was not measured.

Limits: ten starts per cell, one network, a host with a fast link and no hardware decode wait. **Decision:**
default off (`prebufferByDefault` in `Clients/Sailfish/Composition/main.cpp`): no gain shown anywhere, a warm
slowdown measured twice, two more TLS handshakes per start and up to 272 KiB per opened stream. The phone can
still overturn it: step B5 compares `SAILTUBE_PREBUFFER=1` with `0` (`tools/experiments/prebuffer_ab.md`).
Raw logs: shared notes, `playback-beta3/prebuffer-ab-host/`.


## Memory: arena cap, idle trim, teardown trim (off)

Every opened video builds a GStreamer pipeline (~10 threads) and glibc keeps the freed memory in
per-thread arenas. `mallopt(M_ARENA_MAX, 2)` at the top of `main()` fixed that on the phone:
anonymous memory 104 MB after 12 opens vs 231-238 MB without it. The `malloc_trim(0)` after pipeline
teardown is switched off (`kTrimAfterTeardownEnabled = false` in
`Core/Player/Domain/MemoryTrimPolicy.h`): opens that came right after such a trim had a median first
frame of 7.7 s vs 1.8 s for the others. The data is confounded, but a hitch from the trim on the owner
thread is possible, and only 10 trim lines appeared for 24 teardowns (a teardown inside the 1.5 s
settle window restarts the timer and folds into one trim; a teardown whose next video is already
loading is skipped). The arena cap and the 15 s idle trim stay on. With `SAILTUBE_TIMING` every trim
logs `malloc-trim before=<kB> after=<kB> ms=<n> why=teardown|idle` and every skipped one
`malloc-trim skipped why=disabled|settle-restarted|busy`. To turn the teardown trim back on once its
cost is known from the `ms=` values, set `kTrimAfterTeardownEnabled = true` (one constant; the code and
tests are kept, `tests/memtrim` runs both settings).

## Shorts swipes on the host (2026-11-11)

`tools/perf_playback.sh --modes shorts` replays what a Shorts pager asks of the player, with the real resolver,
proxy and player in one process and a Short already playing when the swipe happens:
- **ahead**: the next Short was prepared while the current one played (resolve, the host's rung probe, proxy opens,
  no player; 2.5 s later the swipe): what `likelyNext` / `shortsAhead` gives for the first Short in line;
- **swipecold**: a Short nobody prepared (resolve over the network on the way);
- **back**: to the Short just left (its answer is in the resolver's cache, its streams were closed): what the
  second Short in line costs once its resolve alone was prepared, and a swipe back.
Tap = the swipe; picture = first frame at the player. Five videos, one round per invocation, two runs: the first with four invocations (pre-buffer off, on, on, off), the
second with eight (pre-buffer off/on x first rung 720/360, interleaved and reversed); 120 + 240 swipes, no rate limiting. `SAILTUBE_PREBUFFER=0|1` is the
proxy's start pre-buffer and tail cache (the app default is off).

| Tap to picture, ms (n = 20) | off median | off p90 | on median | on p90 |
|---|---:|---:|---:|---:|
| ahead (prepared) | 50 | 237 | 9 | 23 |
| back (resolve cached, no streams) | 76 | 257 | 135 | 232 |
| swipecold (nothing prepared) | 463 | 896 | 336 | 605 |

Two runs of the same plan pooled (the first run alone, n = 10, had `back` 146 off / 71 on; the second 71 / 160: the
`back` tier has no consistent pre-buffer effect, it moves between 55 and 211 ms from cell to cell).
- A swipe to a prepared Short is a first frame within about 50 ms (p90 240) even with the pre-buffer off; with it
  on the first bytes are already in memory and it is about 10 ms. That effect is the same in both runs. A swipe to a
  Short whose resolve alone was prepared (or back to the one just left) costs 75-135 ms, one to an unprepared Short
  about 460 ms (340 on), most of it the resolve (320 ms) and the first byte (50-100 ms).
- The pre-buffer shows its value where the swipe comes after a gap (the prepared Short, and the cold one a little), not in
  back-to-back starts (the A/B above, where it slowed warm starts). The pinned proxy sets it per proxy, not per
  stream (S-28), so the app cannot have it for the prepared Short only without a second proxy or a change in the
  component. On a phone network the first byte takes longer than here, so the pre-buffered start should gain more
  there; the phone settles it (B5).
- **First rung 360p instead of 720p** (the research's cheapest suggestion; ceiling 360 vs 720, 8 invocations, n = 10 per
  cell for 360, rungs picked 356-360p against 534-720p): `ahead` 99 / 14 ms (off / on) against 50 / 9; `back` 55 / 211
  against 76 / 135; `swipecold` 279 / 323 against 463 / 336. No reliable win: the cold swipe is faster by 180 ms
  without the pre-buffer and 13 ms with it, within the spread of the cells (p90 600-900 ms), and the others move the
  other way by noise. The decode part (first byte to first frame) is 2-6 ms for both rungs: on this host's link the
  size of the first fragment costs nothing, which is the one thing a lower rung saves. On a phone link of 4 Mbit/s
  a 720p first keyframe of 100-200 KB would take 200-400 ms against 60-120 ms for 360p: an estimate from typical
  VP9 keyframe sizes, not a measurement; device step K7 measures it.
- Limits: five videos, a fast link, no hardware decoder, one network; each cell is 10 or 20 swipes.

**Memory over a sequence of swipes** (`session_qt shorts_swipeSequenceStaysBounded`, the real session with the
arena cap applied first, one fixture Short per swipe, each prepared with `likelyNext` before it is opened): resident
size 125 -> 136 MB over 50 swipes in one run and 206 -> 207 MB over 200 in another (the allocator keeps freed
memory; the plateau differs from run to run), 95 MB of the 207 returned by `malloc_trim(0)` after the last stop,
threads 14-17 the whole time, one player and only the playing Short's two stream jobs at the end. The test fails
above 1 MB growth per swipe after a 10-swipe warm-up, above 20 MB of live memory over the warm-up level, or when
the thread count grows by more than 4.

