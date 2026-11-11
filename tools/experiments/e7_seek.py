#!/usr/bin/env python3
# SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
"""E7 seek timer: N flush-seeks on one pipeline, timed from the seek call to ASYNC_DONE.

Needs python3-gobject with GStreamer 1.0 introspection. Run with `python3 -I`.

    python3 -I e7_seek.py --pipeline 'souphttpsrc location=URL ! queue ! qtdemux name=d d. ! fakesink name=sink sync=true' \
        --flags key|accurate|flush|snap --seeks 20 --seed 7 --out results.tsv --label push-http-mp4

The pipeline must contain exactly one sink. Seek targets are drawn from [5 s, duration - 10 s] with
a fixed seed so every mode sees the same targets. A seek that returns False (refused) or does not
reach ASYNC_DONE within --seek-timeout seconds is recorded as such and the run continues, up to
--max-timeouts timeouts (the accurate-seek dead end can take 10-50 s per seek).
"""
import argparse
import random
import sys
import time

import gi

gi.require_version("Gst", "1.0")
from gi.repository import Gst  # noqa: E402

FLAGS = {
    "flush": Gst.SeekFlags.FLUSH,
    "key": Gst.SeekFlags.FLUSH | Gst.SeekFlags.KEY_UNIT,
    "snap": Gst.SeekFlags.FLUSH | Gst.SeekFlags.KEY_UNIT | Gst.SeekFlags.SNAP_BEFORE,
    "accurate": Gst.SeekFlags.FLUSH | Gst.SeekFlags.ACCURATE,
}
WAIT = Gst.MessageType.ASYNC_DONE | Gst.MessageType.ERROR | Gst.MessageType.EOS


def wait_for(bus, seconds):
    """Returns ('async-done'|'error: ...'|'eos'|'timeout', elapsed_ms)."""
    start = time.monotonic()
    message = bus.timed_pop_filtered(int(seconds * Gst.SECOND), WAIT)
    elapsed = int((time.monotonic() - start) * 1000)
    if message is None:
        return "timeout", elapsed
    if message.type == Gst.MessageType.ERROR:
        error, _debug = message.parse_error()
        return "error: " + error.message.replace("\t", " "), elapsed
    if message.type == Gst.MessageType.EOS:
        return "eos", elapsed
    return "async-done", elapsed


def position_ms(pipeline):
    ok, value = pipeline.query_position(Gst.Format.TIME)
    return int(value / Gst.MSECOND) if ok else -1


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--pipeline", required=True)
    parser.add_argument("--flags", choices=sorted(FLAGS), default="key")
    parser.add_argument("--seeks", type=int, default=20)
    parser.add_argument("--seed", type=int, default=7)
    parser.add_argument("--seek-timeout", type=float, default=60.0)
    parser.add_argument("--max-timeouts", type=int, default=3, help="stop after N seek timeouts")
    parser.add_argument("--preroll-timeout", type=float, default=30.0)
    parser.add_argument("--label", default="run")
    parser.add_argument("--out", required=True)
    args = parser.parse_args()

    Gst.init(None)
    pipeline = Gst.parse_launch(args.pipeline)
    bus = pipeline.get_bus()
    out = open(args.out, "a")

    def row(index, target, accepted, outcome, elapsed, landed):
        error = landed - target if landed >= 0 and target >= 0 else ""
        out.write("%s\t%s\t%d\t%d\t%d\t%s\t%d\t%d\t%s\n" % (args.label, args.flags, index, target,
                                                            int(accepted), outcome, elapsed, landed, error))
        out.flush()
        print("e7: %s %s seek=%d target=%d accepted=%d outcome=%s ms=%d landed=%d"
              % (args.label, args.flags, index, target, int(accepted), outcome, elapsed, landed))

    pipeline.set_state(Gst.State.PAUSED)
    outcome, elapsed = wait_for(bus, args.preroll_timeout)
    if outcome != "async-done":
        row(0, -1, False, "preroll " + outcome, elapsed, -1)
        pipeline.set_state(Gst.State.NULL)
        return 1
    ok, duration = pipeline.query_duration(Gst.Format.TIME)
    duration_ms = int(duration / Gst.MSECOND) if ok else -1
    seekable = Gst.Query.new_seeking(Gst.Format.TIME)
    seekable_flag = pipeline.query(seekable) and seekable.parse_seeking()[1]
    print("e7: %s preroll_ms=%d duration_ms=%d seeking_query=%s"
          % (args.label, elapsed, duration_ms, bool(seekable_flag)))
    if duration_ms < 20000:
        row(0, -1, False, "clip shorter than 20 s or duration unknown", 0, -1)
        pipeline.set_state(Gst.State.NULL)
        return 1

    pipeline.set_state(Gst.State.PLAYING)
    pipeline.get_state(int(args.preroll_timeout * Gst.SECOND))
    time.sleep(2.0)
    while bus.pop() is not None:  # drop stale messages so each wait sees its own ASYNC_DONE
        pass
    rng = random.Random(args.seed)
    timeouts = 0
    for index in range(1, args.seeks + 1):
        target = rng.randint(5000, duration_ms - 10000)
        start = time.monotonic()
        accepted = pipeline.seek_simple(Gst.Format.TIME, FLAGS[args.flags], target * Gst.MSECOND)
        if not accepted:
            row(index, target, False, "refused", int((time.monotonic() - start) * 1000), position_ms(pipeline))
            continue
        outcome, _ = wait_for(bus, args.seek_timeout)
        elapsed = int((time.monotonic() - start) * 1000)
        row(index, target, True, outcome, elapsed, position_ms(pipeline))
        if outcome.startswith("error") or outcome == "eos":
            break
        if outcome == "timeout":
            timeouts += 1
            if timeouts >= args.max_timeouts:
                print("e7: %s %s stopped after %d seek timeouts" % (args.label, args.flags, timeouts))
                break
        time.sleep(1.0)
    pipeline.set_state(Gst.State.NULL)
    out.close()
    return 0


if __name__ == "__main__":
    sys.exit(main())
