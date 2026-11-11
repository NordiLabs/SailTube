# SailTube device checklist (NFR-TST-08)

Status: v2, 2026-11-11 (62 steps; the section "Beta 3 sitting: playback" was added on 2026-11-10). One line per step of §6 of the clean-room requirements
(`SAILTUBE_REQUIREMENTS.md`, "Acceptance test plan: device checklist"). The step ids (I1, P1, …) and
the requirement ids are the spec's; the wording here is SailTube's own summary. The device step only
confirms that what the automated suites already prove reaches the screen, the speaker or the system.

Checked by `tools/check_device_checklist.py` (suite `tests/device_checklist`). Rules for editing:

* Each step is one list line that starts with `- **<ID>.**` and ends with an `*Auto: …*` note.
* The note names the host suites that cover the step as `tests/<suite>` (each must exist as
  `tests/<suite>/run.sh`), optionally followed by a short note in parentheses, or says `none` when
  nothing automated covers it yet. Do not name a suite that only touches the area loosely.
* `[smoke]` after the id puts the step in the smoke subset.
* Every area in the change table at the end lists at least one step id from this file.

## When and where to run

* Devices: R1 and R2 (R2 = POCO X3 Pro, Sailfish OS 5.2). Builds: the FULL and STORE profiles; a
  step marked "(FULL build)" or "(STORE build)" runs on that profile only.
* Full run: every step, first on a clean install, then again after upgrading over the previous
  release with its data kept. Target: 30 min per device and pass, hard limit **45 min**.
* Smoke run: the `[smoke]` steps only, once, at reduced counts (3 taps instead of 20, 1 cold start
  instead of 10, one link instead of one per host). Target: 5 min, hard limit **8 min**. Run it for
  every RPM handed to a tester; run the sections the change table names for the change on top.
* Record results in a dated note in `docs/` (device, OS, build, profile, step, result). The timed
  run itself is a device item; the times above are the budget to record against.

Smoke subset (9 steps): I1, I2, P1, P3, P8, M1, F1, L1, W1.

## Install and start

- **I1.** [smoke] Install the RPM on 5.1 and 5.2; on 5.0 the install is refused with a clear message, not a crash (PLT-01). *Auto: tests/rpm_audit (PLT-02 glibc ceiling of the 5.1 target, on every built RPM via tools/rpm_build.sh), tests/release (single-source version, RPM payload compare).*
- **I2.** [smoke] Cold start ten times; note when the first frame and the cached feed appear and compare with NFR-PERF-01. *Auto: tests/qml, tests/library_qml (every page compiles and loads; no start-up timing). Container trace: tools/startup_trace.sh, docs/PERF_STARTUP.md.*
- **I3.** Change the phone language to German and restart: UI strings are translated, dates use the German format, and feed rows lose no field (I18N). *Auto: none.*
- **I4.** (FULL build.) Update the extractor from settings on the stable channel, relaunch and see the new version in diagnostics; roll back and see the old one; tamper the installed copy (docs/EXTRACTOR_UPDATE.md) and see it refused at the next start, the bundled or previous copy kept (NFR-MNT-01, NFR-SEC-04). *Auto: tests/extractor_update (fake release host: refusals, aborts, rollback, never executed; the real 2026.10.26 signature).*

## Playback

