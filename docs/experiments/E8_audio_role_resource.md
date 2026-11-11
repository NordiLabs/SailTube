<!-- SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0 -->
# E8: Audio role and resource on SFOS 5.x

Status: **prepared, not run** (2026-11-08).
Spec: §7 E8, PLY-23, PLT-12, PLT-11, §6 M3 and M4. Devices: R2 (POCO, SFOS 5.2), then R1.

## Question

Which PulseAudio `media.role` does the Sailfish policy treat as media playback, and what does the
audio resource (libaudioresource) add? Wanted behaviour: pause on an incoming call (within 1 s,
PLY-23), duck on navigation prompts. Candidates: `x-maemo`, `music`, `media`, `video`.

Unknowns (no device data): which policy modules are loaded on 5.2; whether the policy corks a stream
by role alone or only for streams whose process holds a resource; whether the policy ties a stream to
a resource by process id; whether ringing alone (call not answered) is enough to take the resource;
whether alarms and navigation prompts cork or duck; whether a navigation app with voice prompts is
installed on the test phone.

## Method

Build/flags: no app build. `gst-launch-1.0` for role-only runs; `python3-gobject` and
`libaudioresource.so.1` (public API: `audioresource_init`, `_acquire`, `_release`, `_free`; types
`AUDIO_RESOURCE_GAME` = 1, `AUDIO_RESOURCE_MEDIA` = 2) for resource runs. Everything is inaudible:
sinks muted at 0 % (verified before each role and after the stream starts), the stream is
`audiotestsrc wave=silence` into `pulsesink volume=0`.

1. The maintainer clears the slot; copy tools to the phone. A second phone is needed to place the call.
2. Role only: `sh e8_audio_role.sh` (all four roles) or `sh e8_audio_role.sh x-maemo music`.
   For each role the script logs the loaded policy modules (once), the stream's sink-input block
   (`media.role`, `policy.*` properties, `Sink`, `Corked`, `application.process.id`), then polls
   Corked/Mute/Sink/volume every 2 s for 90 s (`E8_HOLD_S`, `E8_POLL_S`).
3. Manual part, during each 90 s hold (the script prints `>>> OPERATOR`), noting wall-clock times:
   * call the phone from the second phone, let it ring at least 10 s, then reject it (do not answer:
     answering routes voice audio outside these sinks and is not needed for the policy question);
   * an alarm set in the Clock app to fire inside the window; dismiss it;
   * a navigation voice prompt, only if a navigation app is installed; otherwise record "not tested".
4. Resource runs: `E8_RESOURCE=media sh e8_audio_role.sh <best role>` and `E8_RESOURCE=game ...`.
   `e8_resource.py` plays the same silent stream in-process, acquires the resource before PLAYING
   (the PLY-23 order), pauses on `acquired=false` and logs each callback and how long the pause took.
   Repeat step 3.
5. M4: from step 2, the sink-input listing shows the role on the device.
6. Copy back `poll.<role>.tsv`, `sink-input.<role>.txt`, `resource.<role>.txt` and the operator's
   times; write the dated note.

## Pass criterion

For one role (with or without the resource): the stream is corked or the resource is lost within
1 s of the call starting to ring, and returns after it ends; a navigation prompt ducks rather than
corks (or "not tested" is recorded).

## What it decides

PLY-23 and PLT-12: the `media.role` value every audio sink carries (the host sink-property test then
asserts it), the resource type (`media` expected), and whether resume after a call needs only the
resource callback. PLT-11: audio relies on this resource, not KeepAlive.

## Result notes

(none yet)
