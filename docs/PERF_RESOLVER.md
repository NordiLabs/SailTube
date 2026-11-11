# Resolver fallback path: warm worker (RES-01, CORE-03)

The yt-dlp path is SailTube's fallback when the shared fast path cannot answer, and the only path for search
until the catalog's InnerTube transport covers it. Before this change every call started a new child:
the app binary in resolver mode, a QML engine, PyOtherSide's Python, an import of the 3 MB yt-dlp archive,
then the run. Now a long-lived worker per lane keeps yt-dlp imported.

## Design

- `Core/Worker/Runtime/worker.py` (hosted by the app binary in worker mode, `Core/Worker/Infrastructure/
  PythonWorkerChild`, through PyOtherSide and `Worker.qml`) imports yt-dlp once and runs one command line per
  request, capturing yt-dlp's stdout/stderr per request. It reuses SailTune's pinned `resolver.py` temporary
  storage policy (per-request budget).
- `Core/Worker/Infrastructure/WarmResolver`: two lanes, each one worker process: **latency** (the main
  runner: play, details, search today) and **background** (downloads). Same contract as `YtDlpRunner::run`.
  `YtDlpRunner` routes to it through the small `WarmBackend` interface and keeps the one-shot child as the
  fallback.
- **Player-JS cache on**: `--no-cache-dir` becomes `--cache-dir <cache>/yt-dlp` (private 0700), so yt-dlp keeps
  the player script and challenge solutions between calls and app starts; with the old flag every full-profile
  resolve fetched and solved the player JS again. This gain needs YouTube and is a device/online measurement.
- NFR-ROB-03 watchdog: a call running past 30 s is logged and the lane's queued calls fail at once with
  "busy, retry"; one process per lane, never double-spawned; a call past its own timeout restarts the worker.
- NFR-ROB-04: the worker loads a private copy of the archive, so an update of the installed file mid-session
  cannot break lazy imports; diagnostics report the skew until the worker restarts.
- PLT-14: workers are `HelperProcess` children (own group, parent-death signal, absolute path).
- Start: the latency worker 1.5 s after the first frame (no start-up cost); the background worker on demand.
  `SAILTUBE_NO_WARM_RESOLVER=1` turns the warm path off (A/B, diagnostics).

## Measured (offline, container `sfos-m1-sailtube`, real yt-dlp 2026.10.26 archive, local-file extraction)

`tests/resolver_bench` runs the shipped one-shot child (pinned `PythonResolverChild`) and the warm worker
end to end on the same command line, 8 runs each, and fails unless warm is at least 3x faster:

| Path | First call | Median | Max |
|---|---|---|---|
| One-shot child per resolve (before) | 1595 | 1595 | – |
| Warm worker | 1769 (start + 709 ms import) | **104** | 114 |
| Second run (lighter container load): one-shot | 939 | 939 | – |
| Second run: warm worker | 937 (start + 371 ms import) | **63** | 105 |

Between **0.9 and 1.5 s saved per resolve** (10–15x) on this machine depending on load, before any network time; the phone's CPU is slower, so
the absolute saving there should be larger. `tests/warm_worker` (python3 host of the same worker) measured
3.9 s first call / 185 ms warm median under heavier container load.

Device steps: P1/P2 in `docs/DEVICE_CHECKLIST.md` with the fast path off (fallback only), the timing log on
(DIA-03), comparing `SAILTUBE_NO_WARM_RESOLVER=1` and the default.
