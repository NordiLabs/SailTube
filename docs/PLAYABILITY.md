<!-- SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0 -->
# Playability matrix: the app's whole resolve chain against live YouTube

Status: measured on the host on 2026-11-10, tree at 57ee3c8 plus the working changes of that day
(no phone). Harness: `tools/playability.sh` (source `tools/playability/Playability.cpp`, list
`tools/playability/videos.tsv`). Two runs: one pass over all 27 videos, then a targeted pass over six
of them (the replacement live stream, and five yt-dlp verdicts that were taken under an HTTP 429
window in the first pass).
Everything down to "How to run" is the state before the fixes; the fixes are in "After the fixes" at the end, and
what is still open is its "What remains".

## Summary

* **19 of 27 play** on the app's chain: 16 on the fast path, 1 live stream on yt-dlp without
  JavaScript, 1 age-gated (embeddable) video on yt-dlp with QuickJS. The ordinary, music-label,
  Shorts, finished-live, 4K/HDR, long, multi-audio and embedding-disabled categories all play from
  the fast path with a first sample 0.45–2.0 s after the tap.
* **2 fail but should play** (fails-should-play): an old 2007 upload (`_OBlgSz8sSM`) and a made-for-kids
  video (`XqZsoesa55w`). Both are refused by the `visionos` client. The fast path and the token-free
  yt-dlp pass both speak only that client, and its "Video unavailable" is classified **final**, so the
  QuickJS pass and the controller's one re-resolve never run (cause 1). `android_vr` and `web_embedded`
  play the old upload. The kids video also fails on every JavaScript client run, because the bundled
  runtime allows a run only 4 temporary files (cause 3).
* **8 fail as expected**: age-restricted and not embeddable (2), upcoming, region-limited,
  members-only, private, removed (2). Three of them show the wrong message (cause 5). The upcoming
  placeholder says "This video is not available." instead of "has not started yet" with its start
  time (RES-19, §6 P6). The region-limited video does not say "not available in your country". The
  removed video does not say "removed".
* Rate limiting: from the 12th video of the first pass on, every yt-dlp watch-page fetch got HTTP 429
  (the fast path's `/player` POST was not affected). `classifyFailure` has no rule for it, so during
  that window a private video was classified "video unavailable" (final, wrong message) instead of
  "private" (cause 4). It cleared within 15 minutes.

## Results

TTFF = ms from the tap (`PlaybackController::open`) to the first picture. "(parsed)" = first parsed
sample (`souphttpsrc ! parsebin ! fakesink` on the proxied loopback URL), because the container has
no H.264 / AAC decoder. The phone decodes these in software (avdec_h264).
None of the 19 picks was decodable here (VP9 + Opus), so every TTFF is a parsed one. "Fast" = the
component's outcome (failure / reason / fb = fallback allowed). "Passes" = each yt-dlp run (pass, worker
route, what `YtDlpVideoService::details` reports). "Message" = `PlaybackSession::errorText()`. The
settings key (`errorSettingKey`) was empty for every failure, which is right: no setting fixes them.

