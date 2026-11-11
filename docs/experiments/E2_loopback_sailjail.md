<!-- SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0 -->
# E2: Does loopback work under Sailjail?

Status: **prepared, not run** (2026-11-08). Phase 0, do early.
Spec: §7 E2, §1.5 variants table, RES-17, PLT-03, PLT-04. Device: R2 (POCO, SFOS 5.2) first, then R1.

## Question

Inside a Sailjail sandbox with `Internet` and `Audio`, can a process bind 127.0.0.1, serve media
from its own data directories, and have GStreamer **in the same process** fetch and decode from
it? What fails without `Internet`, and is a failure a bind, connect, exec or data-dir failure?
(The spec's inference: FinTube's "connection refused" was a data-dir or exec problem, not network
filtering. Not verified.)

## Hypothesis from public documentation (to be confirmed, not a result)

The public `sailjail-permissions` profiles show the Base permission with `protocol unix` and
`net none`, and the Internet permission with `ignore net none` and `protocol inet,inet6,netlink`.
If that holds on 5.2, then without `Internet` even `socket(AF_INET)` is refused by seccomp (so
loopback is impossible, not just filtered), and with `Internet` the host network namespace,
including `lo`, is visible. The probe therefore logs `socket()` separately from `bind()`. RES-17
option (a) (in-process `appsrc`, no socket) would not depend on any of this.

## Method

Build/flags: the installed SailTube RPM provides `/usr/share/applications/harbour-sailtube.desktop`
(`Permissions=Internet;Audio`, `OrganizationName=org.nordilabs.sailtube`, `ApplicationName=SailTube`).
Interim probe: `tools/experiments/e2_loopback.py` (Python 3 stdlib + optional python3-gobject). Final
form: an app experiment mode (below). Clip: `h264_720p.mp4` from `tools/experiments/make_clips.sh`.

1. The maintainer clears the slot (phone owner agrees, SailTune told). Copy `tools/experiments/`
   and the clip to `~/.cache/org.nordilabs.sailtube/SailTube/experiments/tools/`.
2. Launch SailTube once from the app grid, so sailjaild has the user's permission grant for it.
   Question: does a command-line `sailjail` launch prompt again, or fail without that grant?
3. `sh e2_loopback.sh h264_720p.mp4` (it runs the preamble: mute + verify). It copies the probe and
   clip into the result directory under the app's cache dir and runs `e2_loopback.py` as:
   * **A host**: no sandbox (baseline; every stage should pass).
   * **B internet**: `sailjail -p harbour-sailtube.desktop -- /usr/bin/python3 -I .../e2_loopback.py`.
   * **C nointernet**: `sailjail -p harbour-sailtube-e2-nonet.desktop -- ...`, only if that profile
     exists (step 4).
   * **D app**: with `E2_APP_MODE=1`, `sailjail -p harbour-sailtube.desktop -- /usr/bin/harbour-sailtube
     --experiment=loopback --experiment-clip=<path>`.
4. For case C the maintainer (with the phone owner's yes; root) installs, then removes after the run:
   `/etc/sailjail/applications/harbour-sailtube-e2-nonet.desktop`, a copy of the app's desktop file
   with `Permissions=Audio` and the same `OrganizationName`/`ApplicationName` (so data dirs match).
5. Each case logs one line per stage: `datadir` (the three PLT-03 dirs, `TMPDIR`, `/tmp`), `bind`
   (`socket()` then `bind()`/`listen()` on 127.0.0.1:0), `connect` (in-process Range GET, expects
   206), `gst` (in-process `souphttpsrc ! decodebin ! fakesink` for 3 s: bytes, frames), `exec`
   (child `gst-launch-1.0 souphttpsrc ! fakesink`). A case with no stage lines means the launcher
   itself failed; its output is in `case.<label>.txt`.
6. Copy the result directory back; write the dated note.

Sailjail CLI: the public sailjail README gives `sailjail -p <name>.desktop -- <absolute exe> [args]`,
with the profile looked up in `/usr/share/applications` or `/etc/sailjail/applications`.
**Uncertain:** whether sailjail accepts an executable other than the profile's `Exec` (here
`/usr/bin/python3`). If case B fails at launch for that reason, the interim probe cannot run
sandboxed and case D (the app's own mode) is the only valid test.

### App experiment mode (for the lead)

Proposal: `harbour-sailtube --experiment=loopback --experiment-clip=<file>` runs headless, starts the
real RES-17 loopback server (or the shared fetcher's) on the clip, plays it through the real
pipeline into `fakesink` for 3 s, prints the same `e2: stage=... result=...` lines, and exits 0/1.
This tests the real code, data dirs and exec path inside the real profile.

## Pass criterion

Case B (and D when available): `stage=gst result=ok` with frames > 0, and every PLT-03 `datadir`
stage ok. Case C is informational: it records which stage fails without `Internet`.

## What it decides

* RES-17: whether option (b), loopback, is allowed in sandboxed builds, or only (a).
* FULL: whether `Sandboxing=Disabled` can be dropped (with E3).
* STORE: viability of the dual-source player (with E9).

## Result notes

(none yet)
