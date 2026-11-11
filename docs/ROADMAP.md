# Roadmap

Phases follow the requirements §8 (Phase 0 → MVP → v1 → Parity), FULL variant (Chum/OpenRepos,
aarch64) first. "Req" are IDs from `SAILTUBE_REQUIREMENTS.md` v1.0. Status as of 2026-11-11: **done** (in code and
host-tested; device checks are tracked in `docs/DEVICE_CHECKLIST.md` and `docs/DEVICE_DESIGN.md`), **partial**,
**later**. The per-row state is `docs/CHECKLIST.md`; the open items are the rows not yet done in this file.

The current build is the public beta `0.1.0-0.beta3` (`docs/BETA.md`).

## Phase 0 — experiments

| ID | Question | State |
|---|---|---|
| E3 | `droidvdec` per device | R2 answered; R1 open |
| E9 | Harbour validator on a GStreamer-linking build | answered: STORE cannot link GStreamer; FULL ships the player, STORE plays through Qt Multimedia later |
| E5, E2, E8 | HTTP stack for googlevideo, loopback under Sailjail, audio role and resource | device runs open (`docs/CHECKLIST.md`) |
| E13 | DASH manifest + playbin3 under Qt Multimedia 5.6 | only matters for the reduced STORE player |

Gate (requirements §8): fast-path first frame under 2 s on R1. R2 measured 1.07–1.71 s tap to first frame; R1 has not been
available.

## MVP — anonymous, FULL, aarch64

Feature-to-requirement map (the user's feature list first):

| Feature | Req | Status |
|---|---|---|
| Fast start (warm resolver, InnerTube `/player` fast path through SailTune's shared component, probe, breaker, cache, prefetch) | RES-01…13, RES-15…22 | done |
| Home (signed-out Popular rows under a Continue watching shelf) | DSC-01…04 | done |
| Search (filters, chips, suggestions, infinite scroll, cache) | DSC-08…11 | done |
| Channel pages | DSC-12 | done (Videos/Shorts/Live/Playlists, About card; banner, Posts and Home tabs left out) |
| Video page with description | DSC-14 | done |
| Playback with quality selection, adaptive streams | SEL-01…07, PLY-01…10, PLY-12, PLY-13, PLY-15…18, PLY-21…24 | done (own GStreamer player; device checks open) |
| Audio-only / background | SEL-08, PLY-09 | done |
| MPRIS, cover (live video on the active cover) | MPR-01…03, PLT-10 | done |
| Subscriptions (local first) | LIB-01, DSC-05…06 | done |
| History, resume, watched marks | LIB-04…06 | done |
| Link handling | LNK-01…04, PLT-13 | done in FULL; cold start (L1) needs the device |
| NewPipe/PipePipe import | IMP-01 | done |
| Settings inventory, diagnostics | SET-01…03, DIA-01…02 | settings done; the report builder exists in Core but no page shows it |
| i18n | I18N-01…03 | done (10 locales) |

Exit: requirements §6 checklist except A, C, S, W, N on R1 and R2.

## v1 — feature-complete core

| Feature | Req | State |
|---|---|---|
| Playlists (local, then remote read) | LIB-02, LIB-03 | done |
| Account (hybrid, as in SailTune's YouTube Music work) | ACC-01…04, ACC-06…10, IMP-02 | done (read-only); ACC-10 vault adapter not pinned, so STORE refuses to store a session |
| Downloads | DL-01…07 | done (remux without ffmpeg; offline playback through the same player) |
| Comments (read-only) | DSC-16 | done |
| Captions | SEL-06 | done for the video's own tracks; auto-translated tracks are not offered in beta 3; caption style not built |
| Chapters | DSC-15, PLY-17 | done |
| SponsorBlock (optional) | SB-01, SB-02 | done |
| Mini-player and cover with live frame | PLY-11, MPR-02 | done |
| Sleep timer | — | done |
| Autoplay / queue, local file playback | PLY-25, PLY-26 | done; no queue page |
| Extractor updates without a release | NFR-MNT-01 | built (FULL only); no settings control yet |
| Optional PO-token provider | RES-14 | code and tests; no reviewed pin, so it refuses to start |

## Parity and later

Hardware decode defaults per device from E3 (PLY-20), WebView sign-in if E1 passes (ACC-05), account writes, Shorts stage 2
(inline playback, loop, action column), queue panel, transcript view, armv7hl RPM (after a real-device run), STORE
variant with the reduced Qt Multimedia player (E2/E9), shared `music.youtube.com` link routing with SailTune, casting
post-parity. The unbuilt rows of the parity matrix are in `docs/YOUTUBE_PARITY.md`.

## Housekeeping tracked here

- Extract `nordtile-yt-runtime` (SHARING.md plan) once SailTune's streaming component is idle.
- CI glibc guard (PLT-02) is in `tools/rpm_audit.py`; the release job, the weekly aarch64 live-check runner (NFR-TST-07)
  and the NFR-SEC-06 distribution review are not done.
