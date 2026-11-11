<!-- SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0 -->
# Independent development record

SailTube is implemented independently. Two GPL-3.0 Sailfish clients (harbour-fintune and harbour-fintube)
were analysed by a separate analyst who wrote behavioural specifications; the implementers
who write SailTube code do not open their sources, repositories, forks, the analyst's clones or the analyst's
working notes.

Sources used by the implementation:

| Source | Use |
|---|---|
| Analyst specifications `SAILTUBE_REQUIREMENTS.md`, `FINTUBE_SPEC.md`, `COMPARISON.md`, `INSTANT_PLAYBACK_SPEC.md` (internal notes, `docs/fin_analysis/`, 2026-11-08) | Requirements, numeric targets, public protocol names. These documents state they contain no code, pseudo-code or copied identifiers. |
| SailTune (same owner, PolyForm Noncommercial 1.0.0) at `e190ac2` | Shared resolver runtime (`Shared/`, see SHARING.md), patterns (process boundary, settings store, test style), build tooling. |
| Public documentation | Qt 5.6 (Multimedia, QML), Sailfish OS developer docs, GStreamer 1.x API and element documentation, ISO/IEC 23009-1 (DASH MPD), ISO/IEC 14496-12 (ISO BMFF boxes), yt-dlp README and output-template/format field documentation, MPRIS 2 specification, Amber.Mpris type information from the SDK. |
| yt-dlp source (Unlicense) | Read for format field behaviour (`formats=dashy` fragments, chunk size). Not a GPL source. |
| The SFOS 5.1.0.11 SDK target | Package and element inventory (PLAYBACK.md). |

Provider observations made during implementation: one flat search run and one blocked extraction from the
build host (2026-11-08); outputs were used only to confirm field names. The core test fixtures in
`tests/fixtures/*.json(l)` are synthetic, written in the documented field shape, and contain no recorded
provider data. The catalog fixtures in `tests/fixtures/catalog/` are recorded provider data (see the catalog
section below).

## Catalog component

The catalog component (discovery, links, caption cues, SponsorBlock fetch, Return YouTube Dislike) is written
without opening FinTube, FinTune, NewPipe, any other GPL YouTube client, `references-gpl/` or the analyst's
working notes.

Sources:

| Source | Use |
|---|---|
| `SAILTUBE_REQUIREMENTS.md` (DRAFT v0.1, 2026-11-08) §2, §3.2 SEL-06, §3.4 DSC, §3.8 SB/RYD, §3.11 LNK, §3.14 I18N, §4, §6, §7 E10 | Requirements, numbers, endpoint names and public JSON field names as quoted by the spec |
| `FINTUBE_SPEC.md` §3.1–3.5, §5 (behavioural spec, no code) | Background behaviour and lessons; numbers cross-checked against the requirements |
| Public answers of YouTube's web endpoints, recorded 2026-11-08 by `tools/record_fixtures.py` (InnerTube WEB `search`, `browse`, `next`, `navigation/resolve_url`, `player` with the WEB and VISIONOS client contexts named in the spec, `visitor_id`; `/feeds/videos.xml`; `api/timedtext` json3; `suggestqueries.google.com`; `sponsor.ajay.app` hash-prefix API; `returnyoutubedislikeapi.com`) | Field names and shapes for the parsers; offline test fixtures. Scrubbed before saving (IPs, cache nodes, visitor-id country, location shelves, tracking blobs); see the tool's docstring and `tests/catalog_fixture_scrub`. |
| The pinned shared player request (`Shared/SailTune/Core/SharedPlayback/Domain/InnerTubeClient.h`, whose own header records its source: the public client table of yt-dlp, Unlicense) and the public answers of `youtubei/v1/player` and `youtubei/v1/visitor_id`, observed and recorded 2026-11-10 (`player_refused`, `visitor_id`; live observations: `LOGIN_REQUIRED` without a visitor id, HTTP 429 for `tlang=` on `api/timedtext`) | The caption track list request of a streamed video (SEL-06): the request reuses the shared definition with the device language; nothing was taken from another client's source |
| Public answers of `youtubei/v1/reel/reel_item_watch` and `youtubei/v1/reel/reel_watch_sequence` (anonymous WEB client), observed and recorded 2026-11-11 through `tools/record_fixtures.py` (`shorts_item_*`, `shorts_sequence_en_*`) | Field names and shapes of a Short's on-picture data (counts, channel, description, related chip, sound line) and of the Shorts sequence; the request body of the item endpoint was found by trying shapes against the live service and keeping the smallest one it accepts. Nothing was taken from any other client's source |
| Homepage HTML of www.youtube.com with `SOCS=CAI` (2026-11-08) | Only to confirm the `INNERTUBE_CLIENT_VERSION` key the spec names (DSC-02); not stored |
| Public documentation | Qt 5.6 (QtNetwork, QtCore JSON/XML, QtDBus, QtQml), Sailfish OS developer docs (Silica, URL handlers, Sailjail), freedesktop D-Bus activation, Protocol Buffers wire format (for the `sp` filter encoder and the visitor-id scrub), RFC 4648 base64url, FIPS 180-4 SHA-256, SponsorBlock and Return YouTube Dislike public API docs |

