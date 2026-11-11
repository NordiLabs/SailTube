<!-- SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0 -->
# E4: PO tokens on armv7hl

Status: **prepared, not run** (2026-11-08). No device script: the POCO is aarch64, so this needs a
32-bit target (the SDK's armv7hl emulator and one real 32-bit device, spec §1.3).
Spec: §7 E4, RES-14, RES-13, §1.3 target devices.

## Question

Is there a JavaScript runtime that can run the BotGuard challenge on 32-bit ARM? RES-14's provider
runs on Deno, which has no 32-bit build. Candidates: the bundled QuickJS-NG (SailTune's private
library) running the provider's script, or Node for armv7l. Measure time and memory per token.

Open points before any run: whether the provider's script runs outside Deno/Node at all (it may
need Node or Deno APIs, or a DOM shim, that QuickJS-NG lacks); whether Node still ships armv7l
builds for the target glibc; the provider's licence, to be checked before anything is bundled (RES-14
runs it as a separate, pinned, user-enabled helper, never as copied code).

## Method

Build/flags: armv7hl target in the SailTube build container (`sfos-m1-sailtube`), QuickJS-NG built
for armv7hl, FULL variant only (RES-14 is `[FULL]`).

1. On aarch64 first (the POCO, or the host), mint one token with the pinned provider on Deno as the
   reference: wall time, peak RSS, token length. This is the known-good baseline.
2. QuickJS-NG path: run the provider's challenge script under QuickJS-NG on aarch64, then on the
   armv7hl emulator. Record the first missing API or the error; if it runs, wall time and peak RSS
   (`/usr/bin/time -v` or `/proc/<pid>/status` VmHWM).
3. Node path: install an armv7l Node build in the emulator; run the provider; same measurements.
4. Validate a minted token the way RES-13 expects: a fast-path URL with `pot=` delivers bytes after
   the young-token window (about 3 s). A token that never streams does not count.
5. Repeat the working path on one real 32-bit device for the time number (emulator time is not
   representative). The device step follows the phone rules in `README.md`.

## Pass criterion

A token minted in under 15 s and under 1 GB peak memory on a real 32-bit device, and it streams.

## What it decides

Whether armv7hl stays token-free only (the default assumption in §1.3 and RES-14), or gets an
optional provider like aarch64.

## Result notes

(none yet)
