# SailTube requests for the shared instant-playback component

Status 2026-11-11: written 2026-11-08 for the playback component, last updated with the re-pin to 5611210. **Requests
S-26..S-31 are open and wait for the SailTune side** (SailTube cannot edit
the pinned copy); the other open S-ids are listed per id in the table for the pin 5611210. They are routed to SailTune by the maintainer. SailTube does not edit SailTune or the pinned copy under `Shared/SailTune/`.

The player-side interface is core's `Shared/Playback/StreamSource.h` (manifest mode `local`); the
playback component's earlier proposal `Shared/Playback/StreamResolver.h` is deleted. Two producers
answer it:

- `Core/Player/Infrastructure/SharedStreamSource*` adapts SailTune's pinned component
  (`Shared/SailTune/Core/SharedPlayback`, first fb5cca07, then fd49627, since 2026-11-10 SailTune `main` 5611210:
  FastStreamResolver + MediaProxy), runs it on a worker thread (`ThreadedStreamSource`) and asks the component to probe
  a rung it delivers in place of the component's top one;
- `Core/Player/Infrastructure/YtDlpStream*` + `Core/Player/Domain/ResolveCache` adapt core's yt-dlp
  path; it is the extractor fallback (`ResolveOutcome.fallback`, RES-08).

Requirement ids refer to SAILTUBE_REQUIREMENTS.md v1.0. "INSTANT" is INSTANT_PLAYBACK_SPEC.md.

## To SailTune (shared component)

| Id | Request | Serves |
|---|---|---|
| S-01 | **Video-only formats flag.** A request flag (`StreamRequest::wantVideo`) makes the format ladder also return ranked video-only formats (H.264/VP9, AV1 only when the caller allows it). The fast path must accept the answer only with a video + audio pair (INSTANT §2.3 item 5, §8). With the flag off, the answer is exactly today's audio answer. | SHARED_PLAYBACK pt. 4, RES-03, SEL-01, SEL-02, SEL-08 |
| S-02 | **Two concurrent streams.** The fetcher serves a video and an audio stream of one video at the same time (two jobs, one per (video, format)), reports this through `supportsConcurrentStreams()`, and gives both one shared mid-stream refresh. | SHARED_PLAYBACK pt. 4, RES-12, RES-16, RES-18 |
| S-03 | **Per-format fields** on every returned format: format id/itag, container, codec strings, width/height, fps, bitrate, `clen`, language and language preference, DRC, Premium, HDR, `isHls`, request headers and `expire`. Video needs `container` (seeking differs per demuxer), `clen` (probe range and `Content-Length`), Premium and fps (quality menu). | RES-06, RES-16, RES-17, SEL-02, SEL-03, SEL-04, SEL-05 |
| S-04 | **Validator for video and HLS.** The shared output validator (https, `*.googlevideo.com`, `/videoplayback`, exactly one `expire`, header allow-list) also accepts video formats. It also accepts the HLS manifest hosts that live video uses (the `manifest.googlevideo.com` master and variant playlists). Please expose it as a pure function, because SailTube's downloader must call it too. | §1.6, RES-17, RES-19, RES-22 |
| S-05 | **Cache key with the video fields.** The key holds quality ceiling, network class, dub language, hardware decode and codec policy, besides client, PO provider and signed-in state. Expose the field registry (as `outputAffectingFields()` does in the proposal) so that SailTube's RES-10 invariant test can enumerate it. | RES-10 |
| S-06 | **Live and upcoming for video.** With the video flag on and a live answer, make the second muxed-HLS call (ANDROID_VR, itags 91-96) and return the master manifest unproxied and unprobed. For an upcoming video, return its scheduled start (`scheduledStartUnixSeconds`). | RES-19, INSTANT §8 |
| S-07 | **Probe video and audio together.** Probe both chosen URLs in parallel, each with its own verdict per format (`ProbeState`), including split verdicts. | RES-06 |
| S-08 | **Error classification exported.** A classification that knows the signed-in state (private is final only when signed out). "Try again later" is never final. Post-live answers get a "still processing" class. Each error carries a cleaned short reason. | RES-09 |
| S-09 | **Diagnostics.** Counters for fast path, extractor and failed, with median timings, breaker state and its end time, and per-resolve phase timings (queue, request, probe). | RES-07, DIA-02 |
| S-10 | **Callback contract.** Callbacks run on the owning thread, never synchronously inside the call, at most once per operation. A cancelled operation never calls back. Every answer is tagged with its video id. | PLY-level answer tagging, RES-11 |
| S-11 | **appsrc feeder.** Besides the loopback URL, offer an in-process feeder (`StreamFeeder`: size, need-data, enough-data, seek-data), so that video can use RES-17 (a) without a listening socket. | RES-17 (a), RES-18 |
| S-12 | **Prefetch replaces the oldest.** With two speculative resolves running, a third replaces the oldest pending one instead of being dropped. | RES-11 |
| S-13 | **Loudness on video answers.** `loudnessDb` (or perceptual loudness minus target) also comes back when the video flag is on. | INSTANT §4, PLY normalisation |

