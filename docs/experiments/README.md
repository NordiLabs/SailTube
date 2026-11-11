<!-- SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0 -->
# SailTube device experiments

Spec: `SAILTUBE_REQUIREMENTS.md` §7 (experiments), §6 (device steps), §4.1 (performance).
Prepared host-side on 2026-11-08 for the playback component. **Nothing here has run on a device.** Every
statement about device behaviour in these files is a question or a hypothesis until a dated
result note says otherwise.

## Index

| Id | Question | Decides | Doc | Device script | Status |
|---|---|---|---|---|---|
| E2 | Does loopback work under Sailjail? (phase 0) | RES-17 hand-off (b), FULL sandboxing, STORE viability | [E2](E2_loopback_sailjail.md) | `tools/experiments/e2_loopback.sh` | prepared, not run |
| E3 | `droidvdec` per device | PLY-20, PLT-08, SEL-02 HW defaults | [E3](E3_droidvdec.md) | `tools/experiments/e3_droidvdec_probe.sh` | prepared, not run |
| E4 | PO tokens on armv7hl | RES-14 scope (armv7hl token-free only?) | [E4](E4_po_tokens_armv7hl.md) | none (needs a 32-bit target) | prepared, not run |
| E7 | qtdemux push-seek on current SFOS GStreamer | PLY-04, SEL-04 Opus preference | [E7](E7_qtdemux_push_seek.md) | `tools/experiments/e7_qtdemux_push_seek.sh` | prepared, not run |
| E8 | Audio role and resource on SFOS 5.x | PLY-23, PLT-12 values | [E8](E8_audio_role_resource.md) | `tools/experiments/e8_audio_role.sh` | prepared, not run |
| E12 | Speculative resolve on touch-down | RES-11 MAY | [E12](E12_speculative_resolve.md) | timing markers + `perf_summary.py --baseline` | prepared, not run |
| — | Performance budgets (§6 P1–P4, P7, T1) | NFR-PERF-02..06 | [perf_timing.md](../../tools/experiments/perf_timing.md) | `pss.sh`, `cpu_sample.sh`, `perf_summary.py` | prepared, not run |

Other §7 experiments are not prepared here: E1 (account), E5 (SailTune
streaming component), E6 (library), E9 (release), E10 (catalog), E11 (account).

Device steps covered: D3 and D7 (E3), M3 and M4 (E8), P1, P3, P7 and T1 (perf_timing.md).

## Files

| File | Runs on | Purpose |
|---|---|---|
| `tools/experiments/common.sh` | device | preamble and teardown sourced by every device script |
| `tools/experiments/gst_watch.py` | device | runs a gst-launch pipeline with a hard timeout; first-frame ms, frame count, child CPU |
| `tools/experiments/range_server.py` | device | loopback HTTP file server (Range/206/416, token, optional no-Range, rate cap) |
| `tools/experiments/e2_loopback.sh`, `e2_loopback.py` | device | E2 |
| `tools/experiments/e3_droidvdec_probe.sh` | device | E3 |
| `tools/experiments/e7_qtdemux_push_seek.sh`, `e7_seek.py` | device | E7 |
| `tools/experiments/e8_audio_role.sh`, `e8_resource.py` | device | E8 |
| `tools/experiments/pss.sh`, `cpu_sample.sh` | device | NFR-PERF-05/06 samplers |
| `tools/experiments/make_clips.sh` | host | synthetic H.264/VP9/Opus/AAC clips (ffmpeg `testsrc2`/`sine`) |
| `tools/experiments/perf_summary.py` | host | p50/p95 per metric against NFR-PERF budgets; tested by `tests/perf_summary/run.sh` |

## Who runs what

Only the maintainer starts device scripts, only after the phone owner says yes and no other
test is running. The maintainer tells SailTune before and after.

Prerequisites the maintainer must clear with the phone owner, because each changes the phone:

* Copying `tools/experiments/` and the clips to the phone, e.g. into
  `~/.cache/org.nordilabs.sailtube/SailTube/experiments/tools/` (inside the Sailjail layout, so E2's
  sandboxed process can read it).
