<!-- SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0 -->
# Weekly live check (NFR-TST-07)

`tools/live_check.sh` checks once a week that the parts of SailTube that depend on YouTube still
work against the live service. The offline suites run against recorded fixtures, so they cannot
notice when YouTube changes something. The script runs three steps. Each step sends one small batch
of requests, is never retried, and reports **PASS**, **FAIL** or **SKIPPED**.

| Step | What runs | Requests |
|---|---|---|
| `discovery` | One anonymous InnerTube WEB `search`, sent with the catalog's **built-in** client version (`ClientIdentity::builtInVersion()` in `Core/Catalog/Infrastructure/ClientIdentity.cpp`) through the transport of `tools/record_fixtures.py`. The step passes when the answer has video rows. | 1 POST |
| `fast_path` | One cold round of `tools/perf_playback.sh --local --rounds 1 --videos aqz-KE-bpKQ --modes cold`: the pinned SailTune fast path (visitor id, `/player`, rung probe), then MediaProxy and SailTube's GStreamer player up to the first decoded frame. | 1 resolve plus the first media bytes |
| `download` | One small download through the packaged extractor, run the way the app runs it: `Core/Worker/Runtime/worker.py` (warm worker protocol) imports the `yt-dlp.pyz` archive and gets the app's common yt-dlp arguments, parsed from `YtDlpRunner::commonArguments` (no JS runtime, no cookies, no retries). It runs `--format worstaudio/worst --max-filesize 10M` on `jNQXAC9IVRw` (19 s, about 120 KB). | 1 extraction plus 1 small file |

Together the steps cover what the requirement asks for: the fast path, the extractor (first rung,
with no JavaScript), the discovery endpoints and a small download. The request budget is
deliberately small: one search, one resolve and one small download. Do not loop the script, and do
not run it more often than weekly from one IP.

**SKIPPED, not FAIL.** A bot wall or a rate limit means the runner's IP was refused, not that
SailTube broke, so the step is SKIPPED. That covers:

- "Sign in to confirm you're not a bot", which the code classifies as `FailureKind::BotWall` /
  `bot_wall` in the pinned `Playability.h` and as `ErrorCode::BotCheck` in `YtDlpJson.cpp`;
- `LOGIN_REQUIRED`;
- HTTP 429 or "Too Many Requests".

`--skip STEP[,STEP]` also reports a step SKIPPED, marked `(requested)`.

**FAIL** is anything else. Examples: an HTTP error from search (a 400 hints that the built-in
version was rejected), no video rows (shape drift), a resolve that ends in `unplayable`,
`no_usable_format`, `probe_rejected` or `network`, a player error, an extractor error, no file
written, a missing packaged archive, or missing build dependencies.

The overall result is FAIL if any step failed, PASS if at least one step passed and none failed,
and SKIPPED if every step was skipped. The script exits 0 when no step failed, 1 when a step failed,
and 2 on a usage or infrastructure error.

## Output

Each run writes two files, named with the UTC date, to `--out` (default
`${XDG_STATE_HOME:-$HOME/.local/state}/sailtube-live`):

- `live-check-YYYY-MM-DDTHHMMZ.txt`: the summary, one line per step plus the overall line, the
  commit and where it ran;
- `live-check-YYYY-MM-DDTHHMMZ.log`: the full harness output. Media URLs and extractor diagnostics
  are reduced to `<url>`, and perf lines carry no addresses.

## Running it

- **From the Mac** (default on non-Linux): `tools/live_check.sh`. The script copies the files the
  steps need into a fresh `mktemp -d /tmp/sailtube-live.XXXXXX` in the `sfos-m1-sailtube` container
  (or `--container NAME`). It runs there with `--local`, copies the summary and log back to `--out`,
  and removes the directory. The container has Python 3.11, Qt5, GStreamer and the packaged runtime
  at `/builds/_vendor/python-runtime/runtime/yt-dlp.pyz`.
- **On the aarch64 runner** (Linux, default `--local`), the runner needs:
  - network access to `www.youtube.com` and `*.googlevideo.com`;
  - `python3` 3.10 or later (3.11 is what the device and container use);
  - the Qt5 and GStreamer development packages that `tools/perf_playback.sh` checks with
    `pkg-config` (`gstreamer-1.0`, `-app`, `-video`, `nemo-gstreamer-interfaces-1.0`, `egl`,
    `Qt5Core`, `Qt5Network`) plus a C++ compiler and `moc`. Running the same Sailfish build
    container image on the runner is the simplest way to get them;
  - the packaged `yt-dlp.pyz`, passed with `--archive` or `SAILTUBE_YTDLP_ARCHIVE`. The script also
    looks in `vendor/python-runtime/runtime/`, `/usr/share/harbour-sailtube/runtime/` and
    `/builds/_vendor/python-runtime/runtime/`. Use the same archive the current RPM ships, so the
    check tests what users run.

  A runner that cannot build the player can still run the other two steps with `--skip fast_path`.
  That step is then reported SKIPPED, so the gap stays visible in the summary.

Options: `--query TEXT` (default `kurzgesagt`, the fixture query), `--video ID` (fast path, default
`aqz-KE-bpKQ`, Big Buck Bunny, which has a VP9 + Opus pair) and `--download-video ID` (default
`jNQXAC9IVRw`).

## Scheduling on the residential aarch64 runner