Unlike the core slice, the catalog fixtures in `tests/fixtures/catalog/` **are recorded provider data**
(scrubbed public answers, as NFR-TST-01 requires). The fixtures in `tests/fixtures/*.json(l)` from the core
component remain synthetic.

Behavioural questions to the analyst: none so far.

This record states intent and method. It does not certify a clean-room process or replace a legal review.
SailTune's own record (its `docs/CLEAN_ROOM.md`) lists an earlier limitation of that project which applies to
the shared files to the same extent.

## Component sections

Status: v0, 2026-11-08. Each component has its own section below.

## Purpose

SailTube is written without reading GPL YouTube-client source, so its code is the project's own and
can carry the PolyForm Noncommercial licence. This file records which sources each component used, so the
provenance of any design can be shown later.

## Allowed sources

* The clean-room specifications in `sailtune-internal-notes-e5eqvydg/docs/fin_analysis/`:
  `SAILTUBE_REQUIREMENTS.md` (the requirement source), `FINTUBE_SPEC.md`, `INSTANT_PLAYBACK_SPEC.md`,
  `COMPARISON.md`. They describe behaviour, public protocols, platform techniques and numbers.
* Answers from the analyst about behaviour, numbers or protocol shapes (never code,
  identifiers or file names), recorded below with date and question.
* Public documentation: GStreamer, Qt, Sailfish OS and its public component READMEs/headers (API
  usage only), PulseAudio, freedesktop specifications, yt-dlp's Unlicense documentation.
* Code already pinned in `Shared/` through `Shared/SHARED_MANIFEST.json` (SailTune, same owner).

## Forbidden sources

* `references-gpl/` and anything in it.
* The analyst's "findings" files.
* FinTube or FinTune source code, and the source of any GPL YouTube client.
* Web searches for other YouTube clients' code.

## Playback component

### Sources used by the playback component