- **P1.** [smoke] Open 20 fresh popular videos on Wi-Fi and 20 on mobile data; read tap-to-first-frame from the timing log against NFR-PERF-02 and check in diagnostics that at least 90 % used the fast path. *Auto: tests/sharedplayback (fast path, proxy), tests/domain (stream choice), tests/resolver_qt (fallback parsing), tests/warm_worker and tests/resolver_bench (warm fallback resolver, measured cold vs warm).*
- **P2.** Let a playlist advance by itself: the prefetched next video starts with a p50 of 0.6 s or better. *Auto: tests/sharedplayback (resolve cache, single flight).*
- **P3.** [smoke] On a WebM and an mp4 video, do 10 seeks with the scrubber and 3 bursts of double taps: audio stays in sync and each burst ends in a single seek (PLY-03, NFR-PERF-03). *Auto: tests/sharedplayback (mid-stream seek in the proxy), tests/domain (mp4 index, DASH manifest).*
- **P4.** On a video with several audio languages, change quality 3 times and dub 2 times without losing the position; choose the original track, then open an English video and confirm no dub is applied (SEL-05/07). *Auto: tests/domain (quality menu, original-language and DRC choice).*
- **P5.** Set speed to 1.5×, open another video: it starts at 1.5× straight away, with no visible jump from 1× (PLY-06/07). *Auto: none.*
- **P6.** Play a running live stream, an upcoming premiere and a stream that just ended: the first plays, the second shows its start time, the third shows a clear message (RES-19, RES-09). *Auto: tests/domain (live streams), tests/sharedplayback (playability), tests/resolver_qt (failure mapping).*
- **P7.** Play 1080p, lock the screen for 10 min while measuring CPU, then unlock: no audio gap, no black frame, CPU within budget; repeat in muxed and HLS modes (PLY-09). *Auto: none.*
- **P8.** [smoke] Go back while a video plays and get the mini-player; open video B, return to A: A takes the player back and B sees no stray seek or skip; reopen A from the mini-player and it re-attaches without reloading (PLY-10/11). *Auto: none.*
- **P9.** On a video with captions in 3 languages plus auto-translations, pick a translation, then open the next video: the automatic caption choice follows the SEL-06 order. *Auto: none.*
- **P10.** With rotation unlocked, rotate a landscape video; fullscreen a vertical Short and confirm it stays portrait; leave fullscreen and confirm auto-rotate still works; check a notch device in both modes (PLY-13/15). *Auto: none.*
- **P11.** Try single taps, double taps and taps close to the screen edges on the controls; the glow is hidden in fullscreen and shown in paused portrait (PLY-16/17). *Auto: none.*
- **P12.** Pause for 20 min, then resume: playback continues, or recovers by itself within 3 s, with no error shown (RES-12). *Auto: tests/sharedplayback (address expiry and refresh rules).*
- **P13.** On the strace or diagnostics build, play for 5 min with no 403 reaching the user; in a debug build inject a 403 at 30 % and playback carries on (RES-12). *Auto: tests/sharedplayback (refusal retried in place, resume at the same byte), tests/privacy (static host inventory; the strace run is its device counterpart).*
- **P14.** On mobile data, play a 10 min video in listen-only mode: diagnostics show only the audio format was fetched and CPU is below the 480p figure; the next video is listen-only too; after a restart the mode is off again (SEL-08). *Auto: tests/domain (audio-only chain), tests/sharedplayback (audio ladder).*
- **P15.** Play 1080p and throttle the network to 1 Mbit/s for 30 s: a buffering indicator appears, playback pauses and then resumes by itself in sync and without an error (PLY-08). *Auto: tests/sharedplayback (dropped connections, flow control; no player stall test yet).*
- **P16.** In portrait windowed mode, open a landscape video, then a vertical Short: the landscape box is screen-wide at 16:9, the Short is taller but at most 60 % of the screen height; on a cutout device the video starts below the cutout with black above it (PLY-14/15). *Auto: none.*
- **P17.** While playing, switch between two EQ presets, set boost to 300 % and enable loudness normalisation: each change is audible within about 0.25 s, with no gap, position jump or clipping (PLY-22). *Auto: none.*
- **P18.** With related autoplay on, let a video finish: a 5 s "up next" countdown appears and cancel stops it; on a second try let it expire: the next video replaces the watch page and Back goes to the list (PLY-25). *Auto: none.*

## Device and platform

- **D3.** Turn hardware decoding on: the probe runs and its result is recorded; play H.264 and VP9 at 1080p; when no frame arrives within 3 s the app falls back and says so (PLY-20). *Auto: tests/resolver_qt (probe mapping), tests/domain (decoder limits).*
- **D7.** Switch the display off and on 10 times while a video plays: no green or black frames and no corrupted text (PLT-07). *Auto: none.*
- **D8.** (FULL build.) With a 30 s system display timeout, play in the foreground for 2 min and the display stays on; pause and it blanks after the timeout; in listen-only mode it blanks while playing (PLT-11, PLY-12). *Auto: none.*
- **D9.** (FULL build, PO provider on.) Kill SailTube with SIGKILL over SSH: its helper process is gone within 2 s (PLT-14). *Auto: tests/process (parent-death signal, owner thread, trimmed PATH, message pattern).*