### Requests added in batch 2 (S-14..S-25)

| Id | Request | Serves |
|---|---|---|
| S-14 | **Final subclass.** `ResolveOutcome` with `FailureKind::Final` carries only `reason = "final"`. Please name the case (removed, terminated, upcoming/premiere, geo, private, invalid id) and treat "private" as final only when the host says it is signed out. The adapter maps every Final to SailTube's final class `Unavailable` and never falls back. | RES-09 |
| S-15 | **Follow googlevideo redirects** in the probe and in MediaProxy (POCO: a 302 on the first ranged request sent the fast path to the extractor, 22 s). **Met at fd49627** (`MediaAddress.h`, up to 3 hops, https `*.googlevideo.com` only); SailTube keeps the component's probe on and adds no redirect handling. Still open: `isMediaCdnAddress` does not require the path `/videoplayback` that core's `acceptedMediaUrl` (RES-22) requires; please align or expose one shared validator (S-04). | RES-06, RES-16, RES-22 |
| S-16 | **Refresher context.** Pass the reason (refused at byte 0, expired, died) and the byte offset to the `Refresher`, so that a host can count a strike and skip the fast client after a byte-0 refusal of an unexpired fast-path URL. | RES-07, RES-12 |
| S-17 | **Video request fields.** A video codec preference with AV1 opt-in (SEL-01), the high-frame-rate choice, and rungs above the ceiling kept (ranked after) instead of removed. SailTube's SEL-02 puts VP9 first with verified hardware decode, the component puts H.264 first; the adapter re-ranks, but the component then probes its own top video, not SailTube's. | SEL-01, SEL-02, RES-06 |
| S-18 | **Per-format probe verdict** (`accepted` / `no verdict` / `not probed`) on the returned formats, so that the player escalates a first-request failure of an unverified URL at once. | RES-06 |
| S-19 | **Warm-up completion.** `warmUp(callback(ok, visitorMs))` or a signal; the adapter polls `hasVisitor()` every 20 ms for the `warmup-done` marker. Optionally persist the visitor id for its 6 h across launches. | RES-05, RES-21, DIA-03 |
| S-20 | **Live with video** (S-06 restated against the pin): the parser turns `isLive`/`isUpcoming` into `Unplayable` ("live"), so every live tap pays the extractor. | RES-19 |
| S-21 | **Playback reuses the probe's connection** (or the probe's ranged GET becomes the first chunk MediaProxy serves): the host harness measured 79–102 ms for the proxy's own TLS connect + first byte right after a probe on separate connections (docs/PERF_PLAYBACK.md); est. 200–300 ms on 4G. | RES-06, RES-16, NFR-PERF-02 |
| S-22 | **Probe the rung the host will play**: let the request carry the host's ordered video codec/height preference (or accept the host's ladder) so the component's probe covers it; today the adapter probes again through `probeAddress` in ~40 % of host taps (~60 ms). | RES-06, SEL-01 |
| S-23 | **Retry a 403 on a freshly issued address once in the probe** (~500 ms, as MediaProxy already does for playback) before failing the fast path: 2 of 55 cold host taps hit it and would have gone to the extractor and counted a breaker strike. | RES-06, RES-07 |
| S-24 | **Typed timing callback** (visitor, connect, /player, parse, probe with redirect count and connect vs first-byte split) instead of parsing `Options::log` text; and a stream tag in MediaProxy log lines. | DIA-02, NFR-PERF-02 |
| S-25 | **Real keep-alive.** `warmUp()` only mints a missing visitor id; it does not refresh the pooled player connection, so the 45 s idle drop costs a new TLS handshake on the next tap. A `keepAlive()` (cheap request or connection refresh) the host can call while lists are visible. | RES-21, NFR-PERF-02 |
| S-26 | **Temp-file budget of the shared runtime.** `Shared/Runtime/Python/resolver.py:67` allows a run 4 `tempfile.mkstemp` calls (`self.attempts < 4`; its comment calls the number "a policy budget, not a measured count"). yt-dlp's QuickJS challenge solver needs more when a run asks several JavaScript clients: for the made-for-kids video `XqZsoesa55w` every QuickJS run ended with exit 2 and "packaged resolver temporary storage limit exceeded" (6 of 6 runs in the matrix, 2 of 2 on 2026-11-10 after the fixes, docs/PLAYABILITY.md). `android_vr` and `visionos` do not serve made-for-kids videos, so no pass without JavaScript can play them, and the host cannot raise the limit (no environment variable or argument reaches it; the warm worker only resets the count per call, `Core/Worker/Runtime/worker.py`). Please raise the budget to what one solver run can write (16 proposed), or count it per solver call instead of per run. The other guards (owned 0700 directory, 32 MiB file size limit, files only in that directory) can stay. SailTube now reports the failure as a retryable extraction failure (`Failed`, label `storage_limit`), not as a missing runtime. | RES-08, RES-09 |
| S-14 (restated, measured) | **Name the Final case.** Still open at the pin: `PlayerResponseParser` sets `reason = "final"` for every phrase in `isFinalReason`. SailTube now asks the extractor once to name an unnamed Final (`StreamResolver::nameFinal`: the token-free pass only, never retried), because the message and the upcoming start need it. Measured on the host on 2026-11-10: the component's Final arrives after 0.1–0.4 s, the named answer after 1.6–3.7 s (one yt-dlp run of 1.5–3.4 s for removed, not-in-your-country and upcoming). RES-09's "a removed video in a queue is skipped within 1.5 s" is not met until the component names the case. Please set `reason` to one of `removed`, `terminated`, `upcoming`, `geo`, `private`, `invalid_id` (the adapter already maps exactly these labels and then makes no extractor run), and for `upcoming` carry the scheduled start in the outcome when the answer has it (yt-dlp reads it from the microformat's `liveBroadcastDetails.startTimestamp`). | RES-09, RES-19 |
| S-27 | **Storyboard spec in the answer's metadata.** The player answer the fast path already reads carries `storyboards.playerStoryboardSpecRenderer.spec` (one string: the base URL with `$L`/`$N` placeholders, then one `width#height#count#columns#rows#interval#name#sigh` group per level, separated by `\|`). Please carry it as it is in `StreamSet.metadata` (bounded, e.g. 4096 characters; it holds signed parameters, so it is never logged). The host builds the scrub preview from it (`PlaybackSession::setStoryboard`, which has no source today), without a second request or an extractor run. Empty when the answer has none (live). | watch overlay scrub preview, NFR-PERF-02 |

### State at the pin 5611210 (re-pin 2026-11-10)

The pin moved from `fd49627` to SailTune `main` `5611210` ("Beta 2 in progress: faster stream start and packaging fix").
What changed in the component between the two: `FastStreamResolver::peekCached` and `ResolveCache::peek` (a read of
the cache without network, breaker or counters); `MediaProxy` fetches the first 256 KiB of a stream at `open()`
(pre-buffer) and its last 16 KiB (tail cache) over connections of their own, asks upstream in growing chunks (512 KiB,
then x4 up to 10 MiB), keeps up to three idle upstream connections for 30 s and hands them to the next job on the same
host, sets `LowDelayOption` on the player's socket, and has `reuse()` (the still-open stream of the same bytes for a
second playback); `HttpsConnection` offers each answer's `Date` header to `Core/Beta/ServerDate.h` (a new pinned
header; an empty inline function without `SAILTUNE_BETA`). Nothing changed in the parser, the ladder, the probe, the
request, the cache key or the timings. Measured effect: docs/PERF_PLAYBACK.md, "Re-pin 2026-11-10".

