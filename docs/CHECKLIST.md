# SailTube requirement checklist

Status: v3, 2026-11-11. Generated from `SAILTUBE_REQUIREMENTS.md` (v1.0, 2026-11-08, clean-room spec; 171
requirements, every one with acceptance criteria). The requirement ids, levels, variants and phases are the
spec's; the full statement and acceptance text are in the spec (this table only summarises them). This file is
edited by hand; the per-row state is the Status column and the summary below.

## Current status

| Measure | Value |
|---|---|
| code, all phases | 170/171 = 99 % |
| code, MVP | 123/123 = 100 % |
| device (Device = yes) | 0/83 = 0 % |
| rows by status | device-pending 49, code+tests 121, external 1, missing 1 |

## Status vocabulary

| Status | Meaning |
|---|---|
| `code+tests` | Implemented, and host tests that exercise the acceptance criteria pass in the native build |
| `code only` | Implemented and builds, but no automated test covers the criteria |
| `missing` | Not implemented, or only a stub/placeholder |
| `device-pending` | `code+tests`, and the device check in the row (Device = yes) is still to be run on R2 (POCO X3 Pro, SFOS 5.2) |
| `device-ok` | Verified on the device; date and build recorded in the device notes |
| `external` | Blocked by something outside SailTube's code (Harbour policy, Google, a SailTune deliverable, hardware) |

Percentages reported to the user:

* **code** = rows at `code+tests`, `device-pending` or `device-ok`, divided by all rows not `external`;
  reported overall and for the MVP phase;
* **device** = rows at `device-ok`, divided by rows with Device = yes.

Columns: **Var** = `FULL`/`STORE` when the requirement applies to one variant only. **Phase** = the spec's
§8 phasing (MVP, v1, parity).
**Component**: `core`, `playback`, `catalog`, `library`, `ux`, `account`, `release`.
`playback ⇐ SailTune` means the shared instant-playback component produced by SailTune's streaming
component (`docs/SHARED_PLAYBACK.md`); SailTube's playback component integrates it, writes SailTube's tests for
it, and marks the row `external` only while the SailTune deliverable is missing.
**Device** = yes when the acceptance criteria need R1/R2 or a §6 device step.

## Experiments (spec §7)

Each ends with a dated note in `docs/` (method, device, OS, result, decision). Device experiments run
only with the phone owner's yes.

| Id | Question | Component | Device | Status |
|---|---|---|---|---|
| E1 | Embedded WebView sign-in on SFOS 5.x | account | yes | missing |
| E2 | Loopback under Sailjail (phase 0, early) | playback (with SailTune streaming) | yes | missing |
| E3 | `droidvdec` per device | playback | yes | R2 answered 2026-11-08: droidvdec present; software h264/vp9 via gstreamer1.0-libav, no AV1; R1 open |
| E4 | PO tokens on armv7hl | playback | yes | missing |
| E5 | Which HTTP stack gets 206 from googlevideo | playback ⇐ SailTune | yes | missing |
| E6 | Remux without ffmpeg (device muxers) | library | yes | R2 2026-11-08 (POCO run 2): H.264+AAC to MP4 remux works on the phone (`docs/E6_REMUX_HOST.md`); 1080p pairs, VP9+Opus to WebM, the Gallery check and R1 open |
| E7 | qtdemux push-seek on current SFOS GStreamer | playback | yes | missing |
| E8 | Audio role and resource on SFOS 5.x | playback | yes | missing |
| E9 | STORE allowed-library list (Harbour validator) | release | | done 2026-11-08: Harbour refuses `qml(Nemo.KeepAlive)`, `qt5-plugin-sqldriver-sqlite`, `qt5-qtmultimedia-plugin-mediaservice-gstmediaplayer` as Requires; STORE drops them (base system provides the last two) |
| E10 | `X-DBusActivatable` vs Maemo keys | catalog | yes | missing |
| E11 | Lifetime of rotating session cookies | account | yes | missing |
| E12 | Speculative resolve on touch-down | playback | yes | R2 2026-11-08 (POCO run 6): the speculative start runs 82–87 ms before the tap and is used; tap to first frame 1.07–1.71 s; the likelyVisible row prefetch did not fire in that run |

## The maintainer gates

| Id | Gate | Acceptance | Status |
|---|---|---|---|
| REL-01 | Native build green | `tools/native_build.sh --tests` in the build container: 0 errors, link OK, lrelease 10/10 clean, all suites pass | ok on every merge; 87 suites on 2026-11-11 |
| REL-02 | RPM | `rpmbuild` in `sfos-m1-native` with `rpm/harbour-sailtube.spec`: no unpackaged or missing files, FULL and STORE profiles | ok at 9e0e6a0 (`tools/rpm_build.sh`, `--store`) |
| REL-03 | Harbour validator | `rpmvalidation` in `sailfish-mac-probe-20261106` passes for the STORE profile (or the E9 decision documents why STORE is dropped) | ok at 9e0e6a0: 18 PASSED, 0 FAILED (`tools/harbour_validate.sh`) |
| REL-04 | Device smoke test | §6 checklist on R2, only with the phone owner's yes and while no SailTune test runs | runs 1–6 on R2 on 2026-11-08; the beta 3 sitting (`docs/DEVICE_CHECKLIST.md`, `docs/DEVICE_DESIGN.md`) has not run |

## Platform (PLT)