## Media integration

- **M1.** [smoke] Lock-screen controls work for play/pause, next and seek, and show artwork and duration (MPR-01). *Auto: none.*
- **M2.** Wired headset button and Bluetooth play/pause both toggle playback. *Auto: none.*
- **M3.** Receive a call during playback: it pauses within 1 s and afterwards does what the setting says (PLY-23). *Auto: none.*
- **M4.** While playing, list the PulseAudio sink inputs: SailTube's stream has the media role chosen in E8 (PLT-12). *Auto: none.*
- **M5.** Send MPRIS Stop (D-Bus tool or Bluetooth stop key) during playback: playback ends, the mini-player closes, and the video reopens at the saved position (MPR-01). *Auto: tests/library_tracker (save on stop), tests/library_domain (resume decision).*
- **M6.** With a queue, the cover shows thumbnail, title, channel and moving progress, and play/pause and next work; without a queue it offers stop, which ends the session; after stop it shows the idle state (MPR-02). *Auto: none.*

## Links

- **L1.** [smoke] With SailTube closed, open a `https://youtu.be/<id>?t=90` link from Messages: playback starts at 1:30. *Auto: none.*
- **L2.** With SailTube running, open a watch link with `list=`, a playlist link, an `@handle` link and two links in quick succession: each goes where LNK-04 says and none is dropped. *Auto: tests/library_domain (channel and handle URL parsers).*
- **L3.** Open one link per LNK-01 host from Messages: each lands on the right SailTube page; with SailTune installed, a `music.youtube.com` link goes to SailTune instead (LNK-01). *Auto: none (no desktop-file check yet).*

## Account

- **A1.** Sign in in a normal browser tab and import: verification passes; switch to a brand channel and import its subscriptions; sign out in the browser and after the 15 min check the expired banner appears; import again; sign out of SailTube and every secret is gone (ACC). *Auto: none.*
- **A2.** (STORE build.) Paste a cookies.txt: the import succeeds. *Auto: none.*
- **A3.** (Only if ACC-05 is built.) Sign in through the in-app WebView page: verification (ACC-07) passes; after signing out of SailTube the WebView profile has no YouTube or Google cookies left (ACC-05). *Auto: none.*

## Discovery

- **C1.** Comments load automatically as the setting says, replies expand, a second page of replies loads and the sort order switches (DSC-16). *Auto: none.*
- **F1.** [smoke] Start in flight mode: the cached feed paints; go online: channels that are due refresh and the scroll position stays; add a terminated channel and the rest of the feed is unaffected; scroll past 100 items (DSC-05/06/07). *Auto: none.*
- **F2.** Search with the "last hour" and "live" filters and hide-Shorts on, and scroll 5 pages (DSC-08/09). *Auto: tests/resolver_qt (listing parser; no filter test).*
- **F3.** On a channel, open the Videos, Shorts, Live and Playlists tabs; then open a channel without uploads (DSC-12). *Auto: none.*
- **F4.** A fresh install opens on Subscriptions; set Search as the start page, restart, and it opens on Search (DSC-04). *Auto: none.*
- **F5.** Type a query: suggestions arrive within NFR-PERF-09 in the phone's language and the keyboard stays up when results come in; where search history exists, clearing it empties it completely (DSC-11). *Auto: none.*
- **F6.** While a video plays, the watch page shows views, likes, date and subscribers; a timestamp in the description seeks within 1 s; share it, copy the link at the current time (it has `t=`), and open it in the browser; long-press a row of an unplayable video and its details open without stopping the current video (DSC-14). *Auto: tests/resolver_qt (details parser).*

## SponsorBlock

- **S1.** On a video with known segments, automatic skipping happens only while playing, and the manual skip button works (SB-02). *Auto: none.*

## Downloads

