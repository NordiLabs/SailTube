# SailTube public beta (0.1.0-0.beta3)

SailTube is a YouTube client for Sailfish OS. This is its first public beta: a time-limited build to try
before the first release. It is **not for the Jolla Harbour store**; it is shared directly or through
OpenRepos, for aarch64 phones with Sailfish OS 5.1 or later.

## What is in this build

- The YouTube-style player overlay and settings sheet, cards on Home, Subscriptions and Search, the watch page
  below the player, Search chips, Library shelves, caption tracks for streamed videos, playability fixes, a stall
  watchdog, and settings that are all wired to the code that reads them. Its device sitting is
  `docs/DEVICE_CHECKLIST.md` (section "Beta 3 sitting: playback") and `docs/DEVICE_DESIGN.md`.
- **Left out on purpose:** Shorts stage 2 (inline playback, loop, action column), touch lock (built, hidden
  behind a release switch), auto-translated captions, a queue page, a transcript view, the channel banner, the
  Equalizer and Resolver updates settings rows (nothing reads them), and the STORE variant.

## The beta ends on 30 December 2026

- The beta works until **30 December 2026, 23:59:59 in your phone's local time**.
- From 14 days before, a banner tells you how many days are left (once a day; tap it to hide it until
  tomorrow). In the last 3 days it appears every time you start the app.
- The About page shows the build and the end date.
- **After the end** SailTube opens only a "This beta has ended" page. Playback, search, downloads and all
  network access are off. **Your data is never deleted or changed**: the page still lets you export a backup,
  and the next beta or the stable version opens your library, history and settings as they are.
- Turning the phone's clock back does not extend the beta: the app remembers the latest time it has seen,
  and never runs before its own build time. This is an honest effort, not copy protection: SailTube's source
  is available under PolyForm Noncommercial 1.0.0, and a rebuild from source has no such limit.
- The beta makes no extra network request and sends no telemetry; the end date is checked on the phone only.

## Install and upgrade

- Install `harbour-sailtube-0.1.0-0.beta3.aarch64.rpm` (for example with `pkcon install-local` or the file
  manager). The app needs `pyotherside`, `Amber.Mpris` and `Nemo.KeepAlive` from the Sailfish repositories.
- Any later beta or the stable release (`0.1.0-1` or newer) installs over it and keeps your
  data. To go back from a stable release to a beta, uninstall first (data stays in your home directory).

## What works

- Search, home and channel pages, video pages with description and chapters, comments.
- Playback through SailTube's own GStreamer engine: adaptive streams up to 1080p where the phone decodes them,
  progressive fallback, live streams, audio-only, background audio, MPRIS controls and the cover.
- Fast start: the shared fast path resolves most videos directly; a warm resolver (yt-dlp kept loaded) is the
  fallback.
- Local library: subscriptions, playlists, history with resume, watch-later; NewPipe/SailTube backup import
  and export.
- Downloads of video and audio, with offline playback.
- Optional signed-in features (cookie import), off by default.
- 10 languages: English, Spanish, Portuguese, French, German, Italian, Russian, Japanese, Chinese
  (simplified) and Arabic.

## Known limitations

- Not yet measured on many phones: playback start times, battery use and hardware decoding differ by device.
- YouTube may show a bot check on some networks; SailTube then falls back to its slower extractor, and some
  videos may not play until the extractor is updated.
- The PO-token helper (for networks that need it) must be installed by hand; see the documentation.
- Updating the bundled extractor from the app is built in but has no settings control yet; the beta uses the
  yt-dlp version it ships with (2026.10.26).
- There is no in-app problem report yet; report issues with the steps, your phone model and Sailfish OS
  version.
- The Harbour (STORE) variant is not part of this beta.

## For maintainers

- Built with `tools/rpm_build.sh --beta` (spec `--with beta`, qmake `CONFIG+=sailtube_beta
  SAILTUBE_BETA_EXPIRES=2026-12-30`). The build fails if the date is missing, unparsable, in the past or
  later than 2026-12-30 (`tools/check_beta_date.py`), and refuses `--with store`.
- Code: `Core/Beta` (policy and clock), `Clients/Sailfish/Presentation/BetaStatus`, the gate in
  `Clients/Sailfish/Composition/main.cpp`, `qml/BetaEnded.qml`, `qml/components/BetaBanner.qml`. Normal and
  STORE builds compile none of it (`tests/beta` checks that).
- Clock state: `~/.local/share/org.nordilabs.sailtube/SailTube/beta/clock` (0600: the highest time seen and the
  day the banner was last dismissed). It is the only thing the beta writes.
- Device test hook, only with `tools/rpm_build.sh --beta --debug-hooks`: start with
  `SAILTUBE_BETA_NOW=2026-12-20T10:00:00 harbour-sailtube` (banner) or `SAILTUBE_BETA_NOW=2026-12-31T10:00:00`
  (ended page). The fake time raises the stored high-water mark, so after an "ended" test the build stays ended
  on that phone: delete the clock file above to reset it.