| Id | Level | Var | Phase | Requirement | Acceptance (summary; full text in spec) | Component | Device | Status |
|---|---|---|---|---|---|---|---|---|
| PLT-01 | MUST |  | MVP | Build against the SFOS 5.1 SDK target | the RPM installs and launches on 5.1.x and 5.2.x. On 5.0, the package manager refuses it with a clear dependency message, not a crash. | release |  | device-pending |
| PLT-02 | MUST |  | MVP | CI fails the build if any RPM requirement names a glibc symbol version newer | a CI test with a deliberately newer target fails. | release |  | code+tests |
| PLT-03 | MUST |  | MVP | Keep every data, cache and config path under the Sailjail layout | with sandboxing forced on in a FULL test build, no file access fails outside the features listed as "lost" above. | core |  | code+tests |
| PLT-04 | MUST |  | MVP | The Sailjail permissions for each variant are | a permission audit test maps each declared permission to a feature flag. | core |  | code+tests |
| PLT-05 | MUST |  | MVP | Qt 5.6 JavaScript is ES5 only | the Node syntax gate in NFR-TST-04 passes on every JS file, including inline QML handlers that have been extracted. | ux |  | code+tests |
| PLT-06 | MUST |  | MVP | Silica conventions | a lint script counts pulley items. A QML test destroys a page while a call is in flight. | ux |  | code+tests |
| PLT-07 | MUST |  | MVP | GL context loss on libhybris | §6 step D7 (10 display off/on cycles during playback) shows no black or green frame and no garbled glyphs. | playback | yes | device-pending |
| PLT-08 | MUST |  | MVP | Hardware decoder quirks | with hardware decode off, no pipeline on R1 or R2 contains a droid element. | playback | yes | device-pending |
| PLT-09 | MUST |  | MVP | `/tmp` is tmpfs (RAM) | during a 2-hour video, `/tmp` usage by the process stays under 1 MiB. | playback |  | code+tests |
| PLT-10 | MUST |  | MVP | MPRIS through Amber.Mpris, declared as a hard RPM requirement | §6 step M1. | playback | yes | device-pending |
| PLT-11 | MUST |  | MVP | Keep-alive and blanking | a static check of the STORE RPM and its QML finds no `Nemo.KeepAlive` import or package requirement. A FULL test build with the KeepAlive plugin removed starts, plays and downloads, logging one… | playback | yes | device-pending |
| PLT-12 | MUST |  | MVP | Audio role and resource | a host player test asserts that every audio sink created in dual, muxed, HLS and local modes carries the `media.role` value fixed by E8 and requests the audio resource before PLAYING. §6 step M4… | playback | yes | device-pending |
| PLT-13 | MUST |  | MVP | URL handler and D-Bus activation | §6 steps L1 and L2. | catalog | yes | device-pending |
| PLT-14 | MUST |  | MVP | Process discipline | host tests: (1) a helper started on behalf of a short-lived calling thread is still alive 10 s after that thread exits; (2) after SIGKILL of the app process the helper exits within 2 s; (3) a… | core | yes | code+tests |

## Playback resolution (RES)

| Id | Level | Var | Phase | Requirement | Acceptance (summary; full text in spec) | Component | Device | Status |
|---|---|---|---|---|---|---|---|---|
| RES-01 | MUST |  | MVP | Warm engine | tracing a fast-path resolve shows no fork or exec. The tenth resolve's local overhead (excluding network) is 50 ms or less. | playback ⇐ SailTune |  | code+tests |
| RES-02 | MUST |  | MVP | Isolated playback component | with a 50-channel forced refresh and a comment walk running, tap-to-first-frame stays within NFR-PERF-02 plus 20 %. | playback ⇐ SailTune |  | code+tests |
| RES-03 | MUST |  | MVP | InnerTube fast path | a fixture test asserts the exact body and header set. On R1, at least 90 % of 50 popular non-restricted videos resolve through the fast path. | playback ⇐ SailTune | yes | code+tests |
| RES-04 | MUST |  | MVP | Client definitions follow the extractor | unit tests with synthetic tables. Swapping in a newer extractor changes the client without rebuilding the app. | playback ⇐ SailTune |  | code+tests |
| RES-05 | MUST |  | MVP | Visitor id | the first tap after launch sends exactly one InnerTube request. | playback ⇐ SailTune |  | code+tests |
| RES-06 | MUST |  | MVP | Probe before play | fake-server tests cover these cases: a 403 only on large ranges; a timeout; split verdicts between video and audio. | playback ⇐ SailTune |  | code+tests |
| RES-07 | MUST |  | MVP | Circuit breaker | decision-library tests. The diagnostics show the breaker state and when it ends. | playback ⇐ SailTune |  | code+tests |
| RES-08 | MUST |  | MVP | Fallback ladder | table-driven ladder tests with a fake extractor. Live weekly check (NFR-TST-07). | playback ⇐ SailTune |  | code+tests |
| RES-09 | MUST |  | MVP | Error classification | classification table tests. A removed video in a queue is skipped within 1.5 s. | playback ⇐ SailTune |  | code+tests |
| RES-10 | MUST |  | MVP | Resolve cache | an invariant test enumerates the settings registry and fails if an output-affecting setting is missing from the key. | playback ⇐ SailTune |  | code+tests |
| RES-11 | MUST |  | MVP | Prefetch | the next queue item starts within NFR-PERF-02's prefetched budget. | playback ⇐ SailTune |  | code+tests |
| RES-12 | MUST |  | MVP | Mid-stream self-heal | a fake server that 403s at 30 % of the file. Playback continues with no pipeline rebuild and no audible gap longer than 2 s. | playback ⇐ SailTune |  | code+tests |
| RES-13 | MUST |  | MVP | Tolerate young PO tokens | a fake-server window test. | playback ⇐ SailTune |  | code+tests |
| RES-14 | SHOULD | FULL | v1 | Optional PO-token provider | a bind check test. With the helper killed, playback still works through token-free paths. | playback |  | code+tests |
| RES-15 | MUST |  | MVP | Consistent address family | playback works on NAT64/IPv6-only Wi-Fi. On a dual-stack network with blackholed IPv6, no connect stalls for more than 1 s. | playback ⇐ SailTune |  | code+tests |
| RES-16 | MUST |  | MVP | Media fetcher | sustained throughput of at least 5 MB/s on Wi-Fi and at least 1 MB/s on 4G on R1. | playback ⇐ SailTune | yes | code+tests |
| RES-17 | MUST |  | MVP | Hand-off to the player | SSRF cases are rejected: look-alike hosts, http, localhost, and redirects off the allow-list. A request without the token gets 403. | playback ⇐ SailTune |  | code+tests |
| RES-18 | MUST |  | MVP | Disk-backed read-ahead | pausing for 10 min adds no more than 32 MiB to the spool. | playback ⇐ SailTune |  | code+tests |
| RES-19 | MUST |  | MVP | Live and upcoming | §6 step P6. | playback | yes | device-pending |
| RES-20 | MUST |  | MVP | Anonymous by default | with a session imported, a packet log of the fast path shows no cookies. | playback |  | code+tests |
| RES-21 | MUST |  | MVP | Prewarm at launch | the first tap 2 min after launch meets NFR-PERF-02. | playback ⇐ SailTune |  | code+tests |
| RES-22 | MUST |  | MVP | Output validation | validator tests. | playback ⇐ SailTune |  | code+tests |