| Category | Id | Rung | Format (SEL-02 pick) | TTFF ms | Failure (fast / yt-dlp passes / first error) | App message | Verdict |
|---|---|---|---|---:|---|---|---|
| ordinary | `R6MlUcmOul8` | fast | dual 136 H.264 534p24 707k + 251 Opus 128k | 735 (parsed) | - | - | plays |
| ordinary | `iqKdEhx-dD4` | fast | dual 136 H.264 720p24 + 251 Opus | 1810 (parsed) | - | - | plays |
| music-label | `kJQP7kiw5Fk` | fast | dual 136 H.264 720p24 + 251 Opus | 888 (parsed) | - | - | plays |
| music-label | `dQw4w9WgXcQ` | fast | dual 136 H.264 720p25 + 251 Opus | 879 (parsed) | - | - | plays |
| music-label | `IB3lcPjvWLA` | fast | dual 136 H.264 720p24 + 251 Opus | 543 (parsed) | - | - | plays |
| shorts | `18NGQq7p3LY` | fast | dual 135 H.264 658p30 (vertical) + 251 Opus | 1068 (parsed) | - | - | plays |
| live-now | `rFZHOHl-L8A` | yt-dlp (no JS) | HLS master manifest (unproxied) | 3470 (parsed) | unplayable / live / fb; nojs: none (8 HLS variants) | - | plays |
| live-dvr | `lqQg6PlCWgI` | fast | dual 136 H.264 720p25 + 251 Opus en-US | 1228 (parsed) | - | - | plays |
| live-dvr | `qVv6vCqciTM` | fast | dual 134 H.264 360p30 + 251 Opus (recording has no taller H.264) | 1978 (parsed) | - | - | plays |
| upcoming | `O9V_EFbgpKQ` | - | - | - | final / final (no fallback) | This video is not available. **(should be "has not started yet" + start)** | fails-expected |
| age-restricted | `HtVdAasjOgU` | yt-dlp + QuickJS | dual 136 H.264 720p30 + 251 Opus en | 14179 (parsed) | unplayable / login_required / fb; nojs: age_restricted; js: none | - | plays (listed as expected to fail) |
| age-restricted | `Tq92D6wQ1mg` | - | - | - | unplayable / login_required / fb; nojs: age_restricted; js: age_restricted | This video is age-restricted and needs a signed-in account. | fails-expected |
| age-restricted | `SZJvDhaSDnc` | - | - | - | as above | as above | fails-expected |
| 4k-hdr | `aqz-KE-bpKQ` | fast | dual 135 H.264 480p30 + 251 Opus (720p+ rungs are 60 fps) | 476 (parsed) | - | - | plays |
| 4k-hdr | `LXb3EKWsInQ` | fast | dual 135 H.264 480p30 + 251 Opus (720p+ rungs are 60 fps) | 1118 (parsed) | - | - | plays |
| old-h264 | `dMH0bHeiRNg` | fast | dual 134 H.264 288p15 + 251 Opus | 446 (parsed) | - | - | plays |
| old-h264 | `_OBlgSz8sSM` | - | - | - | unplayable / unplayable / fb; nojs: video_unavailable (final) | This video is not available. | **fails-should-play** |
| old-h264 | `YE7VzlLtp-4` | fast | dual 136 H.264 720p24 + 251 Opus | 581 (parsed) | - | - | plays |
| long | `r4wLXNydzeY` | fast | dual 136 H.264 720p30 + 251 Opus en-US (3 h 27 min) | 1312 (parsed) | - | - | plays |
| multi-audio | `kX3nB4PpJko` | fast | dual 136 H.264 720p30 + 251 Opus en (original track) | 1005 (parsed) | - | - | plays |
| kids | `XqZsoesa55w` | - | - | - | unplayable / unplayable / fb; nojs: video_unavailable (final) | This video is not available. | **fails-should-play** |
| embed-disabled | `MeJVWBSsPAY` | fast | dual 134 H.264 360p30 + 251 Opus | 743 (parsed) | - | - | plays |
| region-limited | `sJL6WA-aGkQ` | - | - | - | final / final (no fallback) | This video is not available. (should be "not available in your country") | fails-expected |
| members-only | `CNbsMah32Ss` | - | - | - | unplayable / unplayable / fb; nojs: video_unavailable | This video is not available. | fails-expected |
| private | `yZIXLfi8CZQ` | - | - | - | unplayable / login_required / fb; nojs: private_video | This video is private. (run 1, under 429: "not available") | fails-expected |
| removed | `6SJNVb0GnPI` | - | - | - | final / final (no fallback) | This video is not available. (should be "removed") | fails-expected |
| removed | `Q39EVAstoRM` | - | - | - | unplayable / not_ok / fb; nojs: video_unavailable | This video is not available. | fails-expected |

Rows from the second pass: `rFZHOHl-L8A`, `_OBlgSz8sSM`, `XqZsoesa55w`, `CNbsMah32Ss`,
`yZIXLfi8CZQ`, `Q39EVAstoRM`. Their first-pass verdicts were the same, except the private video's
message. The first pass also tried the old 24/7 id `jfKfPfyJRdk`. That stream has ended (`post_live`):
the token-free pass said "video unavailable" and `android_vr`, `web_safari` and `tv` agreed. It is no
longer a live example, so the list now has the channel's current stream instead.

No rows were refused by the component's probe, and the breaker never opened. Eight answers needed the
adapter's own rung probe (`probeAddress`, all `accepted/206`), because SEL-02's pick was not the
component's top rung. No answer went through the controller's fresh re-resolve (cause 1 explains why).

## Per category