| Source | Used for |
|---|---|
| `SAILTUBE_REQUIREMENTS.md` v1.0 §1.3–1.5, §2 (PLT-03, PLT-04, PLT-07, PLT-08, PLT-11, PLT-12) | device targets, Sailjail layout, decoder quirks, audio role/resource |
| `SAILTUBE_REQUIREMENTS.md` §3.1 (RES-10, RES-11, RES-12, RES-13, RES-14, RES-16, RES-17, RES-18) | resolve cache, prefetch, PO helper, fetcher, loopback hand-off, spool |
| `SAILTUBE_REQUIREMENTS.md` §3.2–3.3 (SEL-02, SEL-04, PLY-03..PLY-09, PLY-20..PLY-24) | ladders, seeking, preroll, background freeze, hardware decode, audio chain and policy |
| `SAILTUBE_REQUIREMENTS.md` §3.13 (DIA-02, DIA-03), §4.1 (NFR-PERF-02..06), §6 (D3, D7, M3, M4, P1–P4, P7, T1), §7 (E2, E3, E4, E7, E8, E12) | experiment design, timing markers, budgets |
| GStreamer element docs: [uridecodebin](https://gstreamer.freedesktop.org/documentation/playback/uridecodebin.html), [souphttpsrc](https://gstreamer.freedesktop.org/documentation/soup/souphttpsrc.html), [appsrc](https://gstreamer.freedesktop.org/documentation/app/appsrc.html), [appsink](https://gstreamer.freedesktop.org/documentation/app/appsink.html), [scaletempo](https://gstreamer.freedesktop.org/documentation/audiofx/scaletempo.html), [equalizer-10bands](https://gstreamer.freedesktop.org/documentation/equalizer/equalizer-10bands.html), [pulsesink](https://gstreamer.freedesktop.org/documentation/pulseaudio/pulsesink.html) (`volume`, `stream-properties`), [qtdemux](https://gstreamer.freedesktop.org/documentation/isomp4/qtdemux.html), [matroskademux](https://gstreamer.freedesktop.org/documentation/matroska/matroskademux.html), [queue](https://gstreamer.freedesktop.org/documentation/coreelements/queue.html), [fakesink](https://gstreamer.freedesktop.org/documentation/coreelements/fakesink.html) (`num-buffers`, `silent`, `signal-handoffs`) | pipelines in the engine and the E3/E7 probes |
| GStreamer [gst-launch-1.0](https://gstreamer.freedesktop.org/documentation/tools/gst-launch.html) and the [seeking design doc](https://gstreamer.freedesktop.org/documentation/additional/design/seeking.html) | seek flags (FLUSH, KEY_UNIT, SNAP_BEFORE, ACCURATE), probe pipelines |
| gst-droid public README: <https://github.com/sailfishos/gst-droid> (names `droideglsink`, `droidcamsrc` and the "droidcodec" decoders/encoders; the `droidvdec` element name comes from the spec, PLT-08/PLY-20) | hardware decode chain, E3 probe; `/etc/gst-droid` is known only from the Sailjail Base profile (`private-etc gst-droid`), so E3 copies it if present |
| Nemo.KeepAlive: <https://github.com/sailfishos/nemo-keepalive>, <https://sailfishos.org/develop/docs/nemo-keepalive/> | `DisplayBlanking`, background jobs (PLT-11) |
| Amber.Mpris: <https://github.com/sailfishos/amber-mpris>, <https://sailfishos.org/develop/docs/amber-mpris/>; MPRIS spec <https://specifications.freedesktop.org/mpris-spec/latest/> | MPRIS player (PLT-10), `Player.Pause` used by the experiment teardown |
| libaudioresource: README and public header `include/audioresource.h` at <https://github.com/sailfishos/libaudioresource> (API only: `audioresource_init/acquire/release/free`, `AUDIO_RESOURCE_GAME` = 1, `AUDIO_RESOURCE_MEDIA` = 2, GLib main loop required) | PLY-23, E8 resource helper |
| Sailjail: README <https://github.com/sailfishos/sailjail> (`sailjail -p <name>.desktop -- <exe>`, profile dirs); permissions <https://github.com/sailfishos/sailjail-permissions> (Base: `protocol unix`, `net none`; Internet: `ignore net none`, `protocol inet,inet6,netlink`) | E2 method and hypothesis, PLT-03/PLT-04 |
| PulseAudio stream properties: <https://www.freedesktop.org/wiki/Software/PulseAudio/Documentation/Developer/Clients/ApplicationProperties/>; `pactl(1)` | `media.role`, mute rule commands |
| Linux `proc(5)` (`/proc/<pid>/smaps_rollup`, `/proc/<pid>/stat`, `/proc/uptime`) | PSS and CPU samplers |

### Analyst answers used by the playback component

(none yet)


## Library component (library, imports, downloads)

| Area | Sources |
|---|---|
| LIB-01..07 subscriptions, playlists, history, resume, watched | `SAILTUBE_REQUIREMENTS.md` §3.5; `FINTUBE_SPEC.md` §5.8–5.10 (behavioural description only) |
| SQLite storage, schema versioning, WAL | SQLite documentation (PRAGMA user_version, journal_mode=WAL, busy_timeout); Qt 5.6 QtSql documentation (QSQLITE driver, QSQLITE_OPEN_READONLY) |
| IMP-01 NewPipe/PipePipe backup | `SAILTUBE_REQUIREMENTS.md` IMP-01 and `FINTUBE_SPEC.md` §5.13, which name the tables and columns of NewPipe's public backup format; fixture databases are written from that description, not from NewPipe code |
| ZIP reading | PKWARE APPNOTE (public ZIP format description); zlib manual (raw inflate) |
| IMP-02 account import | `SAILTUBE_REQUIREMENTS.md` IMP-02 (browse ids, renderer names, page limits, identity headers); fixtures are synthetic JSON in that shape, no recorded account data |
| IMP-03 Takeout / OPML | Google Takeout's public `subscriptions.csv` layout (Channel Id, Channel Url, Channel Title); RFC 4180; OPML 2.0 specification |
| IMP-04 SailTube backup | own format (JSON), Qt QSaveFile documentation |
| DL-01..09 downloads | `SAILTUBE_REQUIREMENTS.md` §3.9, PLT-03, PLT-09, NFR-SEC-05; `FINTUBE_SPEC.md` §6.12–6.13; RFC 7233 (HTTP Range); Qt QNetworkAccessManager / QNetworkConfigurationManager documentation |
| DL-03 remux without ffmpeg (E6 host side) | GStreamer 1.x public API and plugin documentation (qtdemux, mp4mux/qtmux, matroskademux, webmmux, matroskamux) |
| DL-07 offline captions and segments | `SAILTUBE_REQUIREMENTS.md` DL-07, SEL-06 (json3 tracks), SB-01/SB-02 (segment fields, "segments are cached with downloads"); Qt QSaveFile/QJsonDocument documentation |
| DL-09 KeepAlive | Sailfish `Nemo.KeepAlive` public documentation |

## ux component (2026-11-08)

* `sailtune-internal-notes-e5eqvydg/docs/fin_analysis/SAILTUBE_REQUIREMENTS.md`: PLT-05, PLT-06,
  PLY-11, PLY-13 to PLY-17, SET-01 to SET-03, DSC-04, I18N-01 to I18N-03, NFR-PERF-01/07,
  NFR-TST-04, §6 steps P8, P10, P11, I2, I3, T1.
* `FINTUBE_SPEC.md` §6.1 to §6.10 and §6.14 (behavioural description of layouts, fullscreen,
  overlay timing, shell, settings inventory). Behaviour only; no code or identifiers.
* Sailfish Silica public API as shipped in the SDK target (`sailfishsilica-qt5` 1.2.154 in
  `sfos-m1-sailtube`): property and enum names read from `plugins.qmltypes` and the public QML
  component declarations (Page `showNavigationIndicator`, `cutoutMode`, `CutoutMode`,
  `Screen.hasCutouts/topCutout`, ApplicationWindow `bottomMargin`/`deviceOrientation`,
  DockedPanel, Orientation enum values).
* Qt 5.6 documentation: QJSEngine, QTranslator (layout direction from `QT_LAYOUT_DIRECTION`),
  LayoutMirroring, QQuickWindow::frameSwapped, Qt Linguist file format.
* App icon: the original SailTube artwork already in `icons/` (sailboat with a play-shaped sail),
  not derived from any YouTube logo.
* Translator comments (2026-11-10): which `//:` comments Qt 5.6's lupdate keeps was found by
  running the SDK's own lupdate on a probe file written for the purpose (a comment directly
  after a `)` is dropped); Qt Linguist manual, "Writing source code for translation".
* Channel tab row (2026-11-10): SilicaFlickable and Row from the Silica and Qt Quick public
  documentation, after the app's own `components/ChipStrip.qml`; that a channel's tabs scroll
  sideways is the official app's visible behaviour. Qt 5.6 `Accessible.PageTab` from the
  Accessible QML type's documentation.
