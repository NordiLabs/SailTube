<!-- SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0 -->
# Pre-buffer on or off: the device A/B (step B5)

The pinned media proxy (SailTune 5611210) fetches the first 256 KiB and the last 16 KiB of every
stream when it is opened. On the host that showed no gain, because the player asks for data about
10 ms after the open; a phone prepares its pipeline for longer, so the head start may count there.
It costs four TLS handshakes per start instead of two and up to 272 KiB per opened stream.

The switch is per launch: `SAILTUBE_PREBUFFER=0` in the app's environment starts it without the
pre-buffer and the tail cache, `SAILTUBE_PREBUFFER=1` with them; unset uses the build's default
(`prebufferByDefault` in `Clients/Sailfish/Composition/main.cpp`: off since the interleaved host
A/B of 2026-11-11, which showed warm starts about 300 ms slower with it). The build on the phone
must contain that switch (it came after the second beta 3 candidate).

## Run (about 15 minutes, one network, the phone not moved)

1. Turn the timing log on (`diagnostics/timingLog=true` in
   `~/.config/org.nordilabs.sailtube/SailTube/SailTube.conf`, or `SAILTUBE_TIMING=1`). Mute the sinks.
2. Pick 6 videos that are in the feed or reachable by one tap each, and keep their order. The
   first play of every launch is dropped from the numbers (it carries the cold resolver), so make
   the first one a spare.
3. Six launches, alternating arms, each launch plays the same 6 videos in the same order and is
   closed afterwards (swipe the app closed, do not leave it on the cover):

   | launch | 1 | 2 | 3 | 4 | 5 | 6 |
   |---|---|---|---|---|---|---|
   | `SAILTUBE_PREBUFFER` | 1 | 0 | 0 | 1 | 1 | 0 |

   For each video: tap it, wait for the picture and three seconds more, go back. On two of the six,
   seek once to about the middle right after the picture appears (an early seek is where the tail
   cache and the pre-buffer should show).
4. Save each launch's log as its own file (`on.1.log`, `off.2.log`, `off.3.log`, `on.4.log`, …):
   the journal lines of that launch that contain `sailtube-timing:`.
5. Summarise on the Mac:

   ```
   tools/experiments/prebuffer_ab.py --on on.1.log on.4.log on.5.log --off off.2.log off.3.log off.6.log
   ```

## Reading it

* `tap>frame` is the user's wait; `open>byte` is the piece the pre-buffer can shorten (the proxy
  opened until the first media byte reaches the pipeline); `byte>frame` is the phone's own work.
* The paired line compares the same video at the same place of an ON and an OFF launch
  (`off - on`): a negative median means OFF was faster.
* Decide on the paired median of `tap>frame` and on `open>byte`: keep the pre-buffer if ON is
  faster by more than the spread between two launches of the same arm (compare `on.1` with `on.4`
  the same way to see that spread); otherwise switch it off — it then only costs data and
  connections. Five plays per launch and three launches per arm give 15 plays per arm: a
  difference under about 150 ms is inside the noise of one sitting.
* The default is one line, `prebufferByDefault`: turn it on for everyone only if ON wins here.

Host counterpart, interleaved the same way: `tools/perf_playback_ab.sh` (results in
`docs/PERF_PLAYBACK.md`).
