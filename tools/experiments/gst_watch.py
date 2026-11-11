#!/usr/bin/env python3
# SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
"""Run a command (normally gst-launch-1.0 -v ... ! fakesink silent=false) with a hard timeout and
report when the first matching output line arrived, how many matched, and the child's CPU time.

Device-side helper for docs/experiments (E3). Python 3 stdlib only; run with `python3 -I`.

    python3 -I gst_watch.py --timeout 3 --match 'last-message = chain' --log raw.txt -- CMD...

Prints one line:
    gst-watch: rc=0 timed_out=0 first_ms=412 frames=88 elapsed_ms=3004 cpu_ms=1630
Only lines before the timeout count; first_ms is -1 when nothing matched. On timeout the child
gets SIGINT, then SIGTERM, then SIGKILL.
"""
import argparse
import os
import re
import selectors
import signal
import subprocess
import sys
import time


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--timeout", type=float, default=3.0, help="seconds before stopping")
    parser.add_argument("--match", default="last-message = chain", help="regex for one frame")
    parser.add_argument("--log", help="file for the timestamped raw output")
    parser.add_argument("command", nargs=argparse.REMAINDER)
    args = parser.parse_args()
    command = args.command[1:] if args.command[:1] == ["--"] else args.command
    if not command:
        parser.error("no command")
    pattern = re.compile(args.match)
    log = open(args.log, "w") if args.log else None

    start = time.monotonic()
    try:
        child = subprocess.Popen(command, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    except OSError as error:
        print("gst-watch: rc=127 timed_out=0 first_ms=-1 frames=0 elapsed_ms=0 cpu_ms=0 exec_error=%s"
              % error.errno)
        return 127

    selector = selectors.DefaultSelector()
    selector.register(child.stdout, selectors.EVENT_READ)
    first_ms = -1
    frames = 0
    timed_out = False
    pending = b""
    stage = 0  # 0 running, 1 SIGINT sent, 2 SIGTERM sent, 3 SIGKILL sent
    deadline = start + args.timeout
    eof = False
    while not eof:
        now = time.monotonic()
        if now >= deadline:
            if stage == 3:
                break  # killed; a grandchild may still hold the pipe open
            timed_out = True
            # os.kill, not Popen.send_signal: that one may reap the child and lose its rusage.
            try:
                os.kill(child.pid, (signal.SIGINT, signal.SIGTERM, signal.SIGKILL)[stage])
            except ProcessLookupError:
                pass
            stage += 1
            deadline = now + 1.0
        for _key, _mask in selector.select(timeout=max(0.0, min(0.1, deadline - now))):
            chunk = os.read(child.stdout.fileno(), 65536)
            if not chunk:
                eof = True
                break
            pending += chunk
            while b"\n" in pending:
                raw, pending = pending.split(b"\n", 1)
                line = raw.decode("utf-8", "replace")
                at_ms = int((time.monotonic() - start) * 1000)
                if stage == 0 and pattern.search(line):
                    frames += 1
                    if first_ms < 0:
                        first_ms = at_ms
                if log:
                    log.write("%6d %s\n" % (at_ms, line))
    _pid, status, usage = os.wait4(child.pid, 0)
    child.returncode = os.waitstatus_to_exitcode(status) if hasattr(os, "waitstatus_to_exitcode") \
        else (-os.WTERMSIG(status) if os.WIFSIGNALED(status) else os.WEXITSTATUS(status))
    elapsed_ms = int((time.monotonic() - start) * 1000)
    cpu_ms = int((usage.ru_utime + usage.ru_stime) * 1000)
    if log:
        log.close()
    print("gst-watch: rc=%d timed_out=%d first_ms=%d frames=%d elapsed_ms=%d cpu_ms=%d"
          % (child.returncode, int(timed_out), first_ms, frames, elapsed_ms, cpu_ms))
    return 0


if __name__ == "__main__":
    sys.exit(main())