## Stream selection (SEL)

| Id | Level | Var | Phase | Requirement | Acceptance (summary; full text in spec) | Component | Device | Status |
|---|---|---|---|---|---|---|---|---|
| SEL-01 | MUST |  | MVP | Codec policy | a format-ranking test on a fixture with AV1, VP9 and H.264 at several heights. | playback |  | code+tests |
| SEL-02 | MUST |  | MVP | Quality ceiling | decision-library tests. Switching from Wi-Fi to 4G applies the mobile ceiling to the next video. | playback |  | code+tests |
| SEL-03 | MUST |  | MVP | Quality menu | a decision-library test on a fixture format list with 720p, 1080p30, 1080p60 and a 1080p Premium rung, each in two codecs, yields exactly one entry per (height, high-fps, Premium) with labels such… | playback | yes | device-pending |
| SEL-04 | MUST |  | MVP | Audio ranking | a decision-library test on a fixture with original, dubbed and descriptive tracks, each in Opus and AAC, with and without DRC, at two bitrates, checks that the four keys apply in order, each key… | playback |  | code+tests |
| SEL-05 | MUST |  | MVP | Dubs | a decision test that reproduces the Polish case. | playback |  | code+tests |
| SEL-06 | MUST |  | MVP | Captions | parser tests on recorded json3. §6 step P9. | catalog | yes | device-pending |
| SEL-07 | MUST |  | MVP | Switches keep the position | 20 switches each land within 1 s of the prior position. | playback |  | code+tests |
| SEL-08 | SHOULD |  | v1 | Listen-only mode | a host player test in listen-only mode builds no video branch, and the fake server receives requests for the audio format only. §6 step P14 on R1 over 4G: a 10 min video in listen-only mode… | playback | yes | device-pending |

## Player (PLY)

| Id | Level | Var | Phase | Requirement | Acceptance (summary; full text in spec) | Component | Device | Status |
|---|---|---|---|---|---|---|---|---|
| PLY-01 | MUST |  | MVP | One player for all modes | the feature matrix test in §6, P1–P6. | playback | yes | device-pending |
| PLY-02 | MUST |  | MVP | Sync model | on a 1080p60 software sync-test clip, A/V offset is 80 ms or less over 5 min, and dropped frames are reported in diagnostics. | playback |  | code+tests |
| PLY-03 | MUST |  | MVP | Seek per container | after 20 random seeks, WebM lands within 100 ms of target and mp4 lands at most one GOP before target, both with A/V offset of 80 ms or less. | playback |  | code+tests |
| PLY-04 | MUST |  | MVP | ISO-BMFF needs a byte-seekable upstream | host player tests (NFR-TST-05) on a generated mp4 fixture served over a link shaped to the Wi-Fi definition: 20 of 20 flush-seeks succeed, none carries the accurate flag on an mp4 branch, each… | playback | yes | device-pending |
| PLY-05 | MUST |  | MVP | Split-seek recovery | a host GStreamer test with an unseekable branch rebuilds at most once. | playback |  | code+tests |
| PLY-06 | MUST |  | MVP | Preroll barrier | a host player test with a resume position of 120 s, a remembered speed of 1.5× and a preroll artificially delayed by 2 s observes exactly one flush-seek, issued after the first async-done and… | playback | yes | device-pending |
| PLY-07 | MUST |  | MVP | Speed | a decision-library test checks the eight steps and that the chosen speed carries over to the next video. A host player test at 2× on a 1 kHz tone fixture measures the output pitch within 2 % of 1… | playback | yes | device-pending |
| PLY-08 | SHOULD |  | MVP | Buffering | a host player test where the fake server stalls the video branch for 5 s shows the pipeline paused and the buffering state raised within 1 s of the underrun, playback resuming only when the buffer… | playback | yes | device-pending |
| PLY-09 | MUST |  | MVP | Background freeze | §6 step P7. CPU budget NFR-PERF-06. | playback | yes | device-pending |
| PLY-10 | MUST |  | MVP | Session continuity | decision-library tests for re-attach and reclaim. §6 step P8. | playback | yes | device-pending |
| PLY-11 | SHOULD |  | v1 | In-app mini-player | §6 step P8. | ux | yes | device-pending |
| PLY-12 | MUST |  | MVP | Display keep-on | a decision-library test covers the keep-on state for playing, paused, listen-only and background, and checks the setting defaults to on. §6 step D8 (FULL, R1 and R2). On a STORE build, the… | playback | yes | device-pending |
| PLY-13 | MUST |  | MVP | Orientation and fullscreen | §6 step P10. | ux | yes | device-pending |
| PLY-14 | SHOULD |  | v1 | Portrait layout | a decision-library layout test on a 1080×2520 portrait screen gives a 16:9 video a 1080×608 box and a 9:16 video a box no taller than 1512 px (60 % of screen height) that is taller than 608 px. §6… | ux | yes | device-pending |
| PLY-15 | MUST |  | MVP | Cutouts | a decision-library test with a synthetic 100 px top cutout puts the windowed video 100 px down with a black strip above it, and keeps the fullscreen picture rectangle entirely inside the safe… | ux | yes | device-pending |
| PLY-16 | MUST |  | MVP | Edge glow | a decision-library test enumerates fullscreen, landscape, playing with controls hidden, paused, controls visible and the always-hide setting, and checks the indicator state in each. §6 P11; in… | ux | yes | device-pending |
| PLY-17 | MUST |  | MVP | Overlay controls | decision tests for tap stacking. §6 step P11. | playback | yes | device-pending |
| PLY-18 | MUST |  | MVP | Recovery UX. | fault-injection test with 3 failures, then success. | playback |  | code+tests |
| PLY-19 | MUST |  | MVP | Renderer survives context loss | §6 step D7 in both software and hardware modes. | playback | yes | device-pending |
| PLY-20 | SHOULD |  | parity | Hardware decode | §6 step D3 on R1 and R2. Results are recorded in E3. | playback | yes | device-pending |
| PLY-21 | MUST |  | MVP | Software decode is the default (PLT-08) | on a clean install the settings show hardware decode off, and the pipeline graph dumped for the first video on R1 and R2 contains no droid element (as in PLT-08). | playback | yes | device-pending |
| PLY-22 | MUST |  | MVP | Audio chain | host player tests check the element order, that a ±15 dB band request is clamped to ±12 dB, the ten band centres, a normalisation gain of 10^(−6/20) ≈ 0.501 for loudnessDb = 6 with the −20 dB… | playback | yes | device-pending |
| PLY-23 | MUST |  | MVP | Audio resource policy | §6 step M3. | playback | yes | device-pending |
| PLY-24 | MUST |  | MVP | Position updates | a host player test counts position updates: at least 4 per second with controls visible, at most 1 per second with controls hidden, none with no consumer attached. The first report after a seek… | playback |  | code+tests |
| PLY-25 | SHOULD |  | v1 | Autoplay | a decision-library test of the autoplay plan: playlists and the queue advance; related autoplay runs only with the setting on, after a 5 s countdown that cancel stops. §6 P2 for playlists and §6… | playback | yes | device-pending |
| PLY-26 | MUST |  | v1 | Local playback of downloads uses the same player, with resume memory, | a host player test plays a downloaded mp4 file and a downloaded WebM pair through the same player component as streams and resumes each within 1 s of the stored position. §6 W2 (resume memory) and… | playback | yes | device-pending |