| Id | At 5611210 | Note |
|---|---|---|
| S-01 | in part, unchanged | `audioOnly = false` ranks video-only formats; no codec choice (S-17) |
| S-02 | met | up to 8 jobs; video and audio of one video proxied together |
| S-03 | open | no Premium, HDR, language preference, muxed audio codec; only the user agent as header |
| S-04 | open | `MediaProxy::allowedAddress` / `MediaAddress.h` unchanged |
| S-05 | open | `StreamRequest::cacheKey` unchanged; the adapter keys nothing on the component and filters each answer |
| S-06, S-20 | open | live answers are still `Unplayable` ("live"): every live tap pays the extractor |
| S-07, S-18 | open | one join verdict, no per-format probe state |
| S-08, S-14 | open | every Final still carries `reason = "final"`; the adapter asks the extractor once to name it |
| S-09 | in part, unchanged | resolver counters, median, breaker; `MediaProxy::Counters` gained `prebufferHits`, `prebufferFailures`, `tailHits`, `tailFailures`, `reused`, `upstreamReused` (the adapter reports them as `proxy.*`) |
| S-10 | met | unchanged |
| S-11 | open | no in-process feeder |
| S-12 | open in the component, eased in the adapter | still no prefetch call; with `peekCached` the adapter's `prefetch()` starts nothing for a request that is already answered, so such a request no longer takes one of the two slots or replaces a running prefetch |
| S-13 | met | unchanged |
| S-15 | met (fd49627) | the path check of `isMediaCdnAddress` is still open |
| S-16 | open | `Refresher` still gets no reason or offset |
| S-17, S-22 | open | no video codec order in the request; the adapter still probes the rung it delivers |
| S-19 | open | no warm-up completion; the adapter polls `hasVisitor()` |
| S-21 | in part | not as asked (the probe's connection is still closed after the headers), but the handshake and the first 256 KiB of each branch now start at `open()`, before the player's first request, and that warm connection serves the next chunk. See the before/after table for what it is worth on the host |
| S-23 | open | no retry of a 403 on a fresh address in the probe |
| S-24 | open | no typed timing callback (`StreamTimings` unchanged; the harness still reads `Options::log`); MediaProxy's `first_byte` lines now end in `from prebuffer` / `from tail` when served from memory, still without a stream tag |
| S-25 | open | no keep-alive |
| S-26 | open | `Runtime/Python/resolver.py` is unchanged at 5611210 (the runtime group was not re-pinned) |
| S-27 | open | no storyboard spec in `StreamSet.metadata` |

Pending upstream (committed in SailTune after the pin, `8b19537` "Streaming: wait for the network instead of failing
or skipping", marked there "not measured on the phone yet"; not pinned): visitor and player timeouts of 4 s and 5 s
(8 s and 10 s at the pin), `networkSilent()` / `checkNetwork()`, and no back-off after a visitor mint that got no
answer at all. None of them answers a request above; the timeouts and the missing back-off are worth taking with the
next re-pin (a phone that lost its network keeps the fast path off for up to five minutes today).

### New requests (re-pin 2026-11-10)

| Id | Request | Serves |
|---|---|---|
| S-28 | **Pre-buffer and tail per stream.** `MediaProxy::open()` now always fetches the first 256 KiB and the last 16 KiB (per-proxy `Options`). A host that opens streams ahead of use (SailTube's Shorts pager opens the next Short's branches before the swipe; a quality switch opens a rung it may drop) pays up to 272 KiB per stream on a metered network for something that may never play, and a video demuxer in push mode never reads the tail. Please let `open()` take the two sizes per stream (or a "deferred" flag that starts them on the first request). | RES-11, RES-16, NFR-PERF-02 |
| S-29 | **`reuse()` by address.** `reuse(videoId, length, type, ...)` cannot tell a stream of the current address from one whose address was refused a moment ago (same length and type). SailTube keeps the other branch's stream open while one branch is re-resolved, so it does not call `reuse()`. A variant that also takes the address (or a `closeStream(url)`), would make it safe for hosts that do not close the whole video on every fault. | RES-12, RES-16 |
| S-30 | **`MediaProxyTest` "four redirects in a row" depends on timing.** `tests/sharedplayback/MediaProxyTest.cpp` (`testRedirectsWithinTheCdnAreFollowed`, the `loop` rig; line 453 at fd49627) scripts the media host by arrival order (`mediaActions = {0, 302, 302, 302, 302, 0}`) and expects `redirects == 3 && refreshCalls == 1`. The rig sets `upstreamTimeoutMs = 2000`, a budget of silence that includes a fresh TLS handshake after every 302 (the connection is dropped on a non-206 answer), and the test's client (`fetch()`: `waitForReadyRead(20)` then one `processEvents()`) paces the proxy and the fake host, which run in the same thread, at 50 passes a second at most: the case takes 1.85–2.0 s with 0.05 s of CPU. When one request is silent for 2 s the proxy does the right thing (`chunk_end status 0 class timeout`, resume at the same byte, body complete), but the script has already spent that request's `302`, so the proxy sees three redirects and never the fourth: `status 200`, full body, `redirects == 3`, `refreshCalls == 0`, and the check fails. Reproduced on a scratch copy by holding the second 302 for 2100 ms (fails exactly so; 1500 ms passes); it failed once on a SailTube gate with three gates on one container. Not a double refresh and not a lost redirect. Wanted: script redirects by the request (answer 302 while the requested address has fewer than four `hop=` steps) instead of by arrival count, or give the rig a silence budget that a loaded host cannot reach (10 s), and drive the event loop without the 20 ms wait per pass. The same test and pacing are in 5611210 (`waitFor` went from 8 s to 20 s there; `fetch()` and the rig's 2 s did not). | a shared gate that fails only on a real defect (NFR-TST) |
| S-31 | **Scheduled start of an upcoming video on the fast path.** For `playabilityStatus.status == LIVE_STREAM_OFFLINE` the pinned `PlayerResponseParser` returns `Final` with the reason `final` before it reads anything else (`PlayerResponseParser.cpp:118-123`), so the answer's own start time (`playabilityStatus.liveStreamability.liveStreamabilityRenderer.offlineSlate.liveStreamOfflineSlateRenderer.scheduledStartTime`, Unix seconds as a string) never leaves the component. SailTube then asks yt-dlp to name the case (`StreamResolver::nameFinal`, one token-free pass, 3.4 s on the host) and takes `release_timestamp` from it. Wanted, with S-14: the reason `upcoming` and `ResolveOutcome.streams.metadata.scheduledStartUnixMs` (0 when absent). The adapter already maps the reason `upcoming` and would skip the naming pass. | RES-19 without the extra pass (3.4 s sooner) |

## To SailTube core (gaps found in core's code; no edits were made)

| Id | Gap | Serves |
|---|---|---|
| R-CORE-1 | **Premium is not parsed.** `MediaFormat::premium` now exists, but `YtDlpJson.cpp` still reads `format_note` only to detect DRC, so the flag is always false. The adapter passes it through. | SEL-02, SEL-03 |
| R-CORE-3 | **RES-09 classes collapse.** `classifyFailure` maps removed, terminated, not-available-in-country and generic "unavailable" to one `VideoUnavailable`. It has no "Incomplete YouTube ID" or "try again later" check, so a rate-limit answer worded as "unavailable" ends up final. The adapter maps `VideoUnavailable` to the final `Unavailable` class until core separates `TryLater` (never final), `Removed`, `Terminated` and `GeoBlocked`. | RES-09 |
| R-CORE-4 | **Validator is https-only.** `httpsUrl()` checks only the scheme and the length. It has no host allow-list, no `/videoplayback` check, no single-`expire` check and no HLS manifest hosts. RES-22 asks for SailTune's validator, which should be pinned under `Shared/` (S-04). It is not added here, so that SailTube does not grow a second validator. | RES-22, §1.6 |
| R-CORE-5 | **Headers are not allow-listed.** `parseFormat` copies up to 16 arbitrary `http_headers` entries into each format. A `Cookie` or `Authorization` header from the extractor would reach the player. | RES-20, §1.6 header allow-list |
| R-CORE-6 | **No run timings in the completion.** `DetailsCompletion` does not pass `RunResult::elapsedMs` or the queue wait, so the adapter can report only the total resolve time. | DIA-02 |
| R-CORE-7 | **`filesize_approx` is not parsed.** The adapter falls back to the URL's `clen` parameter when `filesize` is missing. | RES-06, RES-16 |
| R-CORE-8 | **Additions to `Shared/Playback/StreamSource.h`** (all additive, Qt Core only, defaults keep old behaviour): `NetworkClass`; `StreamRequest::signedIn`, `poProvider`, `network`, `clientOverride`; `ProbeState`; `StreamFormat::languagePreference`, `descriptive`, `premium`, `hdr`, `isHls`, `expiresAtUnixMs`, `probe`, `hasVideo()`, `hasAudio()`; `LiveState`; `ResolvePhases`; `StreamSet::liveState`, `scheduledStartUnixMs`, `phases`; `StreamErrorClass` appended `Removed, GeoBlocked, Private, TryLater, StillProcessing, Terminated, InvalidId, Busy, InvalidResponse`; `isFinal()`, `errorLabel()`, `userReason()`, `streamError()`; `streamCacheKey()`, `StreamRequestField`, `streamRequestFieldCount`, `outputAffectingFields()` (RES-10 registry); `StreamResolver::reportRefused(videoId, format)` (virtual, default `invalidate`). Please review and keep. | RES-06, RES-07, RES-09, RES-10, RES-12, RES-19, SEL-02..05 |
| R-CORE-9 | **Manifest hash.** `Shared/SHARED_MANIFEST.json` records `local_sha256` of `StreamSource.h`; it changes with R-CORE-8 (playback does not edit the manifest). | `tools/shared_sync.py --check` |

Note: core's `YtDlpVideoService` has no resolve cache. The adapter therefore has a small one
(`Core/Player/Domain/ResolveCache`, Qt-free, 24 entries, RES-10 freshness, single-flight with
a bounded join). It stays for the yt-dlp fallback; the fast path uses the component's own cache.