- **W1.** [smoke] Download audio from a feed row: progress shows in the row, on the downloads page and in a notification; kill the app during the transfer and relaunch: it resumes (DL-02/04). *Auto: tests/downloads_engine (audio end to end, kill and restart), tests/downloads_model, tests/downloads_domain.*
- **W2.** Download a 1080p video: the remuxed file plays in SailTube and in the system gallery, also in flight mode, and resume points work (DL-03/07). *Auto: tests/remux, tests/downloads_engine (video pairs), tests/downloads_domain (video picks).*
- **W3.** Choose a folder on the SD card, unmount the card and start a download: the notice appears (DL-06). *Auto: tests/downloads_engine (refused folder fallback), tests/downloads_domain (location).*
- **W4.** In flight mode, play a downloaded video with cached SponsorBlock segments and a downloaded vertical video: segments are skipped, the saved thumbnail and captions show, resume works, and the vertical video goes fullscreen in portrait (PLY-26, DL-07). *Auto: tests/downloads_engine (thumbnail and local media), tests/library_domain (resume decision).*
- **W5.** (FULL build.) Start a video download of 500 MB or more, put the app in the background with the screen off for 10 min: the transfer keeps moving the whole time (DL-09). *Auto: tests/downloads_engine, tests/downloads_model (background flag per profile).*

## Imports

- **N1.** Import a NewPipe backup whose subscriptions use @handles: the handles resolve online, and history, resume points and playlists match the backup (IMP-01). *Auto: tests/library_import, tests/library_store (import merge), tests/library_domain (handle URLs).*

## Timing and degradation

- **T1.** Export the diagnostics timing log covering P1, P2 and P3 and attach it to the release notes. *Auto: tests/diagnostics (timing log off by default, one line per phase when on).*
- **T2.** Block InnerTube with the debug toggle: search, feed and playback still work through the fallbacks, only slower, and the drift counters move only for shape errors (NFR-ROB-01, DIA-01). *Auto: tests/diagnostics (failure classes, drift-only hint), tests/sharedplayback (bot wall, breaker, shape failures), tests/resolver_qt (yt-dlp fallback parsing).*

## Beta 3 sitting: playback (run in this order)

One sitting on R2 with the FULL build, `diagnostics/timingLog` on and every sink muted first. B1–B4 block the
release: each checks a fix or a new safety net that the host can prove only in part. B5–B8 are measurements,
newly wired settings and leftovers; a failure there goes into the release notes. The ids are this file's own (not §6 steps).

