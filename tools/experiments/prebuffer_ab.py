#!/usr/bin/env python3
# SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
"""A/B of the media proxy's start pre-buffer from timing logs (device step B5, host harness).

    tools/experiments/prebuffer_ab.py --on LOG [LOG ...] --off LOG [LOG ...] [--keep-first]
    tools/experiments/prebuffer_ab.py --selftest

Each LOG holds `sailtube-timing:` lines of one launch (the app with diagnostics/timingLog on, or
tools/perf_playback): `tap` and `first-frame` with the same `run=`; `first-byte` and `proxy-open`
when present. A launch's first play is dropped unless --keep-first (it carries the cold resolver);
the host harness's `mode=cold` rounds are each their own process and are all kept.

Prints, per arm and group (path / mode), the nearest-rank median and p90 of tap -> first frame and
of the pieces around the proxy, then the paired view: the same video in the same position of the
two arms' sequences (first ON log with first OFF log, ...), OFF minus ON. Python 3.9 stdlib only.
"""
import argparse
import re
import sys

LINE = re.compile(r"sailtube-timing:\s+(\S+)\s+t=(-?\d+)(.*)")


def parse(path):
    """One launch: a list of plays {run, video, mode, path, t: {event: ms}} in tap order."""
    plays, by_run = [], {}
    for raw in open(path, encoding="utf-8", errors="replace"):
        m = LINE.search(raw)
        if not m:
            continue
        event, t, rest = m.group(1), int(m.group(2)), m.group(3)
        fields = dict(f.split("=", 1) for f in rest.split() if "=" in f)
        run = fields.get("run")
        if run is None:
            continue
        key = (run, fields.get("round", ""), fields.get("mode", ""))
        if event == "tap":
            by_run[key] = {"run": run, "video": fields.get("video", ""), "mode": fields.get("mode", ""),
                           "path": "", "t": {"tap": t}}
            plays.append(by_run[key])
            continue
        play = by_run.get(key)
        if play is None:
            continue
        if event == "resolve-done":
            play["path"] = fields.get("path", "")
        if event == "first-byte" and "branch" in fields:
            play["t"].setdefault("first-byte." + fields["branch"], t)
        play["t"].setdefault(event, t)
    return plays


def percentile(values, q):
    values = sorted(values)
    if not values:
        return None
    return values[min(len(values) - 1, max(0, int(-(-q * len(values) // 1)) - 1))]


def metrics(play):
    t, out = play["t"], {}
    if "first-frame" in t:
        out["tap>frame"] = t["first-frame"] - t["tap"]
    first_bytes = [v for k, v in t.items() if k.startswith("first-byte")]
    if "proxy-open" in t and first_bytes:
        out["open>byte"] = min(first_bytes) - t["proxy-open"]
    if first_bytes and "first-frame" in t:
        out["byte>frame"] = t["first-frame"] - min(first_bytes)
    return out


def group_of(play):
    return play["mode"] or play["path"] or "all"


def load(paths, keep_first):
    launches = []
    for p in paths:
        plays = parse(p)
        cold_processes = plays and all(q["mode"] == "cold" for q in plays)
        launches.append(plays if keep_first or cold_processes else plays[1:])
    return launches


def summarise(on_paths, off_paths, keep_first, out=sys.stdout):
    arms = {"on": load(on_paths, keep_first), "off": load(off_paths, keep_first)}
    groups = sorted({group_of(p) for arm in arms.values() for launch in arm for p in launch})
    rows = []
    out.write("%-10s %-4s %-11s %4s %8s %8s\n" % ("group", "arm", "metric", "n", "median", "p90"))
    for g in groups:
        for name in ("tap>frame", "open>byte", "byte>frame"):
            for arm in ("on", "off"):
                values = [metrics(p)[name] for launch in arms[arm] for p in launch
                          if group_of(p) == g and name in metrics(p)]
                if values:
                    rows.append((g, arm, name, len(values), percentile(values, 0.5), percentile(values, 0.9)))
                    out.write("%-10s %-4s %-11s %4d %8d %8d\n" % rows[-1])
    # Paired: launch i of ON with launch i of OFF, the same video and group, in order.
    diffs = {}
    for on, off in zip(arms["on"], arms["off"]):
        rest = list(off)
        for p in on:
            if "tap>frame" not in metrics(p):
                continue
            for q in rest:
                if q["video"] == p["video"] and group_of(q) == group_of(p) and "tap>frame" in metrics(q):
                    diffs.setdefault(group_of(p), []).append(metrics(q)["tap>frame"] - metrics(p)["tap>frame"])
                    rest.remove(q)
                    break
    for g in sorted(diffs):
        d = diffs[g]
        faster_off = sum(1 for x in d if x < 0)
        out.write("paired %-10s n=%d  off - on: median %+d ms, p90 %+d ms; off faster in %d of %d\n"
                  % (g, len(d), percentile(d, 0.5), percentile(d, 0.9), faster_off, len(d)))
    if not diffs:
        out.write("paired: no pairs (the logs carry no video= field, or the sequences differ)\n")
    return rows, diffs


def selftest():
    import io, os, tempfile
    def log(path, plays):
        with open(path, "w") as f:
            for i, (video, frame, opened, byte) in enumerate(plays, 1):
                f.write("x sailtube-timing: tap t=%d run=%d video=%s net=wifi\n" % (i * 10000, i, video))
                f.write("sailtube-timing: resolve-done t=%d run=%d path=fast\n" % (i * 10000 + 300, i))
                f.write("sailtube-timing: proxy-open t=%d run=%d\n" % (i * 10000 + opened, i))
                f.write("sailtube-timing: first-byte t=%d run=%d branch=video\n" % (i * 10000 + byte, i))
                f.write("sailtube-timing: first-frame t=%d run=%d\n" % (i * 10000 + frame, i))
    d = tempfile.mkdtemp()
    log(os.path.join(d, "on"), [("A", 3000, 400, 900), ("B", 1000, 350, 600), ("C", 1200, 350, 650), ("D", 1400, 350, 700)])
    log(os.path.join(d, "off"), [("A", 3100, 400, 950), ("B", 900, 350, 500), ("C", 1300, 350, 760), ("D", 1100, 350, 600)])
    buf = io.StringIO()
    rows, diffs = summarise([os.path.join(d, "on")], [os.path.join(d, "off")], False, buf)
    got = {(g, a, m): (n, med, p90) for g, a, m, n, med, p90 in rows}
    ok = (got[("fast", "on", "tap>frame")] == (3, 1200, 1400) and got[("fast", "off", "tap>frame")] == (3, 1100, 1300)
          and got[("fast", "on", "open>byte")] == (3, 300, 350) and diffs["fast"] == [-100, 100, -300])
    rows, diffs = summarise([os.path.join(d, "on")], [os.path.join(d, "off")], True, io.StringIO())
    ok = ok and len(diffs["fast"]) == 4
    print("prebuffer_ab selftest: %s" % ("PASS" if ok else "FAIL"))
    if not ok:
        print(buf.getvalue())
    return 0 if ok else 1


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--on", nargs="+", default=[])
    ap.add_argument("--off", nargs="+", default=[])
    ap.add_argument("--keep-first", action="store_true", help="keep each launch's first play")
    ap.add_argument("--selftest", action="store_true")
    a = ap.parse_args()
    if a.selftest:
        return selftest()
    if not a.on or not a.off:
        ap.print_usage()
        return 2
    summarise(a.on, a.off, a.keep_first)
    return 0


if __name__ == "__main__":
    sys.exit(main())
