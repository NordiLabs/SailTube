<!-- SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0 -->
# E12: Speculative resolve on touch-down

Status: **prepared, not run** (2026-11-08). Needs app support first (below).
Spec: §7 E12, RES-11 (last bullet, MAY), RES-10, NFR-PERF-02. Device: R1 per the spec; R2 if R1 is
not available (record which).

## Question

Starting the resolve on press of a list row, and cancelling it if the press does not become a tap,
how much tap-to-first-frame latency does it save, and what share of the speculative resolves is
wasted?

Unknowns: the press-to-release time of real taps on Silica list items (the upper bound of what can
be saved); how often scrolling or long-press starts a press that is not a tap; whether wasted
resolves trigger rate limiting (RES-12 strike logic and DIA-01 429 counters should stay flat).

## Method

Build/flags: a build where the DIA-03 timing log is on and speculative resolve is switchable without
rebuilding. Proposal for the lead: a debug setting or env var `SAILTUBE_SPECULATIVE_RESOLVE=0|1`,
plus the markers `speculative-start`, `speculative-cancel`, `speculative-used` (key `run=<press id>`)
next to `tap`/`resolve-done`/`first-frame` (`tools/experiments/perf_timing.md`). A speculative
resolve must follow RES-11: never starts the PO helper, counts toward the 2-concurrent cap, and is
single-flight with the real resolve (RES-10) so the tap joins it.

1. The maintainer clears the slot. Preamble: mute + verify (the samplers' `common.sh`, or the same
   `pactl` steps by hand if no script runs).
2. A run (off): launch, one warm-up tap, then 20 taps on fresh videos from a feed or search page on
   Wi-Fi, scrolling normally between taps (scrolls create presses that must not count as taps).
   Export the timing log as `off.log`.
3. B run (on): same list type, 20 different fresh videos, same scrolling. Export as `on.log`.
4. Host: `python3 -I tools/experiments/perf_summary.py --baseline off.log on.log`. It reports
   `ttff.fast.wifi` for B, `speculative.waste` (cancelled / started) for B, and
   `e12.saved-p50.fast.wifi` = p50(A) − p50(B).
5. Also compare the DIA-01 429 counter before and after B.
6. Pause players at the end (`exp_end` does it when a script ran); write the dated note.

## Pass criterion

Saved p50 ≥ 150 ms and waste ≤ 30 % (`e12.saved-p50.fast.wifi` verdict PASS), with no rise in 429s.

## What it decides

Whether RES-11's optional speculative resolve on press ships (default on), ships off, or is dropped.

## Result notes

(none yet)
