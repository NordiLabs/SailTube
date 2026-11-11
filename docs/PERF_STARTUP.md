# Start-up cost (process start to first frame)

Measured 2026-11-08 in the build container `sfos-m1-sailtube` (Sailfish OS 5.1.0.11 aarch64 userland on an
Apple M1, 4 CPUs shared with other builds), commit after `820c0fe` with the start-up trace. The phone was
not available; device numbers are the open item at the end. Ux owns the start-up experience; core owns the
wiring in `Clients/Sailfish/Composition/main.cpp` and this measurement.

## Method

`SAILTUBE_STARTUP_TRACE=1` prints one line per phase with the milliseconds since the **process** started
(the kernel start time from `/proc/self/stat`, so dynamic loading before `main()` counts).
`SAILTUBE_STARTUP_TRACE=exit` quits once the scene exists. `tools/startup_trace.sh [RUNS]` runs the binary of
the last native build in the container N times with a fresh home (first launch: empty stores, schema
creation, GStreamer registry) and N times on one home (later launches), and prints medians.

The container has only the `minimal` platform plugin and Qt 5.6 Quick has no software renderer, so the
last phase measured here is **the QML scene created** (root object ready). Scene-graph and GL set-up up to the
first frame are device-only.

## Results (median of 5 runs, ms since process start)

| Phase (cumulative) | First launch | Later launch | What it is |
|---|---|---|---|
| entry | 13 | 19 | `main()` reached: exec, dynamic linking, static init |
| application | 20 | 20 | `SailfishApp::application` |
| settings + GStreamer probe | 189 | 34 | `AppSettings`, `probeGstreamer` (dlopen GStreamer, element/caps scan). First launch: GStreamer builds its registry cache (+155 ms) |
| resolver wiring | 189 | 34 | runners, services, warm resolver object (no process started) |
| library open | 250 | 55 | SQLite open; first launch creates the schema (+40 ms) |
| downloads open | 251 | 56 | queue store open |
| view created | 256 | 59 | `SailfishApp::createView` |
| catalog exposed | 265 | 61 | catalog composition, context properties |
| **QML scene** | **466** | **253** | `setSource`: Silica `ApplicationWindow`, initial page, MPRIS controls, loaders |

Floor for comparison (same container): an empty Silica `ApplicationWindow` takes 64–85 ms to create, one
page with a 20-row `SilicaListView` about 160 ms. SailTube's scene (192 ms on later launches) is close to that
floor: Qt 5.6 has no QML disk cache, so the QML is compiled at every start.

## What is deferred

- **Warm resolver workers** (`docs/PERF_RESOLVER.md`): the latency worker starts 1.5 s after the first frame
  (`QQuickWindow::frameSwapped`), the background worker on its first call. Neither adds to start-up.
- Catalog image cache and network access manager: created by QML on the first image, not at start.
- Translations: `SailfishApp::application` loads only the catalog of the current locale.

## Candidates, by expected gain on the phone

1. **QML scene (75 % of later launches).** Ux: keep the initial page to what the first screen shows (lists
   with their model already in memory, no hidden pages or sections instantiated, heavy pages behind
   `Loader { asynchronous: true }` or pushed on demand), keep `CoverPage` a `Component` (already so).
   Each Silica page that is created at start costs roughly 80–100 ms here.
2. **First-launch GStreamer registry (155 ms here, likely 0.5–1 s on the phone).** One-time cost; could move
   after the first frame if the probe ran lazily before the first `QMediaPlayer` (playback component: the probe must
   still run before any pipeline exists and before `deviceCapabilities` is read by QML).
3. **Library schema creation (40 ms first launch, 21 ms later).** Library component: opening is cheap; heavy
   migrations, if any are added, belong after the first frame.

## Design requests (a) downloads before the view, (b) probe after the first frame — measured

Re-measured after build `87629a3` (new GStreamer player engine), trace split so the probe is its own
phase (median of 5, ms since process start):

| Phase | First launch | Later launch |
|---|---|---|
| gst-init (`GstEngine::initialize`: gst_init, demotion, factory checks) | +117 | +13 |
| gst-probe (capability probe, after gst-init) | +0 | +0 |
| library-open | +74 | +5 |
| downloads-open (runner, manager open, model) | +6 | +0 |
| QML scene | +179 | +161 |
| **scene ready** | **392** | **196** |

- **(b) probe:** with GStreamer already initialised the capability probe costs under 1 ms, so caching or
  deferring it gains nothing. The cost is **gst_init** (the registry scan, 117 ms on a first launch). It is paid
  at start because `PlaybackSession`'s constructor creates the `VideoPlayer`, whose constructor calls
  `GstEngine::initialize`. Deferring it needs the playback component to create the player on the first play (or
  after the first frame); core then moves `main.cpp`'s explicit `GstEngine::initialize` and the probe behind the
  first frame (they must still run before the first pipeline and before QML reads `deviceCapabilities`).
  Since this measurement (2026-11-08) `GstEngine` start-up runs on its own worker thread and the session no longer
  waits for the probe (POCO run 5: first frame 1.59 s; run 6: 1.77 to 1.88 s); the parity
  matrix still lists the join before the first frame as partial.
- **(a) downloads:** opening the runner, the queue store and the model costs 0–6 ms, so the view is not waiting
  on it; no change made (the library component keeps its open order).
- The remaining start-up work is the QML scene (about 80 % of a later launch), which the ux and design components own.

## Device steps (open)

Run on R2 (POCO X3 Pro) with the RPM: `SAILTUBE_STARTUP_TRACE=1 harbour-sailtube` from SSH (`invoker`
adds its own time before `entry`), record the trace for a first and a later launch, then the same with the
home screen's launch animation (tap-to-first-frame by stopwatch or `journalctl` timestamps). Steps I2 in
`docs/DEVICE_CHECKLIST.md`.