| Category | Ids | Verdict |
|---|---|---|
| ordinary | 2 | plays |
| music label (VEVO-style) | 3 | plays |
| Shorts | 1 | plays |
| live now (24/7) | 1 | plays (yt-dlp token-free pass, HLS; fast path refuses live by design) |
| finished live with DVR | 2 | plays (fast path) |
| premiere / upcoming | 1 | fails-expected, **wrong message** (cause 5) |
| age-restricted | 3 | 1 plays (embeddable, QuickJS pass); 2 fails-expected (need cookies, RES-08 rung 3) |
| 4K / HDR | 2 | plays (at 480p30: Auto never picks the 60 fps rungs, observation 7) |
| H.264-only old upload | 3 | 2 plays; **1 fails-should-play** (`_OBlgSz8sSM`, causes 1–2) |
| long (2 h+) | 1 | plays |
| multi-audio (dubbed) | 1 | plays (original track chosen) |
| kids (made for kids) | 1 | **fails-should-play** (causes 1–3) |
| embedding disabled | 1 | plays |
| region-limited | 1 | fails-expected, **wrong message** (cause 5) |
| members-only | 1 | fails-expected (generic "not available") |
| private | 1 | fails-expected (right message when not rate-limited) |
| removed | 2 | fails-expected (generic "not available", cause 5) |

## Causes, ranked

Line numbers are those of the working tree on 2026-11-10. `Clients/` and
`Core/Player` change at the same time, so the lines may move.

1. **A token-free "Video unavailable" is final, so the QuickJS pass and the re-resolve never run**
   (both fails-should-play). The token-free pass speaks only `visionos` and gets "Video unavailable" for
   the old upload and the kids video. `Core/Resolver/Infrastructure/YtDlpJson.cpp:247`
   (`classifyFailure`: "video unavailable" → `VideoUnavailable`) →
   `Core/Player/Infrastructure/YtDlpStreamResolver.cpp:63` (→ `StreamErrorClass::Unavailable`) →
   `Shared/Playback/StreamSource.h:140` (`isFinal`: Unavailable is final) →
   `YtDlpStreamResolver.cpp:167` (no widened pass after a final error) →
   `Clients/Sailfish/Presentation/PlaybackSession.cpp:221` (`resolveErrorKind`: Final, so
   `PlaybackController.cpp:584` makes no fresh re-resolve). The shared component already avoids this
   trap. `Shared/SailTune/Core/SharedPlayback/Domain/Playability.h:31-35` says a bare "video unavailable"
   from one client is never final, and the component does fall back. The yt-dlp adapter then turns the
   same answer final. Fix direction: after the token-free pass, treat `VideoUnavailable` as non-final
   (widen once). Treat it as final only on the widened pass, or when the text matches the component's
   final phrases ("has been removed", "terminated", "not made this video available in your country",
   "private video").
2. **The token-free pass names no `player_client`, so it repeats the fast path's client.**
   `Core/Resolver/Infrastructure/YtDlpVideoService.cpp:61` passes only `youtube:skip=dash`. Without a JS
   runtime, yt-dlp 2026.10.26 then uses `_DEFAULT_JSLESS_CLIENTS = ('visionos',)`. The fast path
   already speaks `visionos` (`Shared/SailTune/Core/SharedPlayback/Infrastructure/FastStreamResolver.h:44`).
   So rung 2 adds nothing whenever rung 1 got an UNPLAYABLE answer: in all 8 non-live fallbacks, the
   token-free pass ended the same way as the fast path (unavailable, age gate or private). Only live,
   which the component refuses on purpose, got something new from it. `android_vr` (JS-free, no player PO token) played
   `_OBlgSz8sSM`, as did `web_embedded` (see the client findings).
3. **The runtime allows a run only 4 temporary files, which blocks the JS clients for made-for-kids
   videos.** `Shared/Runtime/Python/resolver.py:67` (`self.attempts < 4`; the warm worker resets the
   count per call, `Core/Worker/Runtime/worker.py:177`). For the kids video, yt-dlp adds
   `web_embedded` (its own made-for-kids rule) and runs the QuickJS solver. That needs more script files,
   so 6 of 6 JS runs ended with "packaged resolver temporary storage limit exceeded" (exit 2). The app
   would then show `RuntimeUnavailable`'s "The packaged YouTube resolver is not available. Reinstall
   SailTube…", which is wrong advice. Even with cause 1 fixed, the kids video stays unplayable until this
   limit is raised (to the number of solver scripts a run can write, e.g. 16) or counted per solver
   call. This is SailTune's shared runtime (`Shared/Runtime`), so it is a request for the lead
   (docs/SHARED_REQUESTS.md).
