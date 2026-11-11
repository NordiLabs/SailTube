# Architecture

SailTube is a native Sailfish OS client for YouTube videos: Qt 5.6, C++11 and QML with Sailfish Silica, built
for the SFOS 5.1 aarch64 target (installs on 5.1 and 5.2). It is the sibling of SailTune (the YouTube Music
client) and shares its resolver runtime (see [SHARING.md](SHARING.md)). The requirements baseline is the
analyst's `SAILTUBE_REQUIREMENTS.md` v1.0; requirement IDs below (`RES-03`, `PLY-01`, …) refer to it.

**Status 2026-11-11.** §1 to §4 and the "Now" column of §5 describe the phase-1 vertical slice (Qt Multimedia,
one yt-dlp child per call); they are kept as the record of the seams. What ships in FULL today: the app's own
GStreamer player (`Core/Player`: two `uridecodebin` branches on one clock, per-sink seeks, hardware render path,
stall watchdog, MPRIS, live cover video); SailTune's shared fast-path resolver and loopback `MediaProxy`
(pinned, `Shared/SailTune/`) with the yt-dlp warm worker as the fallback (`Core/Worker`); a native InnerTube
catalog (`Core/Catalog`); the SQLite library and download engine (`Core/Library`, `Core/Downloads`); the account
rows (`Core/Account`); the time-limited beta profile (`Core/Beta`). The STORE build has no video playback (E9).
Open items: `docs/CHECKLIST.md` and `docs/ROADMAP.md`.

## 1. Layers

```
Clients/Sailfish/qml           Silica pages, cover, MPRIS (Amber.Mpris), display keep-on
Clients/Sailfish/Presentation  QObject controllers for QML: SearchModel, PlaybackSession, AppSettings
Clients/Sailfish/Composition   main.cpp: child-mode dispatch, platform probe, wiring
Core/<Context>/Domain          Qt-free policies and records (host-tested with a plain C++ compiler)
Core/<Context>/Infrastructure  Qt adapters: process runner, parsers, network, GStreamer probe
Shared/                        pinned SailTune snapshot: resolver child, QuickJS child, Python bridge
```

The rule from SailTune carries over: decisions live in pure, testable domain code (`NFR-TST-04`), adapters
only translate. Domain code never sees Qt types; presentation never sees provider JSON.

| Context | Domain | Infrastructure |
|---|---|---|
| Catalog | `Video.h` (summaries, details, chapters, id validation) | — (today via the resolver service) |
| Media | `MediaFormat.h`, `StreamSelection` (codec/height/fps policy, fallback chain, quality menu), `DashManifest` (MP4 `sidx` index reader, on-demand MPD builder) | `DashManifestPreparer` (two range reads, private manifest file) |
| Resolver | — | `YtDlpRunner` (bounded process boundary), `YtDlpJson` (parsers, URL allow-list, error classes), `YtDlpVideoService` (search, extraction profiles) |
| Platform | — | `GstCapabilityProbe` (dlopen of libgstreamer: decoders, demuxers, droid caps, playbin/playbin3 switch, droidvdec demotion) |

## 2. Process model (phase 1)

- **One GUI process** (`harbour-sailtube`). The same binary has two **child modes**, dispatched in `main()`
  before any GUI, media or settings object exists (shared from SailTune):
  - `SAILTUNE_RESOLVER_CHILD=1`: a `QCoreApplication` that loads PyOtherSide and runs the packaged upstream
    yt-dlp zipimport archive (`/usr/share/harbour-sailtube/runtime/yt-dlp.pyz`) with the given arguments.
  - `SAILTUNE_QUICKJS_CHILD=1`: runs the private QuickJS-NG library (`lib/libsailtune-quickjs.so.1`) as the
    JavaScript runtime that yt-dlp spawns for the player script in the full profile.
- `YtDlpRunner` starts children in their own process group, with a private 0700 temporary directory, a
  sanitised environment, stdout/stderr caps, a deadline and group kill on cancel/timeout. At most two run at
  once. The environment variable names are the shared runtime's protocol names and stay `SAILTUNE_*`.
- **Search** is a flat yt-dlp listing (`ytsearchN:` with `--flat-playlist --playlist-items`), one InnerTube
  search request inside yt-dlp, no per-video extraction. Measured in the build container: 5 results in 2.3 s
  wall including Python start-up (host CPU; the phone will be slower).
- **Video details and formats** come from one full yt-dlp extraction printed as one JSON object. The first
  attempt uses the **fast profile** (no JavaScript runtime: yt-dlp picks its JS-less client); a failure that is
  not final, or an answer with no playable stream, repeats once with the **full profile** (QuickJS).