## Discovery (DSC)

| Id | Level | Var | Phase | Requirement | Acceptance (summary; full text in spec) | Component | Device | Status |
|---|---|---|---|---|---|---|---|---|
| DSC-01 | MUST |  | MVP | Data tiers | fixture tests for "valid empty" versus "shape missing". | catalog |  | code+tests |
| DSC-02 | MUST |  | MVP | WEB client identity | unit tests with a fake homepage and clock: the version comes from the extractor table when present, else from a scrape that sends `SOCS=CAI`; malformed values such as `abc` or `2.2026` are not… | catalog |  | code+tests |
| DSC-03 | MUST |  | MVP | Renderer generations | the fixture suite holds at least one recorded answer per listed renderer generation (old and new row types, Shorts, channel, playlist, continuation, both header kinds); every fixture parses into… | catalog |  | code+tests |
| DSC-04 | MUST |  | MVP | Home | a unit test checks the landing page defaults to Subscriptions and that Recommended is offered only when signed in. The PLT-06 pulley lint passes on every page. §6 step F4. If tabs are implemented,… | catalog | yes | code+tests |
| DSC-04a | SHOULD |  | MVP | Signed-out Home shows Popular rows (user-approved 2026-11-08) | signed out, Home lists the week's most-viewed videos for the locale's region and language from the anonymous search endpoint (no new host), cached 10 min; signed in, Recommended replaces it | catalog |  | code+tests |
| DSC-05 | MUST |  | MVP | Feed, RSS first | a fixture with one dead channel still paints the other channels. | catalog |  | code+tests |
| DSC-06 | MUST |  | MVP | Refresh schedule and stale-while-revalidate | decision-library tests for the interval table, with boundary cases at exactly 2, 14 and 60 days, the hourly retry of empty channels, and "replace only when the id sequence changed". A unit test… | catalog | yes | code+tests |
| DSC-07 | MUST |  | v1 | Feed depth and live | fixture tests: a merged feed from 40 channels with continuations reaches at least 300 items by scrolling; a running stream found by RSS enrichment sorts above every other row with a LIVE badge;… | catalog | yes | code+tests |
| DSC-08 | MUST |  | MVP | Search | encoder tests against YouTube's own canonical codes. | catalog |  | code+tests |
| DSC-09 | MUST |  | MVP | Infinite scroll correctness | a fixture where page 2 is all Shorts still reaches page 3. | catalog |  | code+tests |
| DSC-10 | MUST |  | MVP | Search cache | unit tests with a fake clock: the same (query, filters, kind) within 2 min reuses page 1 with no request; between 2 and 10 min page 1 is fetched again; after 10 min the entry is gone; a ninth… | catalog |  | code+tests |
| DSC-11 | MUST |  | MVP | Suggestions | unit tests with a fake server and clock: five keystrokes within 300 ms send one request carrying `hl` from the device locale; a reply for an older query that arrives after a newer one is dropped;… | catalog | yes | code+tests |
| DSC-12 | MUST |  | MVP | Channels | fixtures for each tab. | catalog |  | code+tests |
| DSC-13 | MUST |  | v1 | Related | a fixture test with a recorded watch-next response and two continuation pages exposes at least 60 related items, in order and without duplicates. | catalog |  | code+tests |
| DSC-14 | MUST |  | MVP | Video info on the playing page | a host test that delays the watch-next reply by 3 s shows tap-to-first-frame within 50 ms of the undelayed run, and the info line filled once the reply arrives. A unit test on a description… | catalog | yes | code+tests |
| DSC-15 | MUST |  | v1 | Chapters | decision-library tests: a description with ≥ 3 valid timestamps starting at 0:00 yields chapters; inputs violating each rule (< 3 entries, first ≠ 0:00, non-increasing or < 10 s apart, start ≥… | catalog | yes | code+tests |
| DSC-16 | MUST |  | v1 | Comments | the comment fixture passes. §6 step C1. | catalog | yes | device-pending |
| DSC-17 | MUST |  | v1 | One Shorts rule everywhere | fixture tests for a 2:50 Short and a 2:50 regular video. | catalog |  | code+tests |
| DSC-18 | MUST |  | v1 | Members-only and restricted rows are badged, or hidden by setting, and never | fixture tests with members-only and age-restricted rows: with the hide setting off they show a badge; with it on they are hidden and the list shows a hidden-row count equal to the number removed.… | catalog |  | code+tests |
| DSC-19 | MUST |  | MVP | Images | unit tests: list rows use `mqdefault` and details use `hqdefault`; a protocol-relative avatar URL with `=s88` becomes `https:` with `=s176`; a 176×175 avatar is accepted and a 176×170 one… | catalog |  | code+tests |

## Library (LIB)

| Id | Level | Var | Phase | Requirement | Acceptance (summary; full text in spec) | Component | Device | Status |
|---|---|---|---|---|---|---|---|---|
| LIB-01 | MUST |  | MVP | Subscriptions | unit tests: subscribing twice gives one entry; unsubscribe removes it; both sort orders are correct on a 20-channel fixture; a simulated crash between write and rename leaves the previous store… | library |  | code+tests |
| LIB-02 | MUST |  | v1 | Local playlists | unit tests: create, rename and delete a list; adding the same video twice leaves one entry; a removal commits only after the remorse delay, and cancelling it restores the item. Where implemented… | library |  | code+tests |
| LIB-03 | MUST |  | v1 | Remote playlists | fixture tests with recorded `VL` browse pages and a synthetic continuation chain: a 5000-item playlist loads all 5000 items in order, and refreshing a saved mirror replaces its contents. §6 L2:… | library | yes | code+tests |
| LIB-04 | MUST |  | MVP | History | decision-library tests: watched at 80 % and at 15 s before the end, not at 79 % and 16 s; watched stays set after a later partial view; manual mark and unmark; with the cap at 100 the 101st record… | library |  | code+tests |
| LIB-05 | MUST |  | MVP | Resume | decision-library tests with a fake clock: saves happen every 30 s while playing and on leave, stop and end; a position of 9 s, or 14 s before the end, stores "none"; an entry 31 days old is gone;… | library | yes | code+tests |
| LIB-06 | MUST |  | MVP | Watched indicators | a QML test shows the progress bar at 0.6 % and hides it at 0.4 %, and a tap on the badge area still opens the video. A unit test with 5000 history entries measures the data sent to the UI after… | library |  | code+tests |
| LIB-07 | SHOULD |  | v1 | Hide watched in the feed and channel lists, and optionally in search | a fixture test with 3 watched videos among 10 rows, in the feed and in a channel list: with the setting on, 7 rows show in each and paging still continues (DSC-09). With the search option on, the… | library |  | code+tests |

## Account (ACC)

| Id | Level | Var | Phase | Requirement | Acceptance (summary; full text in spec) | Component | Device | Status |
|---|---|---|---|---|---|---|---|---|
| ACC-01 | MUST |  | v1 | Never block on auth | with no session, and again with a session whose verification is rejected, fixture tests for playback, search, feed refresh and a download all pass. A fault-injection test fails account… | account | yes | code+tests |
| ACC-02 | MUST |  | v1 | Sign-in options, offered in this order | a QML test checks the sign-in page lists the methods in rank order: on FULL, browser import first; on STORE, no browser import, so paste or file import is first. WebView appears only when ACC-05… | account | yes | code+tests |
| ACC-03 | MUST | FULL | v1 | Browser import | a unit test where a fresh login exists only in the WAL. §6 step A1. | account | yes | code+tests |
| ACC-04 | MUST |  | v1 | Paste and file import using SailTune's parser, with the same gate | parser tests on both variants reuse SailTune's corpus for the Cookie header, copied headers, cURL and Netscape formats. An input with neither `__Secure-3PAPISID` nor `SAPISID` is refused with a… | account | yes | code+tests |
| ACC-05 | MAY |  | parity | WebView sign-in, only after E1 passes | if implemented: E1's pass criterion holds on R1 and R2, and §6 step A3 passes. The PLT-04 permission audit shows `WebView` declared only in builds that include this feature. | account | yes | missing |
| ACC-06 | MUST |  | v1 | Authenticated requests | unit tests with a fixed timestamp and fixture cookies check the Cookie header order and that missing names are skipped, the SAPISIDHASH value against a precomputed SHA-1 vector, the fall-back to… | account |  | code+tests |
| ACC-07 | MUST |  | v1 | Verification | unit tests over recorded `account/account_menu` answers map signed-in → accepted, anonymous-200 → rejected, and network/HTTP errors → inconclusive with the previous state unchanged. A fake clock… | account | yes | code+tests |
| ACC-08 | MUST |  | v1 | Rotation write-back | unit tests: a response rotating `__Secure-3PSIDCC` updates the stored value; an untracked new cookie is ignored; an unchanged response writes nothing (store modification time unchanged); 20… | account |  | code+tests |
| ACC-09 | MUST |  | v1 | Logins and brand channels | a concurrency test that selects accounts while other calls run. | account |  | code+tests |
| ACC-10 | MUST |  | v1 | Storage | FULL unit test with the vault unavailable: the session goes to a 0600 file written by temp file and rename, and the disclosure shows once across two restarts. STORE unit test with the vault… | account | yes | code+tests |
| ACC-11 | SHOULD |  | parity | Signed-in features | with a fixture account session: subscriptions and playlists import (IMP-02 tests); Recommended home, Watch Later and Liked render from recorded fixtures; an age-restricted fixture video resolves… | account | yes | code+tests |

## Imports and export (IMP)

| Id | Level | Var | Phase | Requirement | Acceptance (summary; full text in spec) | Component | Device | Status |
|---|---|---|---|---|---|---|---|---|
| IMP-01 | MUST |  | MVP | NewPipe and PipePipe backup | fixture databases from two NewPipe versions. | library |  | code+tests |
| IMP-02 | MUST |  | v1 | Import from the account when signed in | fixture tests with recorded `FEchannels` and `FEplaylist_aggregation` pages import every UC id and playlist id, import no UC row as a playlist, stop after 20 pages on a 25-page fixture and at the… | account | yes | code+tests |
| IMP-03 | SHOULD |  | parity | Google Takeout `subscriptions.csv` import, which needs no sign-in | fixture tests with a Takeout `subscriptions.csv` that has a header row, a UTF-8 BOM and quoted titles containing commas import every channel id with no sign-in, and a re-import adds no duplicates.… | library |  | code+tests |
| IMP-04 | SHOULD |  | parity | Export a SailTube backup (subscriptions, playlists, history, settings; no | a round-trip test exports a populated library, wipes it, imports the file, and gets identical subscriptions, playlists, history and settings. A scan of the exported file finds no ACC-06 cookie… | library |  | code+tests |

## SponsorBlock (SB)

| Id | Level | Var | Phase | Requirement | Acceptance (summary; full text in spec) | Component | Device | Status |
|---|---|---|---|---|---|---|---|---|
| SB-01 | MUST |  | v1 | Fetch segments privately | unit tests with a fake server: the request path is the first 4 hex characters of sha256(videoId) and never the id itself; segments for other ids are discarded; a 404 gives no segments; segments… | catalog | yes | code+tests |
| SB-02 | MUST |  | v1 | Decide skips | decision-library tests. §6 step S1. | playback | yes | device-pending |

## Return YouTube Dislike (RYD)

| Id | Level | Var | Phase | Requirement | Acceptance (summary; full text in spec) | Component | Device | Status |
|---|---|---|---|---|---|---|---|---|
| RYD-01 | MAY |  | parity | Return YouTube Dislike | if implemented: unit tests show no request to `returnyoutubedislikeapi.com` with the setting off (the default), and one request per video with it on, reused for 10 min. A QML test finds the… | catalog |  | code+tests |

## Downloads (DL)

| Id | Level | Var | Phase | Requirement | Acceptance (summary; full text in spec) | Component | Device | Status |
|---|---|---|---|---|---|---|---|---|
| DL-01 | MUST |  | v1 | Kinds | fixture tests: with itag 140 present, an audio download picks it; without it, the best Opus is picked and saved in WebM with a matching extension. A video download's picker defaults to the SEL-02… | library | yes | code+tests |
| DL-02 | MUST |  | v1 | Engine | offline engine tests (NFR-TST-02): an interrupted transfer resumes with a Range request from the saved offset and ends at exactly `clen` bytes; an expired URL mid-transfer re-resolves the same… | library | yes | code+tests |
| DL-03 | MUST |  | v1 | Video without bundling ffmpeg | §6 step W2. The output plays in the system gallery when muxed. | library | yes | device-pending |
| DL-04 | MUST |  | v1 | Progress everywhere | a QML test starts downloads from a feed row, a search row, a channel row and the watch page, and checks that the downloads page, the row badge in each list and the watch-page indicator update at… | library | yes | code+tests |
| DL-05 | MUST |  | v1 | File names | unit tests: a 300-byte CJK title is cut on a character boundary to at most 150 bytes; the whole name is at most 255 bytes; titles containing `/`, `..`, `%(title)s`, NUL or a leading dot produce a… | library |  | code+tests |
| DL-06 | MUST |  | v1 | Location | unit tests: a read-only folder is refused when chosen; a chosen folder that is missing at download time sends the file to the private dir and raises a visible notice. §6 W3. | library | yes | code+tests |
| DL-07 | MUST |  | v1 | Offline playback in flight mode, with optional saved thumbnail, captions and | §6 W2 in flight mode, plus §6 step W4: the downloaded video plays with its saved thumbnail, captions and SponsorBlock segments, resumes within 1 s of the stored position, and shows no network error. | library | yes | code+tests |
| DL-08 | SHOULD |  | parity | "Download all" for a playlist, skipping items already downloaded | a unit test on a 10-item playlist with 3 items already downloaded queues exactly 7; running "Download all" a second time queues nothing. | library |  | code+tests |
| DL-09 | SHOULD | FULL | parity | A KeepAlive background job keeps downloads running while the app is in | FULL build, §6 step W5. STORE build: a static check of the RPM finds no KeepAlive (PLT-11), and the downloads page states that downloads run only while the app is open. | library | yes | code+tests |

## Cover and MPRIS (MPR)

| Id | Level | Var | Phase | Requirement | Acceptance (summary; full text in spec) | Component | Device | Status |
|---|---|---|---|---|---|---|---|---|
| MPR-01 | MUST |  | MVP | MPRIS (Amber.Mpris) | a D-Bus test against the running app reads CanPlay, CanPause, CanControl and CanSeek as true, and CanGoNext and CanGoPrevious as true only with a queue. SetPosition and Seek land within 1 s of the… | playback | yes | device-pending |
| MPR-02 | MUST |  | MVP | Cover | a QML test covers the three action states (queue: play/pause and next; no queue: play/pause and stop; idle). §6 step M6. | playback | yes | device-pending |
| MPR-03 | MUST |  | MVP | Lock-screen controls and headset buttons work on SFOS 5.1 and 5.2 | §6 steps M1 and M2. | playback | yes | device-pending |

## Link handling (LNK)

| Id | Level | Var | Phase | Requirement | Acceptance (summary; full text in spec) | Component | Device | Status |
|---|---|---|---|---|---|---|---|---|
| LNK-01 | MUST |  | MVP | Hosts | a static check of the desktop file finds an `x-url-handler/<host>` MIME type for each listed host, and the `music.youtube.com` handler only in the subpackage or install path meant for it. §6 L1… | catalog | yes | code+tests |
| LNK-02 | MUST |  | MVP | Forms | a table test of links. | catalog |  | code+tests |
| LNK-03 | MUST |  | MVP | D-Bus handling | §6 steps L1 and L2. | catalog | yes | device-pending |
| LNK-04 | MUST |  | MVP | Routing | decision-library routing tests for a video, a video with `list=`, a playlist and a channel check the target page, and that the video-with-list case seeds the queue with that list, positioned at… | catalog | yes | code+tests |

## Settings (SET)

| Id | Level | Var | Phase | Requirement | Acceptance (summary; full text in spec) | Component | Device | Status |
|---|---|---|---|---|---|---|---|---|
| SET-01 | MUST |  | MVP | Inventory and defaults | the NFR-TST-03 invariant test checks that a fresh install's defaults table matches every row above, per variant, with the `[FULL]` rows absent on STORE. | ux |  | code+tests |
| SET-02 | MUST |  | MVP | Each control lives in exactly one place | a test enumerates the settings registry and the QML settings pages and fails if any key has no control or more than one. A test collects every error message that links to a setting and checks the… | ux |  | code+tests |
| SET-03 | MUST |  | MVP | Defaults live in one table that the engine, the UI and the tests share | the NFR-TST-03 invariant test extracts the default literals from the UI and fails if any differs from the shared table; changing one table value in a scratch build makes it fail. | ux |  | code+tests |

## Diagnostics (DIA)

| Id | Level | Var | Phase | Requirement | Acceptance (summary; full text in spec) | Component | Device | Status |
|---|---|---|---|---|---|---|---|---|
| DIA-01 | MUST |  | MVP | Counters by class | fixture tests for each class. | core |  | code+tests |
| DIA-02 | MUST |  | MVP | Report | a unit test builds a report after a fixture session with a signed-in account, a running PO helper (FULL) and 10 resolves. Every listed section is present; each of the 10 timelines has the five… | core |  | code+tests |
| DIA-03 | SHOULD |  | MVP | An opt-in timestamped timing log for the performance measurements in §4 | with the log off (the default), no timing file is written. With it on, §6 T1: the exported log has one timestamped line per phase for every P1, P2 and P3 run, enough to compute the NFR-PERF-02 and… | core | yes | code+tests |

## Internationalisation (I18N)

| Id | Level | Var | Phase | Requirement | Acceptance (summary; full text in spec) | Component | Device | Status |
|---|---|---|---|---|---|---|---|---|
| I18N-01 | MUST |  | MVP | Discovery requests send `hl` and `gl` from the device locale, so titles, | a fixture/unit test asserts that discovery requests carry `hl`/`gl` derived from the device locale (de_DE → `hl=de`, `gl=DE`); a static check lists every request that hard-codes `hl=en` or… | catalog | yes | code+tests |
| I18N-02 | MUST |  | MVP | Parsers prefer structured fields: `videoDetails.viewCount`, microformat | parser tests run under the C, de_DE and ja_JP process locales give identical results; fixtures with unparseable count or date text still produce the row with the raw text shown; structured fields… | catalog |  | code+tests |
| I18N-03 | MUST |  | MVP | Every translation in the repository is compiled and shipped | a build check fails if any `.ts` in the repository has no matching `.qm` in the RPM; on a device set to a language without a translation, plural strings still read correctly in English (§6 I3). | ux | yes | device-pending |
| I18N-04 | SHOULD |  | parity | Parser fixtures are recorded in at least 3 locales: en, de and one non-Latin | the fixture set contains recorded answers for search, the feed/channel and watch-next in en, de and one non-Latin locale, and the parser suite passes on all of them. | catalog |  | code+tests |

## Performance (NFR-PERF)

| Id | Level | Var | Phase | Requirement | Acceptance (summary; full text in spec) | Component | Device | Status |
|---|---|---|---|---|---|---|---|---|
| NFR-PERF-01 | MUST |  | MVP | Cold start to first Silica frame | Target: p50 ≤ 1.5 s, p95 ≤ 2.5 s (measured on R1/R2, §6 T1) | ux | yes | device-pending |
| NFR-PERF-02 | MUST |  | MVP | Tap to first video frame, fast path, Wi-Fi | Target: p50 ≤ 1.2 s, p95 ≤ 2.5 s (measured on R1/R2, §6 T1) | playback | yes | device-pending |
| NFR-PERF-03 | MUST |  | MVP | Seek inside the buffer | Target: p50 ≤ 300 ms (measured on R1/R2, §6 T1) | playback | yes | device-pending |
| NFR-PERF-04 | MUST |  | MVP | Quality or dub switch to resumed playback | Target: ≤ 2 s on Wi-Fi (measured on R1/R2, §6 T1) | playback | yes | device-pending |
| NFR-PERF-05 | MUST |  | MVP | Memory (PSS) | Target: ≤ 300 MB during 1080p software playback; ≤ 180 MB on the feed (measured on R1/R2, §6 T1) | playback | yes | device-pending |
| NFR-PERF-06 | MUST |  | MVP | Background, screen off | Target: Video decode stops within 1 s of hiding (+800 ms grace); mean CPU ≤ 8 % of one core over 10 min of Opus audio; no app timer above 1 Hz (measured on R1/R2, §6 T1) | playback | yes | device-pending |
| NFR-PERF-07 | SHOULD |  | v1 | Feed scroll with thumbnails | Target: No frame over 50 ms in a 30 s fling test (QML profiler) (measured on R1/R2, §6 T1) | ux | yes | device-pending |
| NFR-PERF-08 | MUST |  | v1 | 50 comments with replies | Target: ≤ 2 s on Wi-Fi, off the playback component (measured on R1/R2, §6 T1) | catalog | yes | device-pending |
| NFR-PERF-09 | SHOULD |  | v1 | Search page 1 | Target: p50 ≤ 1.0 s; suggestions ≤ 300 ms after the debounce (measured on R1/R2, §6 T1) | catalog | yes | device-pending |

## Robustness (NFR-ROB)

| Id | Level | Var | Phase | Requirement | Acceptance (summary; full text in spec) | Component | Device | Status |
|---|---|---|---|---|---|---|---|---|
| NFR-ROB-01 | MUST |  | MVP | Degrade, never break: every discovery and resolve path has a fallback, and | fault-injection fixtures. | core |  | code+tests |
| NFR-ROB-02 | MUST |  | MVP | Stores are written atomically: a private temp file, then rename | a host test kills the engine with SIGKILL at random points during 200 store writes; afterwards every store parses and holds either the old or the new content. A kill during playback leaves the… | core |  | code+tests |
| NFR-ROB-03 | MUST |  | MVP | No worker lane can hang for good | a host test blocks one lane with a call that never returns: within 35 s the watchdog logs the stuck lane, pending calls on it fail with "busy, retry", other lanes keep serving, and the worker… | core |  | code+tests |
| NFR-ROB-04 | MUST |  | MVP | Updating the extractor while it is imported does not break later imports | a host test imports the extractor, replaces its `.pyz` mid-session, then runs 10 resolves and a lazy import of a not-yet-loaded extractor module: all succeed on the old copy. Diagnostics show the… | core |  | code+tests |

## Privacy and security (NFR-SEC)

| Id | Level | Var | Phase | Requirement | Acceptance (summary; full text in spec) | Component | Device | Status |
|---|---|---|---|---|---|---|---|---|
| NFR-SEC-01 | MUST |  | MVP | A static network-inventory test, reusing SailTune's tool, allows only | the static inventory test runs in CI on both variants and fails when a scratch change adds an unlisted host; the `[FULL]` hosts fail the STORE run. A scan of the RPM finds no telemetry or… | core | yes | code+tests |
| NFR-SEC-02 | MUST |  | MVP | SSRF protections as in RES-17 | the RES-17 SSRF test set runs in CI and passes. If hand-off option (a) is used, a socket listing during playback on R1 shows no listening socket owned by the app. | core | yes | code+tests |
| NFR-SEC-03 | MUST |  | MVP | Secrets as in ACC-10 | the ACC-10 tests pass. A test runs sign-in, resolve, rotation and sign-out, then scans the logs and a diagnostics report for every cookie value used and finds none. A file-system scan after each… | core |  | code+tests |
| NFR-SEC-04 | MUST | FULL | MVP | Runtime downloads | FULL build, unit tests against a fake release host: an http URL, a redirect to an unlisted host, an invalid certificate, a wrong SHA-256, a missing or bad signature where one is published, and an… | core |  | code+tests |
| NFR-SEC-05 | MUST |  | MVP | File names as in DL-05 | the DL-05 name tests, the NFR-SEC-01 inventory test and (FULL) the RES-14 bind check test all pass. | core |  | code+tests |
| NFR-SEC-06 | MUST |  | MVP | Licence review before bundling any third-party component | a CI check fails if any component in the NFR-MNT-02 manifest lacks a licence or a completed-review flag. A static check of the RPM file list finds no runtime helper (Deno, the PO provider, ffmpeg)… | core |  | code+tests |

## Testability (NFR-TST)

| Id | Level | Var | Phase | Requirement | Acceptance (summary; full text in spec) | Component | Device | Status |
|---|---|---|---|---|---|---|---|---|
| NFR-TST-01 | MUST |  | MVP | Recorded fixtures | a CI test checks the fixture set holds every listed kind. A recorder test feeds an answer with a real-looking googlevideo IP, `sn-` node names and a visitor id with a country, checks the saved… | catalog |  | code+tests |
| NFR-TST-02 | MUST |  | MVP | Offline engine suite with no network | the suite runs in CI with networking disabled (empty network namespace) and passes. Each of the six cases is a named test, and removing the 416 handling in a scratch build makes the suite fail. | core |  | code+tests |
| NFR-TST-03 | MUST |  | MVP | Invariant tests | each of the four invariants is a named CI test, and seeding one violation per invariant (a new output-affecting setting outside the key, a changed UI literal, a category with no UI row, a… | core |  | code+tests |
| NFR-TST-04 | MUST |  | MVP | Pure decision library | CI runs the library under Node in a bare `vm` context with no QML stubs, and each listed decision has its own test. A seeded `let`, arrow function or template string in code fails the syntax gate,… | ux |  | code+tests |
| NFR-TST-05 | MUST |  | MVP | Player tests on the host | CI runs the host GStreamer suite on generated WebM and MP4 fixtures, with named tests for seek accuracy, split-seek with the rebuild cap, the preroll barrier, freeze and thaw, and EOS, using the… | playback |  | code+tests |
| NFR-TST-06 | MUST |  | v1 | A coverage ratchet: a new engine function without a test fails CI, unless | in a scratch branch, adding an untested engine function fails CI, and so does adding a new entry to the baseline. | release |  | code+tests |
| NFR-TST-07 | MUST |  | v1 | Weekly live check from an aarch64 runner, with a residential-IP runner | the weekly job runs on an aarch64 runner and its log shows the fast path, the extractor ladder, the discovery endpoints and a small download. A bot-wall or 429 result is reported as skipped. The… | release |  | external |
| NFR-TST-08 | MUST |  | MVP | Device checklist (§6) | a doc check fails if a §6 step has no "Auto:" note (naming tests, or "none") or if an area in the change-to-section table has no step. A timed run on R1 takes 8 min or less for the smoke set and… | release | yes | code+tests |

## Maintainability (NFR-MNT)

| Id | Level | Var | Phase | Requirement | Acceptance (summary; full text in spec) | Component | Device | Status |
|---|---|---|---|---|---|---|---|---|
| NFR-MNT-01 | MUST | FULL | v1 | Extractor updates without an app release | FULL build: unit tests per NFR-SEC-04, and §6 step I4. A process trace during the update shows the `.pyz` is imported and never executed. STORE build: settings show no update control, and a static… | core | yes | code+tests |
| NFR-MNT-02 | MUST |  | MVP | A pinned-versions manifest (name, version, size, SHA-256, licence) for | a CI check compares the manifest with the RPM contents: every bundled or managed component has a row with name, version, size, SHA-256 and licence; sizes and hashes match the shipped bytes; no… | core |  | code+tests |
| NFR-MNT-03 | MUST |  | MVP | Release hygiene | CI tests: a tag that disagrees with the spec version fails the build; a tag on a commit not on main fails the release job; tags containing a hyphen are released as pre-releases; two CI builds of… | release |  | code+tests |
| NFR-MNT-04 | SHOULD |  | MVP | Client definitions and endpoint parameters live in one place, with a | a static check finds client names, client versions and endpoint parameter strings only in the one definitions file, and every value there is referenced by a fixture. In a scratch branch, changing… | core |  | code+tests |