4. **Rate limiting is classified as final.** `YtDlpJson.cpp:236-252` (`classifyFailure`) has no rule
   for "HTTP Error 429" / "Too Many Requests". Under the 429 window, yt-dlp could not read the watch page
   and the private video fell through to "video unavailable" (line 247): final, with the wrong message.
   RES-09 says rate limiting is never final. Fix: classify `http error 429` / `too many requests` as
   `Network`, or a non-final try-later class, before the "video unavailable" rule.
5. **Final fast-path answers lose their reason** (wrong messages for expected failures).
   `Core/Player/Infrastructure/SharedStreamSource.h:249` maps every component `Final` to `Unavailable`,
   because the component's `reason` is just "final" (docs/SHARED_REQUESTS.md S-14). So the upcoming
   placeholder shows "This video is not available." instead of the RES-19 upcoming text with its start
   time (§6 P6 fails as written). The JP-only video does not say "not available in your country", and
   the policy-removed video does not say "removed". Fix: S-14, where the component reports which final
   phrase matched (`upcoming`, `removed`, `terminated`, `geo`, `private`), and `errorFor` maps them. The
   upcoming case also needs the scheduled start (the fast path has it in `playabilityStatus`).
6. **Live goes to the extractor (by design) and costs 3.5 s.** The component refuses live
   (`Shared/SailTune/Core/SharedPlayback/Infrastructure/PlayerResponseParser.cpp:145`, `Unplayable`/"live").
   The token-free pass (`visionos`) gives 8 HLS variants and a `manifest_url`, so the master manifest is
   played unproxied (RES-19). This was checked only up to the first parsed sample. The A/V join of
   these variants is not proven (RES-19 says only `android_vr`'s muxed itags 91–96 join). On the phone,
   check §6 P6 with this stream, and compare `player_client=android_vr`.
7. Observation, not a failure: `Core/Media/Domain/StreamSelection.cpp:101` (`autoEligible` excludes
   > 30 fps) starts 60 fps uploads (`aqz-KE-bpKQ`, `LXb3EKWsInQ`) at their tallest ≤ 30 fps H.264 rung,
   480p30, under Auto. This is by design (SEL-02/03), but on these videos Auto means 480p.
8. Observation: the extractor path is slow. The age-gated video took 14.2 s to the first sample: the
   token-free pass took 3.8 s to say "age restricted", then QuickJS took 9.6 s. That is over the
   NFR-PERF-02 extractor budget (6 s). An age gate seen by the fast path (`login_required`) could skip
   straight to the widened pass.

## player_client findings

Each failing video was run again with `--extractor-args youtube:skip=dash;player_client=<c>`, at most
3 clients per video, with the full profile (QuickJS) and otherwise the service's own arguments. Each
answer went through `toStreamSet`, `playable`, `selectionInputFor` and `selectStreamsWithFallback`,
then a first parsed sample of the picked branch.

| Video (category) | Client | Result |
|---|---|---|
| `_OBlgSz8sSM` (old upload) | `android_vr` | **plays**: 720p VP9 + Opus dual, first sample 222 ms (run 2); "video unavailable" in run 1, under 429 |
| | `web_embedded` | **plays**: 720p VP9 + Opus dual, 299 ms (run 2), 457 ms (run 1) |
| | `tv` | "video unavailable" (run 2); only muxed 360p H.264 in run 1 (826 ms) |
| `XqZsoesa55w` (kids) | `web_embedded`, `tv`, `android_vr` | runtime temp-file limit in every run (cause 3); undecided until it is raised |
| `Tq92D6wQ1mg`, `SZJvDhaSDnc` (age-gated) | `web_embedded`, `tv`, `tv_simply` | age_restricted: need the cookie rung (RES-08 step 3) |
| `HtVdAasjOgU` (age-gated, embeddable) | default widened pass | plays: yt-dlp adds `web_embedded` itself for age-gated videos when a JS runtime is present |
| `jfKfPfyJRdk` (ended live) | `android_vr`, `web_safari`, `tv` | video unavailable: the stream is gone, as expected |
| `rFZHOHl-L8A` (live) | (none needed) | the token-free default (`visionos`) gives HLS |

What this decides for the explicit client choice (INFERENCE, to verify before code changes):

* **Token-free pass: `player_client=android_vr,visionos`** instead of the implicit `visionos`. It
  differs from the fast path's client, `android_vr` needs no JS and no player PO token, it played the
  only fails-should-play video that the runtime limit does not block, and RES-19 already names it for
  live. One caveat: `android_vr` was tested with QuickJS available, and `web_embedded` cannot be the
  token-free client (it requires the JS player).
* **Widened pass: keep yt-dlp's default** (`tv_samsung, visionos, web`, plus `web_embedded` for
  age-gated and made-for-kids videos, which yt-dlp adds itself). Adding `web_embedded` explicitly
  would cover the old-upload case even if the widening order changes. `tv` is not a reliable rescue.