This per-call process model is the phase-1 shortcut. It does not meet `RES-01` (warm engine, no spawn on the
fast path); see §5.

## 3. Playback path (phase 1, Qt Multimedia)

```
tap ─► PlaybackSession.open(id) ─► details (fast → full) ─► selectStreams() ─► attempt chain
         │                                                     │
         │  Adaptive:    DashManifestPreparer: 2×Range read of moov+sidx ─► private .mpd ─► QMediaPlayer
         │  Progressive: QNetworkRequest(url + format headers) ───────────────────────────► QMediaPlayer
         │  Live:        HLS variant or master manifest ─────────────────────────────────► QMediaPlayer
         └► VideoOutput { source: playback }   (Sailfish declarative video backend, droideglsink when droid)
```

- **Engine:** Qt Multimedia 5.6 `QMediaPlayer` with the Sailfish GStreamer backend (`playbin`). It plays one
  URI. Separate audio and video streams are joined into one URI with a generated **on-demand DASH manifest**
  whose byte ranges come from each file's own `sidx` index. The pipeline owns the clock, so A/V sync is
  GStreamer's, not ours. Only the MP4 family (H.264 + AAC) is offered this way because the index reader handles
  ISO BMFF; WebM (VP9/Opus) adaptive is future work.
- **playbin3 on demand:** GStreamer 1.26 on SFOS 5.x ships only the new `dashdemux2`, written for playbin3,
  while Qt 5.6 asks for `playbin`. For an adaptive attempt the session re-registers playbin3's type under the
  factory name `playbin` and recreates its `QMediaPlayer` (Qt builds the pipeline in the player's
  constructor); every other attempt uses the stock playbin. `VideoOutput` follows the `mediaObject`
  notification. This is experiment **E13** (new, see [PLAYBACK.md](PLAYBACK.md)); `Settings > Adaptive
  streams` turns it off and progressive (360p) remains.
- **Selection** (`StreamSelection`, pure): decodable codecs from the probe (H.264/VP9/HEVC/AV1, hardware vs
  software), height cap (default 1080), lower frame rate at equal height unless the user picks a `p60` entry,
  software decode capped at 720p, HDR excluded, DRC and dubbed audio ranked below the original. The result is an
  **ordered chain**: adaptive → progressive (video mode), AAC → Opus → muxed (audio-only), variant → master
  (live).
- **Recovery:** each attempt has a 25 s start watchdog. An early failure after a full-profile extraction is
  retried once after 4 s (the provider holds some URLs back right after extraction; SailTune STREAMING.md field
  finding). Then the next chain entry, then one full re-extraction, then a visible error with **Retry**
  (`PLY-18`). Quality and audio-only switches keep the position.
- **Session:** one global `PlaybackSession` (one `QMediaPlayer`). Leaving the video page keeps playing;
  reopening the same video re-attaches without a resolve (`PLY-10`). MPRIS via Amber.Mpris; display kept on
  only while a video plays in the foreground (`PLY-12`).

## 4. Platform rules applied in the slice

`PLT-03` Sailjail layout (`org.nordilabs.sailtube/SailTube`, permissions `Internet;Audio` only);
`PLT-05` ES5-only QML/JS (gate in `tests/qml`); `PLT-06` ≤5 pulley items (gate); `PLT-07` no persistent GL
context/scene graph, repaint 250 ms after wake; `PLT-08` hardware decoding setting (off demotes `droidvdec` to
rank none); `PLT-09` `TMPDIR` on flash before GLib/GStreamer; `PLT-10` Amber.Mpris; `PLT-12` partially (Qt
Multimedia joins the Sailfish resource policy through its `resourceqt` plugin; the explicit `media.role` is
open, E8); `RES-22` stream URL allow-list.

## 5. Target design (MVP, per the requirements draft)

The slice was shaped so that each piece is replaced behind the same seams, not rewritten (the "Now" column is the
phase-1 state; the FULL build ships the "Target" column except where a row says otherwise):