- **B1.** On an H.264 video (the quality menu names avc1), seek 30 times while it plays: 10 drags of the seek bar, 5 bursts of double taps, the rest single double taps, and let one SponsorBlock skip happen; then change the speed twice. After every seek the picture and the sound continue and the position keeps moving; nothing freezes with the state still "playing" (PLY-03; the seek freeze of beta 1 and 2). *Auto: tests/player_gst (the race made deterministic, 400-seek stress on dual MP4 and WebM).*
- **B2.** Play a video of known length, lock the screen: the lock-screen controls show that length and a progress that moves at real speed; drag the lock-screen progress to about the middle and playback continues from there, not from the start; with a D-Bus tool `mpris:length` and `Position` read microseconds (a 829 s video: 829000000) and `Seek` with 10000000 moves 10 s (MPR-01). *Auto: tests/mpris (length, Position, Seek and SetPosition read on a session bus from the shipped MediaControls.qml).*
- **B3.** Open one video of each kind and note the time to the first frame: an old H.264-only upload (plays, through the full resolver), a running live stream (picture and sound, quality menu usable), an upcoming premiere (its start time, no error), a members-only, a private and a removed video (each its own message, none says "reinstall" or "no stream can be played on this device") (RES-08, RES-09, RES-19). *Auto: tests/fault_injection (34 resolve cases), tests/resolver_qt (failure reasons, pass arguments), tests/player_hls (fMP4 HLS end to end).*
- **B4.** Mid-video, turn flight mode on for 10 s and off again: playback continues by itself within a few seconds, no error; pause for 2 min and resume: no false "stopped responding"; turn flight mode on and leave it: after the recovery steps the error "Playback stopped responding." appears with Retry, and Retry plays once the network is back; walk from Wi-Fi to mobile data while playing: it continues (PLY-18, RES-12). *Auto: tests/player_stall (every guard, the steps, the budget), tests/session_qt (a server that stops mid-stream, a refused reopen, a stuck pipeline, the exhausted budget, no false stall when paused or in the background).*
- **B5.** Pre-buffer on or off: six launches that alternate `SAILTUBE_PREBUFFER=1` and `0` (order 1 0 0 1 1 0), each playing the same six videos from the feed in the same order on one network, with one early seek on two of them; save each launch's timing lines and run `tools/experiments/prebuffer_ab.py` over them (steps: `tools/experiments/prebuffer_ab.md`). Keep the pre-buffer only if tap-to-first-frame is faster with it by more than the spread between two launches of the same arm; note the redirect cases (NFR-PERF-02). *Auto: tests/perf_summary (the A/B summary on synthetic logs), tests/sharedplayback (pinned component), tests/shared_stream_source (adapter).*
- **B6.** On the old H.264-only upload of B3 (it resolves through yt-dlp), drag the seek bar: the bubble shows a small picture of the position under the finger, cut cleanly from its sheet, with the time; on an ordinary video the bubble shows the time and chapter only (expected until the fast path carries storyboards). *Auto: tests/resolver_qt (storyboard levels and the image allow-list), tests/session_qt (cell of a position).*
- **B8.** Settings that now act: set "Maximum quality on mobile data" to 720p and open a video on mobile data: the quality menu shows 720p as the automatic pick, and 480p again after setting it back; turn "Background playback" off, play and lock the screen: the sound stops and the video is paused after unlocking, while play on the lock screen still starts it; set "Volume boost" to 200 % while a quiet video plays: louder at once, no gap; turn "Loudness normalisation" on and compare a loud music video with a quiet talk: closer in level (SEL-02, PLY-22). *Auto: tests/session_qt (mobile ceiling, background pause, boost and loudness values in the audio chain, live change without a reload).*
- **B7.** Leftovers from earlier runs: play for 2 h and check that the app's temporary directory stays within its budget; turn hardware decoding on and repeat D3; play a downloaded video that was saved as one file with picture and sound: the picture shows (run 5 had sound only), and the `sailtube: decoders` line is in the log if it does not. *Auto: tests/tmpdir (budget and clean-up), tests/player_gst (local muxed files show pictures).*

## Shorts player (swipe pager; owner's hand test)

The pager opens each Short with `playback.openShort`, prepares the next two with `playback.shortsAhead` and
pauses with `playback.holdShort` / `releaseShort`. Timing log on:
`tap … kind=short ahead=1|0` to `first-frame` is the swipe.