## The list

`tools/playability/videos.tsv` (category, id, expectation, clients to try, why). Picks come from
yt-dlp's own extractor tests (Unlicense) and long-lived official uploads. Four came from flat listings
run once on 2026-11-10: the members-only video (from the channel's `UUMO…` playlist), the long video,
the upcoming placeholder and the current live stream.

| Category | Id | Expect | Why it is in the category |
|---|---|---|---|
| ordinary | `R6MlUcmOul8` | plays | Tears of Steel (Blender, 2012): long-lived official upload, VP9 + H.264 ladder |
| ordinary | `iqKdEhx-dD4` | plays | Vsauce "Mind Field" Ep 1 (2017): big creator channel; a yt-dlp test video |
| music-label | `kJQP7kiw5Fk` | plays | "Despacito" on LuisFonsiVEVO: label-managed VEVO upload |
| music-label | `dQw4w9WgXcQ` | plays | Rick Astley "Never Gonna Give You Up": official label video |
| music-label | `IB3lcPjvWLA` | plays | Afrojack "The Spark" official video (yt-dlp test) |
| shorts | `18NGQq7p3LY` | plays | a `/shorts/` upload from yt-dlp's tests (vertical) |
| live-now | `rFZHOHl-L8A` | plays | Lofi Girl "lofi hip hop radio", the current 24/7 stream (the channel restarts it now and then; re-check with its `/streams` tab) |
| live-dvr | `lqQg6PlCWgI` | plays | London 2012 hockey broadcast: finished live (`was_live`), 1 h 41 min |
| live-dvr | `qVv6vCqciTM` | plays | 2022 Christmas stream via `/live/`: post-live recording (yt-dlp test) |
| upcoming | `O9V_EFbgpKQ` | fails | a "free chat room" placeholder, scheduled and never starting (`is_upcoming`) |
| age-restricted | `HtVdAasjOgU` | fails | The Witcher 3 trailer: age-gated, embedding allowed (yt-dlp: works with `web_embedded`) |
| age-restricted | `Tq92D6wQ1mg` | fails | MMD "Adios": age-gated, embeddable only with clientScreen=EMBED |
| age-restricted | `SZJvDhaSDnc` | fails | 2014 news report: age-gated, needs authentication |
| 4k-hdr | `aqz-KE-bpKQ` | plays | Big Buck Bunny 4K 60 fps |
| 4k-hdr | `LXb3EKWsInQ` | plays | "Costa Rica in 4K 60fps HDR": HDR rungs |
| old-h264 | `dMH0bHeiRNg` | plays | "Evolution of Dance" (2006) |
| old-h264 | `_OBlgSz8sSM` | plays | "Charlie bit my finger" (2007) |
| old-h264 | `YE7VzlLtp-4` | plays | Big Buck Bunny, 2008 upload (yt-dlp's first test) |
| long | `r4wLXNydzeY` | plays | podcast episode, 3 h 27 min |
| multi-audio | `kX3nB4PpJko` | plays | MrBeast video with dubbed audio tracks |
| kids | `XqZsoesa55w` | plays | Pinkfong "Baby Shark Dance": made for kids |
| embed-disabled | `MeJVWBSsPAY` | plays | not embeddable, not age-gated (yt-dlp test) |
| region-limited | `sJL6WA-aGkQ` | fails | geo-restricted to Japan (yt-dlp test); the host is outside Japan |
| members-only | `CNbsMah32Ss` | fails | members-only video (`subscriber_only` in the channel's UUMO playlist) |
| private | `yZIXLfi8CZQ` | fails | now a private video (yt-dlp test) |
| removed | `6SJNVb0GnPI` | fails | removed for a policy violation (yt-dlp test) |
| removed | `Q39EVAstoRM` | fails | "This video does not exist" (yt-dlp test) |

Not covered: a premiere (a scheduled first showing of an upload, which differs from a scheduled
stream). No stable example was at hand.

## Method

One child process per video, so no visitor id, breaker, connection or cache is carried over. The
yt-dlp player-JS cache directory is shared, as it survives app starts. In the child, everything is the
app's code except two recording seams and the port:

* **Start-up as in `main.cpp`**: the warm worker's latency lane (`WarmResolver`) is started and the
  fast path's visitor id minted before the tap (the app warms both once the first page is up).
* **Controller**: the real `PlaybackController` (open → `resolveRequestFor` → port), with phone
  capabilities (software H.264 + VP9, no verified hardware decode, no AV1) and default preferences
  (Auto: 720p ceiling for software decode, unmetered).
* **Port**: what `PlaybackSession::Port::resolve` and `onStreams` do (fresh → `invalidate`;
  `streamRequestFor(r, unmetered, poProvider=false)`; `resolveErrorKind`, `selectionInputFor`; the answer
  goes to `onResolved`). It links the real free functions from `PlaybackSession.cpp`.
* **Resolver**: `SharedStreamSource` over SailTune's pinned `FastStreamResolver` and `MediaProxy`, with
  `YtDlpStreamResolver` → `YtDlpVideoService` → `YtDlpRunner` as the fallback. The runner uses the
  bundled runtime: the binary in resolver-child, QuickJS and worker modes; `yt-dlp.pyz` 2026.10.26;
  QuickJS 0.18.0 from `/builds/_vendor/python-runtime`; python3.11 and PyOtherSide from the container.
  The adapter runs on the GUI thread, not through `ThreadedStreamSource`: the same decisions, without
  the thread hop.
* **Seams** (record only): the Api traits' resolver forwards every call to the real
  `FastStreamResolver` and records its outcome and the rung probes. A `WarmBackend` forwards every
  yt-dlp run to the real `WarmResolver` when its lane is idle, else to a second real `YtDlpRunner`
  (one-shot child), as the runner would. It records exit, stderr class (`classifyFailure`), the
  service's verdict and the first ERROR/WARNING line (URLs cut, 140 characters). Every run went to the
  warm worker.
* **Picture**: the controller's first load is opened like `PlaybackSession::openBranches` (fetcher →
  MediaProxy loopback URLs, HLS direct). It is timed with the real `VideoPlayer` when VP9 + Opus (the
  container decodes those), else to the first parsed sample of the picture branch.
* **Message**: `PlaybackSession::errorText()`'s switch over the controller's failure (real
  `streamErrorText` / `errorText`), and `errorSettingKey(controller.errorSettingFacts(...))`.

Environment: Apple M1 Max, container `sfos-m1-sailtube` (Sailfish SDK userland, aarch64, OrbStack), GStreamer
1.26, Qt 5.6.3, gcc 13 `-O2 -Wall -Wextra` (warning-free), the Mac's uplink through NAT, LC_ALL=C.UTF-8.
TTFF is host time to the first parsed sample. The phone adds its RTTs, its CPU and software decode
(see docs/PERF_PLAYBACK.md).

Request budget: two passes (27 videos, then 6), at least 4 s apart, plus two one-video smoke runs
before them. That made 39 yt-dlp runs in all, 21 of them player_client tries (15 + 6), and 7 flat listings to find
examples. All yt-dlp runs went to the warm worker. The 429 window started at the 12th video of the
first pass, after the listings and smoke runs of the previous half hour. Other work may use the same
uplink.

## How to run

```
tools/playability.sh                                  # the whole list, container sfos-m1-sailtube
tools/playability.sh --videos _OBlgSz8sSM,XqZsoesa55w # some ids (ids not in the list run as "adhoc", expect plays)
tools/playability.sh --no-clients                     # no player_client tries
tools/playability.sh --no-warm                        # one-shot resolver children instead of the warm worker
tools/playability.sh --local ...                      # build and run here (inside the container)
tools/playability.sh --build-only
```

`SAILTUBE_PLAYABILITY_CONTAINER`, `SAILTUBE_PLAYABILITY_DIR` (default `/tmp/sailtube-playability`, its
own directory) and `SAILTUBE_PYTHON_RUNTIME` select the container, its directory and the bundled
runtime. The script copies `Core`, `Shared`, `Clients/Sailfish/Presentation` and the tool, builds about
60 sources in parallel (~40 s), assembles the runtime layout (`runtime/`, `lib/`), and runs with HOME
and the caches inside its build directory. The output is `playability: event` lines while running, one
`playability: result k=v …` line per video, then the three tables above (Markdown). It never prints a
URL. Do not loop it: one pass per day, and a second one only for rows that changed.

## After the fixes

Status: measured on the host on 2026-11-10, tree at c887e47 plus the working changes of that day (the
fixes for causes 1–6). Same harness and container as above. Two targeted runs, each video at most
once per run:

* **Run 1** (12 videos, `--no-clients`): all 21 yt-dlp runs got HTTP 429 on the watch page. The
  window was the one the first matrix had opened earlier that day. The rows it could still decide are
  the three fast-path `Final` answers (the player request itself was not limited) and the two
  ordinary videos.
* **Run 2** (9 videos, after the window had cleared): first one ordinary video and the upcoming
  placeholder (exactly one yt-dlp run, no 429), then seven more. No run in it saw a 429.

### Per video, before and after

"Before" is the matrix above. "After" is run 2 unless the row says run 1. Times are ms from the tap
to the first sample (plays) or to the message (fails). The host was shared with other builds, so the
fast-path times vary more than in the matrix.

| Category | Id | Before | After | Run |
|---|---|---|---|---|
| ordinary | `R6MlUcmOul8` | plays, fast, 735 | plays, fast, same pick, 1758 (658 in run 1) | 2 |
| music-label | `dQw4w9WgXcQ` | plays, fast, 879 | plays, fast, same pick, 570 (526 in run 1) | 2 |
| old-h264 | `_OBlgSz8sSM` | **fails**: "This video is not available." | **plays**: yt-dlp + QuickJS, dual 247-sr VP9 720p30 + 251 Opus, first decoded picture 11 936. Token-free pass 1.7 s with 0 formats, then the QuickJS pass 9.6 s with 31 formats. | 2 |
| kids | `XqZsoesa55w` | **fails**: "This video is not available." | **fails**: "The video could not be extracted. Try again; if it keeps failing, SailTube's resolver may need an update." after 26 952. Token-free pass: 0 formats. QuickJS pass: exit 2, "temporary storage limit exceeded" (cause 3). Both ran twice, because the session still re-resolved. | 2 |
| live-now | `rFZHOHl-L8A` | plays, yt-dlp without JS, HLS master manifest, 3470 | plays, yt-dlp without JS, muxed HLS itag 95 (H.264 720p30 + AAC), 5685. The run took 2.6 s and gave 14 HLS formats: the 8 of `visionos` plus 6 muxed ones. | 2 |
| upcoming | `O9V_EFbgpKQ` | "This video is not available." | "This live event has not started yet. Scheduled to start <date and time>." after 3740 (fast `Final` at 384, then one yt-dlp run of 3.4 s) | 2 |
| region-limited | `sJL6WA-aGkQ` | "This video is not available." | "This video is not available in your country." after 2281 (fast `Final` at 107, one yt-dlp run) | 1 |
| removed | `6SJNVb0GnPI` | "This video is not available." | "This video has been removed." after 1626 (fast `Final` at 112, one yt-dlp run) | 1 |
| members-only | `CNbsMah32Ss` | "This video is not available." | "This video needs a signed-in account." after 2300, final, one yt-dlp run | 2 |
| private | `yZIXLfi8CZQ` | "This video is private." (run 1 of the matrix, under 429: "not available") | "This video is private." after 1810, one yt-dlp run. Under 429 (run 1): "YouTube is limiting requests right now. Try again in a minute." | 2 |
| removed (does not exist) | `Q39EVAstoRM` | "This video is not available." | **wrong in run 2**: "YouTube changed how this video is delivered. …" after 5650 and four yt-dlp runs. Fixed after the run, see below; not run again. | 2 |
| age-restricted, embeddable | `HtVdAasjOgU` | plays, QuickJS pass, 14 179 | plays, QuickJS pass, same pick, 31 573: under 429 the first resolve failed and the session's re-resolve played it | 1 |

Under the 429 window (run 1), the videos that need yt-dlp showed "YouTube is limiting requests right
now. Try again in a minute." instead of "This video is not available." (cause 4). The live stream
showed the bot-check message there: all four of its yt-dlp runs carried HTTP 429. In run 2 it played
with the same client list, so that message came from the window, not from the clients.

### What decided each row

* **Cause 1** (a bare "Video unavailable" is not final). `_OBlgSz8sSM` plays because the QuickJS pass
  now runs after the token-free pass found nothing. Clearly final answers stay final and cost one
  yt-dlp run: removed, country, private, upcoming, members-only.
* **Cause 3** (temp-file budget). The kids video is still blocked by SailTune's runtime
  (`Shared/Runtime/Python/resolver.py:67`, at most 4 temporary files per run). The limit cannot be
  raised from SailTube's side: no environment variable or argument reaches it. The failure is now a
  retryable extraction failure with its own text, not "Reinstall SailTube". After run 2 the session
  stopped re-resolving this failure by itself, because the second resolve meets the same budget. That
  should halve the 27 s to about 14 s. It is host-tested, not measured live. Request S-26.
* **Cause 4** (HTTP 429). Classified as try-later, never final, and the QuickJS pass is not started
  into the same limit. The session waits 10 s and resolves once more. The harness retries at once, so
  the 10 s pause is only host-tested.
* **Cause 5** (final fast-path answers keep their reason). The pinned component says only "final", so
  the extractor is asked once to name the case (token-free pass only, never the QuickJS pass, never
  retried). Measured cost: the fast path's `Final` arrives after 0.1–0.4 s, the named message after
  1.6–3.7 s (one yt-dlp run of 1.5–3.4 s). RES-09's "a removed video in a queue is skipped within
  1.5 s" is not met for these until the component names the case itself (S-14).
* **`Q39EVAstoRM`** (found by run 2). Both passes ended with exit 0, no formats and a reason that no
  rule knew, and "no format with a URL" was read as a changed delivery. The token-free clients very
  likely word it "This video is unavailable" (the app clients' text). That is an inference: the harness
  does not print the line. Two changes followed. The phrase is now known. An answer without formats is
  a changed delivery only when the run shows it (SABR-only formats, a failed n or signature challenge,
  formats skipped for a missing PO token, formats listed without a usable URL). Otherwise it is the
  plain, non-final "This video is not available.". Host tests cover both. The video was not run again
  (two runs per video).

### The player_client decision

The token-free pass keeps `player_client=android_vr,visionos`. The QuickJS pass keeps yt-dlp's
default clients. The reason is not the one the matrix inferred:

* **Recorded videos: `android_vr` adds nothing.** In run 2 the token-free pass returned 0 formats for
  the old upload, the kids video and the members-only video. yt-dlp 2026.10.26 requires a GVS PO token
  for `android_vr`'s https formats and skips them without one (`_video.py`, "require a GVS PO Token
  which was not provided. They will be skipped"). The matrix's `android_vr` trial that played
  `_OBlgSz8sSM` ran with the full profile and was not reproduced. What rescues the old upload is
  cause 1 plus the QuickJS pass.
* **Live: `android_vr` is what RES-19 asks for.** Its HLS needs no PO token. In run 2 the token-free
  pass returned 14 HLS formats instead of 8, six of them muxed (itags 91–96), and the app picked itag
  95 and parsed its first sample. The matrix (only `visionos`) played the master manifest of split
  variants, which RES-19 calls not joinable on the phone.
* **Cost.** One more player request in every token-free pass. The pass took 0.9–3.4 s in run 2 with
  both clients (nine runs). A one-client figure under the same load was not measured.

To drop `android_vr` again, change one constant (`tokenFreeYoutubeArguments` in
`Core/Resolver/Infrastructure/YtDlpVideoService.cpp`) and the four test lines that assert it. Live
then goes back to the manifest of split variants. A better split would name `android_vr` only when
the fast path has said "live". That needs the hint to reach the adapter and is not done.

### What remains

* **Kids video**: unplayable until SailTune raises or re-scopes the temp-file budget (S-26).
  `android_vr` and `visionos` do not serve made-for-kids videos, so only the QuickJS pass can.
* **Age-restricted, not embeddable** (`Tq92D6wQ1mg`, `SZJvDhaSDnc`): need the cookie rung (RES-08
  step 3) and an account. Not run again.
* **Final answers cost one yt-dlp run** to get their reason and the upcoming start (S-14).
* **Slow extractor path**: the old upload needs 11.9 s to its first picture, 9.6 s of it in the
  QuickJS pass (observation 8 stands).
* **On the phone**: the muxed live variant (itag 95) and the 10 s try-later pause were not checked on
  a device. §6 P6 should use this stream.
* **Not measured live after the last changes**: `Q39EVAstoRM`'s message, the kids video's shorter
  failure, and a real 429 with the delayed retry.

Request budget of the two runs: run 1 made 21 yt-dlp runs (all with HTTP 429 on the watch page),
run 2 made 14 (none with 429), all through the warm worker, with 5–6 s between videos. No
player_client try ran: in run 2 every video that was expected to play and had tries listed did play.