| Concern | Now | Target | Requirement |
|---|---|---|---|
| Resolve fast path | yt-dlp fast profile per call | Native C++ InnerTube `/player` request with the JS-less client (definitions read from the bundled yt-dlp client table), visitor id minted at launch, pooled TLS, probe-before-play, breaker | RES-03…07, RES-21 |
| Fallback extractor | one-shot child per call | **Warm resolver child**: the same shared child, started once (5 s after the first frame), kept alive, line-delimited JSON requests over stdin/stdout, yt-dlp imported once, player-JS cache in the app cache | RES-01, RES-08 |
| Catalog | yt-dlp flat search | Native InnerTube WEB client for home/search/channel/related/comments (SailTune's anonymous adapter pattern), RSS-first subscriptions feed | DSC-* |
| Media fetch | GStreamer `souphttpsrc` direct | Decided by **E5**: keep souphttpsrc if it gets 206 reliably, else an app-owned fetcher (10 MiB Range chunks, mid-stream self-heal, disk read-ahead) behind an in-process source element or a token-guarded loopback server | RES-12, RES-16…18 |
| Player engine | `QMediaPlayer` (single URI) + DASH manifest | `PlaybackEngine` port with two adapters: **QtMultimediaEngine** (today's, also the STORE fallback) and **NativeGstEngine** (FULL): one pipeline, two `uridecodebin` branches sharing the audio clock, per-sink seeks, preroll barrier, freeze in background, audio chain (scaletempo, EQ, boost, limiter), software `appsink` renderer and optional explicit `droidvdec → droideglsink` | PLY-01…09, PLY-19…22 |
| Caches | none | resolve cache keyed by every output-affecting setting, prefetch of the next item | RES-10, RES-11 |
| Library | none | local subscriptions, history, resume, playlists (SQLite, atomic writes); account later via SailTune's hybrid cookie approach | LIB-*, ACC-* |

The engine port is the main seam: `PlaybackSession` keeps the attempt chain and UI state; an engine receives
"play this candidate at position p" and reports first frame, position, errors and stalls. The DASH-manifest
path stays useful even after the native engine exists: it is the only adaptive path available to a STORE build
that cannot link GStreamer (`E9`).

## 6. Decisions (short ADR log)

| # | Decision | Status |
|---|---|---|
| D1 | Share SailTune's resolver runtime as a pinned, hash-checked snapshot under `Shared/` now; extract a common library later. See [SHARING.md](SHARING.md). | accepted |
| D2 | Phase-1 playback on Qt Multimedia; adaptive formats via a generated on-demand DASH manifest played by playbin3 only for those attempts; native GStreamer engine is the MVP target behind an engine port. | superseded in FULL by the native GstEngine; the Qt Multimedia path remains in the tree for a reduced STORE player |
| D3 | Probe GStreamer with `dlopen` (no link dependency); conservative H.264/AAC assumptions when the probe fails. | accepted |
| D4 | Selection is a pure ordered chain; the session walks it; failures never loop (one delayed retry, one re-extraction). | accepted |
| D5 | Default ceilings: 1080p, low frame rate unless explicit, software decode ≤ 720p, AV1 only with a decoder. | accepted (SEL-01/02) |
| D6 | FULL variant first (Chum/OpenRepos, yt-dlp bundled); STORE later only if E2/E9 allow. | accepted (§1.5 of the requirements) |
| D7 | SFOS 5.1 is the floor (system Python 3.11, glibc 2.38 symbols from the 5.1 target). | accepted (PLT-01) |

## 7. Privacy and safety boundaries

No telemetry. Provider text never reaches the UI or logs: failures are classified into fixed codes
(`resolver::ErrorCode`) and translated. Stream URLs and headers are ephemeral (never persisted; the DASH
manifest that contains them lives in the private cache only for the current attempt and is deleted on the next
attempt, on stop and at start). Thumbnails use a bounded private disk cache. The resolver children run with no
config files, cookies, plugins or automatic downloads.

Source code describes intended behaviour, not device verification. See [PLAYBACK.md](PLAYBACK.md) §6 for what
the phone must confirm.

## 8. Contract: opening a video (`playback.open` hints)

`PlaybackSession::open(videoId, hints)` takes the row map of whatever list the user tapped (search, feed,
channel, related, history, playlist). All rows use the same keys. Hints only fill in what is already
known: the details answer wins as soon as it arrives, and a missing or wrongly typed key means "not known".
Hints never pick a stream and are never written to a store. Core owns this table; a component that adds a key
adds a row here in the same commit. Core wires none of this; the "applied by" column says who does.

| Key | Type | Unit / values | Set by | Applied by | Status |
|---|---|---|---|---|---|
| `videoId` | string | 11 chars `[A-Za-z0-9_-]` | every row model | ignored in hints (the first argument wins) | — |
| `title` | string | | search, catalog, library rows | playback: display until details arrive | done |
| `channel` | string | display name | same | playback: display | done |
| `channelId` | string | `UC…` | same | playback: display / channel link | done |
| `thumbnail` | string | https URL (i.ytimg.com) | same | playback: display; a hint wins over the details URL (no refetch) | done |
| `durationSeconds` | number | seconds, -1 unknown | same | playback: slider range before the player reports one | done |
| `viewCount` | number | count, -1 unknown | search, catalog | playback: display | done |
| `live` | bool | | search, catalog | playback: live layout before details (no seek bar) | done |
| `position` | number | **milliseconds** despite the name; history rows (`HistoryModel` role) | library | **not a seek request.** Resume is applied once, by `LibraryPlaybackBridge` through `setStartPosition(ms)` after `decideResume` (honours the resume setting, the LIB-05 rules and the "ask" offer). Playback must not seek from this hint as well. | done (bridge); playback keeps `setStartPosition` effective until the first frame, PLY-06 preroll barrier in batch 2 |
| `positionMs` | number | milliseconds | reserved alias of `position`; no producer yet | as `position` | — |
| `audioOnly` | bool | true = start in audio-only mode for this video | reserved: a "listen" action in a row menu (ux/library); absent = the Settings default (`AppSettings.audioOnly`) | playback: set before the first resolve, so only one resolve runs (SEL-08) | done (playback) |
| `startSeconds` | number | **seconds**, -1 none; a link's `t=` / `start=` / `#t=` (LNK-02) | catalog: `linkRouter` opens (shell QML) | informational. The start is applied once, through `libraryPlayback.setLinkStart(id, ms)` before `open()`, then `setStartPosition`; it wins over the resume point. Playback must not seek from this hint. | done (catalog + library) |
| `playlistId` | string | YouTube list id (`PL…`, `OL…`, `UU…`); empty = none | catalog: links with `list=`, playlist preview rows (`PlaylistModel`) | playback: queue the list starting at `index` (LNK-04) | done (`QueueSeeder`, `playback.setQueue`) |
| `index` | number | 0-based position of the video in `playlistId`; -1 unknown | catalog: same as `playlistId` | playback: queue start | done |
| `preferStream` | bool | default false; true = play the stream even when a download of the video exists | design/ux: the "Stream instead" action | playback: `open()` plays a finished download locally by default (PLY-26) and skips it when this is true | done (playback) |
| `loop` | bool | default false; true = restart at the end (no up-next, no `ended`, no autoplay) | design: the Shorts pager | playback: loops the item instead of ending it | done (playback) |
| `short` | bool | default false; true = a Short of the pager (what `playback.openShort` sets with `loop`): never parked (no mini-player), stores no resume point (history kept), held (paused) while the app is in the background; a non-bool keeps the playing item's mark on a re-attach | design: the Shorts pager | playback: `PlaybackController::setShortItem`, holds (`holdShort` / `releaseShort`) | done (playback) |

Local media (downloads) do not go through `open()`. `DownloadsModel.localMedia(videoId)` returns a record,
and `playback.openLocal(record)` plays it with the same player (PLY-26). Record keys (library's
`DownloadManager::localMedia`):

| Key | Type | Meaning |
|---|---|---|
| `available` | bool | false = nothing playable (then `error` is a fixed code, e.g. file missing) |
| `videoId` | string | the video, always set (history and resume of local playback, PLY-26) |
| `id` | int | download id |
| `kind` | string | `video` or `audio` |
| `layout` | string | `pair` = separate video and audio files (`videoPath` + `audioPath`); otherwise one file in `filePath` |
| `filePath`, `videoPath`, `audioPath` | string | absolute local paths in the app's download directory; empty when not applicable |
| `thumbnailPath` | string | saved thumbnail, empty if none |
| `title`, `channel`, `channelId`, `durationSeconds` | string, number (s) | display, as in the hints above |
| `videoCodec`, `audioCodec` | string | RFC 6381 strings of the saved tracks, for decoder selection |
| `error` | string | fixed error code, empty when `available` |
| `captions` | list | DL-07: `[{languageCode, name, autoGenerated, path}]`, `path` = saved json3 body for the SEL-06 parser (`CaptionService::parseJson3`); empty when none or the setting is off |
| `segments` | list | DL-07/SB-02: `[{category, actionType, start, end}]` in seconds, end > start (a `poi_highlight` is a point), sorted; empty when none or off |
| `sidecarsPending` | bool | captions or segments are wanted but not saved yet (fetched again when online) |

Rules for `openLocal` (playback, batch 2): it accepts only paths inside the download directory, it makes no
network request (DL-07, flight mode), and it resumes from the same library store as streams (PLY-26). The
record carries `videoId` (library, batch 2), so `openLocal(record)` needs no second argument.