- **K1.** Swipe through 20 Shorts of a feed on Wi-Fi: each one starts by itself, upright and filling the screen, and loops at its end; from the swipe settling to the picture is quick for a Short that was prepared (`ahead=1`: expect a few hundred ms at most) and about a second for one swiped to at once after the previous; no frame of the Short left behind stays on screen (DSC-17). *Auto: tests/session_qt (shorts_aheadPreparesTwo, shorts_openHoldAndBackground), tests/perf_summary (the host harness's swipe numbers in docs/PERF_PLAYBACK.md).*
- **K2.** Swipe back to the Short you just left: it plays again within about a second, from its start (DSC-17). *Auto: tests/session_qt (shorts_swipeSequenceStaysBounded).*
- **K3.** Leave the Shorts tab for another tab while a Short plays: the sound stops at once; come back: it plays again. Pause it with a tap, leave and come back: it stays paused. Open the comments: it pauses; close them: it resumes (not if you had paused it). *Auto: tests/session_qt (shorts_holdsAndRules, shorts_openHoldAndBackground).*
- **K4.** Lock the screen while a Short plays: nothing plays (the normal player keeps its sound on the lock screen, a Short does not); unlock: it plays on; a Short paused by a tap before the lock stays paused. The cover and the lock-screen controls show it paused while locked. *Auto: tests/session_qt (shorts_holdsAndRules).*
- **K5.** Swipe 50 Shorts in a row at your own pace, then check the app's memory (`grep -E 'Pss|Rss' /proc/$(pidof harbour-sailtube)/smaps_rollup` after the 10th and after the 50th): growth under about 30 MB in all, no thread count growth (`ls /proc/PID/task | wc -l`), the app still responsive. *Auto: tests/session_qt (shorts_swipeSequenceStaysBounded: resident size, live memory and threads bounded over 50 swipes on the host).*
- **K6.** Open a video from the Shorts section (a long press or a link) while a Short plays: the Short ends, the video starts, and no mini-player appears for the Short when you go back (a Short is never parked). *Auto: tests/session_qt (shorts_openHoldAndBackground).*

- **K7.** First rung for Shorts: with the timing log on, swipe through 10 Shorts with "Maximum quality on Wi-Fi" at 720p and 10 more at 360p (or on mobile data at 480p and 360p), alternating the two settings every 5 Shorts; compare `tap … kind=short ahead=0` to `first-frame` (the swipes nobody prepared) and `ahead=1` between the two: keep the Shorts ceiling at 720p unless 360p is faster by more than the spread (host: no reliable difference, estimate on a slow link 200-300 ms; `docs/PERF_PLAYBACK.md` "Shorts swipes on the host"). *Auto: tests/perf_summary (the summary of timing logs).*

## Slice steps in PLAYBACK.md

`docs/PLAYBACK.md` §6 ("Device verification for this slice") holds the earlier playback slice steps
V0–V11 for the POCO X3 Pro. They stay there and are not repeated here. They overlap with this
checklist as follows; when a slice step and a step above disagree, the step above is the release gate.

| Slice step | Checklist steps |
|---|---|
| V0 (install, probe line) | I1, D3 |
| V1 (video capabilities page) | D3 |
| V2 (search, load more) | F2 |
| V3 (first frame, stream label, lip-sync, CPU) | P1, P3 |
| V4 (seek, pause/resume) | P3 |
| V5 (quality menu) | P4 |
| V6 (adaptive streams off) | P7 |
| V7 (audio only, lock screen, MPRIS) | P14, M1 |
| V8 (display off/on) | D7 |
| V9 (live stream) | P6 |
| V10 (hardware decoding off) | D3 |
| V11 (leave and reopen from "Now playing") | P8 |

## Change area → checklist sections

Run the smoke subset plus every step listed for each area the change touches. A change to the
library schema or to settings storage also needs the upgrade pass (install over the previous release
with data kept).

| Area | Sections | Steps |
|---|---|---|
| Playback engine (player, controls, layout, background) | Playback, Device and platform, Media integration, Beta 3 sitting | P1, P3, P4, P5, P7, P8, P10, P11, P15, P16, P17, P18, D3, D7, M1, M5, B1, B4, B6, B7, B8 |
| Resolver / shared playback (fast path, media proxy, yt-dlp fallback) | Playback, Timing and degradation, Beta 3 sitting | P1, P2, P6, P12, P13, P14, P15, T1, T2, B3, B4, B5 |
| Stream selection (quality, audio language, captions, decoders) | Playback, Device and platform | P4, P9, P14, D3 |
| Catalog / search / channels / comments | Discovery | C1, F1, F2, F3, F5, F6, T2 |
| Library (subscriptions, history, playlists, resume) | Discovery, Media integration, Imports | F1, M5, W4, N1 |
| Imports and export | Imports, Account | N1, A2 |
| Downloads and remux | Downloads | W1, W2, W3, W4, W5 |
| Settings / UI shell (pages, navigation, start page) | Install and start, Discovery, Playback | I2, F4, P11, P18 |
| Links and URL routing | Links | L1, L2, L3 |
| Account and secrets | Account | A1, A2, A3 |
| Media integration (MPRIS, cover, headset, calls, audio role) | Media integration, Beta 3 sitting | M1, M2, M3, M4, M5, M6, B2 |
| Shorts pager (inline Short playback, preparing the next ones, holds) | Shorts player | K1, K2, K3, K4, K5, K6, K7 |
| Platform glue (display, rotation, power, helper processes) | Device and platform, Playback | D3, D7, D8, D9, P10 |
| SponsorBlock | SponsorBlock, Downloads | S1, W4 |
| Packaging / RPM (spec, desktop file, FULL and STORE profiles) | Install and start, Links, Account, Downloads | I1, I2, I4, L3, A2, D8, D9, W5 |
| Translations | Install and start, Discovery | I3, F5 |
| Privacy / network (hosts, secrets, extractor updates) | Install and start, Playback, Account, Timing and degradation | I4, P13, A1, A3, T2 |