* Packages, if missing: `gstreamer1.0-tools` (`gst-launch-1.0`, `gst-inspect-1.0`),
  `python3-gobject` (E7, E8 resource mode, E2 in-process stage). Each script reports what is missing
  and degrades or stops; none installs anything.
* E2 only: a root-installed test profile for the "without Internet" case (see the E2 doc).

Run as the session user over ssh, for example
`ssh defaultuser@<phone> 'sh ~/.cache/org.nordilabs.sailtube/SailTube/experiments/tools/e3_droidvdec_probe.sh <clips>'`.
Set `SAILTUBE_EXP_COMMIT=<git sha>` so `env.txt` records the tools version. Results land in
`~/.cache/org.nordilabs.sailtube/SailTube/experiments/<name>-<UTC stamp>/` (`run.log`, `env.txt`,
`results.tsv`, raw logs); copy that directory back and summarise it in the dated note.

## Preamble every device script runs first (`exp_begin`)

1. Refuse to run as root; warn if the user is not `defaultuser`. `XDG_RUNTIME_DIR` and
   `DBUS_SESSION_BUS_ADDRESS` default to `/run/user/<uid>` and its `dbus/user_bus_socket`.
2. **Window guard**: refuse to start if the step would touch SailTune's device-test windows
   (2 min before to 10 min after :17 and :52, i.e. free slots :02–:15 and :27–:50). Long
   experiments re-check before each step and wait (nothing playing) until it fits.
   `SAILTUBE_EXP_IGNORE_WINDOW=1` only after the maintainer cleared the slot with SailTune.
3. Create the result directory; record `env.txt`: UTC date, user, `/etc/sailfish-release`,
   `/etc/os-release`, `/etc/hw-release` (device model), `uname`, userland word size
   (`getconf LONG_BIT`), `gst-inspect-1.0 --version`, `pactl --version`, Python and gi/Gst
   availability, the sink list.
4. **Mute**: for every index in `pactl list short sinks`: `pactl set-sink-volume <i> 0` and
   `pactl set-sink-mute <i> 1`, as the session user.
5. **Verify**: parse `pactl list sinks`; every sink must show `Mute: yes` and every channel of its
   `Volume:` line at `0%` (`Base Volume` is ignored). Any failure aborts before anything plays.
   Scripts re-run mute + verify before each step and after starting any audio stream, because sinks
   can appear mid-run (Bluetooth, call routing).
6. Install the `EXIT` trap (`exp_end`). Nothing in the tools ever raises a volume or unmutes.

Test streams themselves are inaudible by construction: video experiments use `fakesink` only; E8
uses `audiotestsrc wave=silence` into `pulsesink volume=0`.

## Post-run steps (`exp_end`, also on error or Ctrl-C)

1. Stop every registered test process: SIGINT (gst-launch-1.0 `-e` turns it into EOS, the Python
   helpers end cleanly), then SIGTERM, then SIGKILL, two seconds apart.
2. **Pause every player**: `org.mpris.MediaPlayer2.Player.Pause` on each `org.mpris.MediaPlayer2.*`
   name on the session bus (SailTube, SailTune, the system player). `EXP_NO_PAUSE=1` skips this only
   when a later step in the same session pauses.
3. Mute and verify again; log any sink that is not muted.
4. Leave the sinks muted at 0 %. **Restoring the volume is the user's decision**; the tools never do it.
5. The maintainer removes anything it installed for the run (E2 test profile) and copies the result
   directory back.

## Dated result note (every experiment ends with one)

Append to the experiment's doc, under "Result notes", newest first:

```
### YYYY-MM-DD, <device> (R1|R2|other), run <result dir name>
- Method: script and tools commit, arguments, clips, deviations from the doc's steps.
- Device: model from hw-release, architecture, userland bits.
- OS: Sailfish OS version (sailfish-release), GStreamer version, PulseAudio version.
- Result: the numbers (copy the summary table), pass/fail against the doc's criterion,
  anything unexpected; path of the raw results kept on the host.
- Decision: what the result fixes (requirement id and value), or "no decision" and why; follow-ups.
```

Then update the row's status in the index above and in `docs/CHECKLIST.md`.