The build host's IP is known to be bot-walled by YouTube, so the meaningful weekly run comes from a
residential IP: an external aarch64 Linux box (for example a Raspberry Pi 4/5 or similar) on a home
connection, outside this repository's infrastructure. Keep a checkout there, update it before each
run, and schedule one run a week at a quiet hour.

cron (`crontab -e` as the runner user; create `~/.local/state/sailtube-live` once):

```cron
# Sundays 04:17 local time; pull, then one live check; results in ~/.local/state/sailtube-live
17 4 * * 0  cd $HOME/sailtube && git pull --ff-only -q && tools/live_check.sh --local --archive $HOME/runtime/yt-dlp.pyz >> $HOME/.local/state/sailtube-live/cron.log 2>&1
```

systemd user timer (`~/.config/systemd/user/sailtube-live.service` and `.timer`, then
`systemctl --user enable --now sailtube-live.timer` and `loginctl enable-linger $USER`):

```ini
# sailtube-live.service
[Unit]
Description=SailTube weekly live check (NFR-TST-07)
Wants=network-online.target
After=network-online.target

[Service]
Type=oneshot
WorkingDirectory=%h/sailtube
ExecStartPre=/usr/bin/git pull --ff-only -q
ExecStart=%h/sailtube/tools/live_check.sh --local --archive %h/runtime/yt-dlp.pyz
TimeoutStartSec=20min
```

```ini
# sailtube-live.timer
[Unit]
Description=Weekly SailTube live check

[Timer]
OnCalendar=Sun *-*-* 04:17:00
RandomizedDelaySec=30min
Persistent=true

[Install]
WantedBy=timers.target
```

`Persistent=true` catches up on a run missed while the box was off. `RandomizedDelaySec` spreads the
start time so the runner never hits the service at the same second each week. If the runner has no
`git pull` access, copy a fresh snapshot there instead. The script needs no `.git`; without one the
summary shows `commit: unknown`.

## What to do with the result

Copy the `.txt` summary to the maintainer, or attach it to the weekly report. Then go by the
result:

- **PASS**: nothing to do. Keep the summaries; a slow drift in `tap-to-first-frame` is also worth a
  look against `docs/PERF_PLAYBACK.md`.
- **SKIPPED (bot_wall / rate_limited / login_required)**: the runner IP was refused. Do not retry in
  a loop. If the same step is SKIPPED two weeks in a row from the residential runner, treat it as a
  signal: YouTube may be bot-walling the client definition itself, not just the IP. Raise it with the
  playback component (fast path) or core (extractor).
- **FAIL, discovery**: re-run once by hand the next day to rule out a blip, then:
  - HTTP 400: the built-in WEB version is probably too old. The app itself still works, because it
    scrapes the version at run time. Update the version following `docs/CLIENT_DEFINITIONS.md` (one
    file plus a re-record with `tools/record_fixtures.py`).
  - No rows or a parse problem: shape drift. Re-record the catalog fixtures and let the catalog component
    fix its parsers against them.
- **FAIL, fast_path**:
  - `unplayable`, `no_usable_format` or `bad_response`: the pinned VISIONOS client may have rotated
    out. That is a SailTune change plus a re-pin (`docs/CLIENT_DEFINITIONS.md`, "The pinned SailTune
    caveat"), so send it to SailTune through the maintainer.
  - `probe_rejected` or a player error: the media host or the player changed. Send it to the
    playback component with the `.log`.
- **FAIL, download**: an extractor error usually means the bundled `yt-dlp.pyz` is too old. Update
  the runtime archive (NFR-MNT-01) and run again with the new one. "No file written" with exit code 0
  means a format or size problem: check the `.log`.
- **FAIL from the runner itself** (missing archive, missing build packages, Python too old, network
  error): fix the runner; this is not a SailTube defect.

Record every FAIL and its resolution in the maintainer's notes, so a recurring pattern shows up
over the weeks.

## Run record

**2026-11-08, build container `sfos-m1-sailtube`** (aarch64, Python 3.11.15, yt-dlp 2026.10.26;
OrbStack on the build Mac, so the build host's IP), commit `b43107c` plus local work:

```
discovery: PASS 15 videos for one search, 861 ms (WEB 2.20261105.05.01 built-in)
fast_path: PASS video aqz-KE-bpKQ tap-to-first-frame 1050 ms (path fast, cold)
download: PASS 117066 bytes (.mp4, yt-dlp 2026.10.26, video jNQXAC9IVRw, 2771 ms)
live_check: PASS (pass 3, skipped 0, fail 0)
```

The build host's IP is known to be bot-walled, so SKIPPED results were expected. On this run YouTube
answered all three steps: the bot wall on this IP comes and goes. The whole run, including the
player build in the container, took about 20 s. The result still does not stand in for the
residential runner, because a bot-walled week on this IP would be SKIPPED and say nothing.

## Limits

- The extractor is checked through its first rung (no JavaScript, no cookies). The full
  fast-path-to-extractor fallback ladder (RES-08) is covered offline by table-driven tests. The live
  check does not force a fallback, because that would cost a second resolve.
- The download step uses yt-dlp's own downloader in the app's worker. The app's `DownloadEngine`
  (QtNetwork, resume, remux) is tested offline in `tests/downloads_engine` and `tests/remux`.
- One video per step can be unlucky: if a video is removed or region-locked, the step FAILs with
  `final` or `unplayable`. Pick another freely licensed video with `--video` or `--download-video`.
