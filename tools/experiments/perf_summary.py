#!/usr/bin/env python3
# SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
"""Summarise SailTube timing markers and samplers against the NFR-PERF-02..06 budgets.

Host-side, Python 3 stdlib only; run with `python3 -I`. Marker grammar: tools/experiments/perf_timing.md.

    python3 -I perf_summary.py [--tsv] [--keep-warmup] [--baseline OFF.log] LOG...

Rules (spec §0 and §4.1):
* Percentiles are nearest-rank over each series after discarding its first (warm-up) run;
  fewer than 20 remaining runs gives INCONCLUSIVE for a percentile budget.
* A budget written as a single bound ("<= 1.5 s") applies to every run, so it is checked on max.
* "Within 1 s of hiding (+800 ms grace)" is checked as hidden -> video-decode-stopped <= 1800 ms.
* MB in NFR-PERF-05 is 10^6 bytes (the stricter reading); PSS and spool samples are in KiB.
* Mean CPU needs >= 600 s between the first and last sample of a context, else INCONCLUSIVE.

Exit status: 0 when no metric FAILs, 1 when one does, 2 on usage errors.
"""
import argparse
import math
import re
import sys
from collections import OrderedDict, defaultdict

MARKER = re.compile(r"sailtube-(timing|sample):\s+(\S+)((?:\s+[A-Za-z_][\w.-]*=\S*)*)\s*$")
FIELD = re.compile(r"([A-Za-z_][\w.-]*)=(\S*)")
MIN_RUNS = 20
MB_KIB = 1e6 / 1024.0

# NFR-PERF-02: (p50 limit, p95 limit) in ms; None = no limit at that percentile.
TTFF_BUDGETS = {
    "ttff.fast.wifi": (1200, 2500),
    "ttff.fast.4g": (2000, 4000),
    "ttff.prefetch": (600, None),
    "ttff.extractor": (6000, 12000),
}
# NFR-PERF-03: single bounds, checked on every run.
SEEK_MAX = {
    "seek.out.webm.wifi": 1500,
    "seek.out.webm.4g": 3000,
    "seek.out.mp4.wifi": 2000,
    "seek.out.mp4.4g": 4000,
}


class Event(object):
    __slots__ = ("kind", "name", "t", "fields", "line")

    def __init__(self, kind, name, t, fields, line):
        self.kind, self.name, self.t, self.fields, self.line = kind, name, t, fields, line


def parse(paths):
    """Returns (events in file order, warnings)."""
    events, warnings = [], []
    for path in paths:
        with open(path, encoding="utf-8", errors="replace") as handle:
            for number, raw in enumerate(handle, 1):
                if "sailtube-timing:" not in raw and "sailtube-sample:" not in raw:
                    continue
                match = MARKER.search(raw.rstrip("\n"))
                fields = dict(FIELD.findall(match.group(3))) if match else {}
                try:
                    t = int(fields.get("t", ""))
                except ValueError:
                    warnings.append("%s:%d: malformed marker (needs name and integer t=)" % (path, number))
                    continue
                events.append(Event(match.group(1), match.group(2), t, fields, number))
    return events, warnings


def percentile(values, q):
    """Nearest-rank percentile of a non-empty list."""
    ordered = sorted(values)
    return ordered[max(0, int(math.ceil(q * len(ordered))) - 1)]


class Row(object):
    def __init__(self, metric, values=None, value=None, budget="-", verdict="INFO", n=None):
        self.metric, self.budget, self.verdict = metric, budget, verdict
        self.values = values or []
        self.value = value
        self.n = len(self.values) if n is None else n

    def cells(self):
        def fmt(number):
            return "-" if number is None else ("%d" % number if float(number).is_integer() else "%.1f" % number)
        p50 = percentile(self.values, 0.5) if self.values else None
        p95 = percentile(self.values, 0.95) if self.values else None
        top = max(self.values) if self.values else None
        return [self.metric, str(self.n), fmt(p50), fmt(p95), fmt(top), fmt(self.value), self.budget, self.verdict]


def pair(events, start_name, end_name, key):
    """Pairs start/end events sharing fields[key]; returns ([(start, end)], unpaired starts)."""
    open_starts, pairs = OrderedDict(), []
    for event in events:
        if event.kind != "timing" or key not in event.fields:
            continue
        ident = event.fields[key]
        if event.name == start_name:
            open_starts[ident] = event
        elif event.name == end_name and ident in open_starts:
            pairs.append((open_starts.pop(ident), event))
    return pairs, list(open_starts.values())


def series(pairs, keep_warmup):
    values = [end.t - start.t for start, end in pairs]
    return values if keep_warmup else values[1:]


def percentile_row(metric, values, p50_max, p95_max):
    budget = ",".join(part for part in ("p50<=%d" % p50_max if p50_max is not None else "",
                                        "p95<=%d" % p95_max if p95_max is not None else "") if part)
    if len(values) < MIN_RUNS:
        verdict = "INCONCLUSIVE(n<%d)" % MIN_RUNS
    elif (p50_max is not None and percentile(values, 0.5) > p50_max) or \
            (p95_max is not None and percentile(values, 0.95) > p95_max):
        verdict = "FAIL"
    else:
        verdict = "PASS"
    return Row(metric, values, budget=budget, verdict=verdict)


def max_row(metric, values, limit):
    if not values:
        return Row(metric, values, budget="max<=%d" % limit, verdict="INCONCLUSIVE(n=0)")
    return Row(metric, values, budget="max<=%d" % limit, verdict="PASS" if max(values) <= limit else "FAIL")


def tap_groups(events, keep_warmup):
    """Tap-to-first-frame grouped by path and network; early taps (<= 5 s after launch) apart."""
    taps, unpaired = pair(events, "tap", "first-frame", "run")
    paths = {}
    for event in events:
        if event.kind == "timing" and event.name == "resolve-done" and "run" in event.fields:
            paths[event.fields["run"]] = event.fields.get("path", "unknown")
    launches = [event.t for event in events if event.kind == "timing" and event.name == "launch"]
    groups, early = OrderedDict(), OrderedDict()
    for start, end in taps:
        path = paths.get(start.fields["run"], "unknown")
        net = start.fields.get("net", "unknown")
        base = "ttff.%s.%s" % (path, net) if path == "fast" else "ttff.%s" % path
        prior = [t for t in launches if t <= start.t]
        launch = max(prior) if prior else None
        target = early if launch is not None and start.t - launch <= 5000 else groups
        target.setdefault(base, []).append((start, end))
    rows = []
    for base, pairs in groups.items():
        values = series(pairs, keep_warmup)
        if base in TTFF_BUDGETS:
            p50_max, p95_max = TTFF_BUDGETS[base]
            rows.append(percentile_row(base, values, p50_max, p95_max))
        else:
            rows.append(Row(base, values))
    for base, pairs in early.items():
        # One early tap per launch, so there is no warm-up run to drop.
        values = [end.t - start.t for start, end in pairs]
        if base in TTFF_BUDGETS:
            p50_max, p95_max = TTFF_BUDGETS[base]
            rows.append(percentile_row("ttff.early." + base[5:], values, p50_max + 500,
                                       None if p95_max is None else p95_max + 500))
        else:
            rows.append(Row("ttff.early." + base[5:], values))
    # P1: at least 90 % of resolved taps use the fast path (prefetch and cache hits excluded).
    resolved = [paths.get(start.fields["run"]) for start, _end in taps]
    counted = [path for path in resolved if path in ("fast", "extractor")]
    if counted:
        share = 100.0 * counted.count("fast") / len(counted)
        rows.append(Row("fast-path-share", value=share, n=len(counted), budget="value>=90",
                        verdict="PASS" if share >= 90.0 else "FAIL"))
    return rows, len(unpaired)


def seek_rows(events, keep_warmup):
    pairs, _ = pair(events, "seek-start", "seek-done", "seek")
    groups = OrderedDict()
    for start, end in pairs:
        if "burst" in start.fields:
            continue  # double-tap bursts are judged by flush count, not latency
        if start.fields.get("inbuf") == "1":
            name = "seek.inbuf"
        else:
            name = "seek.out.%s.%s" % (start.fields.get("container", "unknown"), start.fields.get("net", "unknown"))
        groups.setdefault(name, []).append((start, end))
    rows = []
    for name, group in groups.items():
        values = series(group, keep_warmup)
        if name == "seek.inbuf":
            rows.append(percentile_row(name, values, 300, None))
        elif name in SEEK_MAX:
            rows.append(max_row(name, values, SEEK_MAX[name]))
        else:
            rows.append(Row(name, values))
    # Double-tap burst: exactly one flush-seek per burst id.
    bursts = OrderedDict()
    for event in events:
        if event.kind == "timing" and event.name == "seek-start" and "burst" in event.fields:
            bursts.setdefault(event.fields["burst"], 0)
    for event in events:
        if event.kind == "timing" and event.name == "seek-flush" and event.fields.get("burst") in bursts:
            bursts[event.fields["burst"]] += 1
    if bursts:
        flushes = list(bursts.values())
        rows.append(Row("seek.burst-flushes", flushes, budget="each==1",
                        verdict="PASS" if all(count == 1 for count in flushes) else "FAIL"))
    # PLY-04: no accurate flush-seek on an mp4 branch.
    mp4 = [event for event in events if event.kind == "timing" and event.name == "seek-flush"
           and event.fields.get("container") == "mp4"]
    if mp4:
        accurate = sum(1 for event in mp4 if event.fields.get("accurate") == "1")
        rows.append(Row("seek.mp4-accurate", value=accurate, n=len(mp4), budget="value==0",
                        verdict="PASS" if accurate == 0 else "FAIL"))
    return rows


def switch_rows(events, keep_warmup):
    pairs, _ = pair(events, "switch-start", "switch-resumed", "switch")
    groups = OrderedDict()
    for start, end in pairs:
        groups.setdefault("switch.%s" % start.fields.get("net", "unknown"), []).append((start, end))
    rows = []
    for name, group in groups.items():
        values = series(group, keep_warmup)
        rows.append(max_row(name, values, 2000) if name == "switch.wifi" else Row(name, values))
    return rows


def background_rows(events):
    """hidden -> next video-decode-stopped before the next shown."""
    values, pending = [], None
    for event in events:
        if event.kind != "timing":
            continue
        if event.name == "hidden":
            pending = event
        elif event.name == "shown":
            pending = None
        elif event.name == "video-decode-stopped" and pending is not None:
            values.append(event.t - pending.t)
            pending = None
    return [max_row("background.decode-stop", values, 1800)] if values else []


def sample_rows(events):
    rows = []
    limits = {"pss": {"playback-1080p-sw": 300 * MB_KIB, "feed": 180 * MB_KIB}, "spool": {}}
    for kind in ("pss", "spool"):
        by_context = OrderedDict()
        for event in events:
            if event.kind == "sample" and event.name == kind and "kb" in event.fields:
                by_context.setdefault(event.fields.get("context", "unspecified"), []).append(int(event.fields["kb"]))
        for context, values in by_context.items():
            limit = 512 * 1024 if kind == "spool" else limits["pss"].get(context)
            name = "%s.%s" % (kind, context)
            if limit is None:
                rows.append(Row(name, values, budget="kib"))
            else:
                rows.append(Row(name, values, budget="max<=%dKiB" % int(limit),
                                verdict="PASS" if max(values) <= limit else "FAIL"))
    cpu = defaultdict(list)
    for event in events:
        if event.kind == "sample" and event.name == "cpu":
            cpu[event.fields.get("context", "unspecified")].append(event)
    for context in sorted(cpu):
        samples = sorted(cpu[context], key=lambda event: event.t)
        first, last = samples[0], samples[-1]
        span_ms = last.t - first.t
        hz = float(first.fields.get("hz", "100"))
        name = "cpu.%s" % context
        if span_ms <= 0:
            rows.append(Row(name, n=len(samples), verdict="INCONCLUSIVE(one sample)"))
            continue
        busy = (int(last.fields["ticks"]) - int(first.fields["ticks"])) / hz
        percent = 100.0 * busy / (span_ms / 1000.0)
        gaps = [b.t - a.t for a, b in zip(samples, samples[1:])]
        if gaps and min(gaps) < 1000:
            verdict = "INVALID(sampled above 1 Hz)"
        elif span_ms < 600000:
            verdict = "INCONCLUSIVE(<600s)"
        elif context.startswith("background"):
            verdict = "PASS" if percent <= 8.0 else "FAIL"
        else:
            verdict = "INFO"
        rows.append(Row(name, value=percent, n=len(samples), budget="mean%<=8" if context.startswith("background") else "-",
                        verdict=verdict))
        if "wakeups" in first.fields and "wakeups" in last.fields:
            rate = (int(last.fields["wakeups"]) - int(first.fields["wakeups"])) / (span_ms / 1000.0)
            rows.append(Row("wakeups-per-s.%s" % context, value=rate, n=len(samples)))
    return rows


def speculative_rows(events):
    started = sum(1 for e in events if e.kind == "timing" and e.name == "speculative-start")
    cancelled = sum(1 for e in events if e.kind == "timing" and e.name == "speculative-cancel")
    if not started:
        return []
    waste = 100.0 * cancelled / started
    return [Row("speculative.waste", value=waste, n=started, budget="value<=30",
                verdict="PASS" if waste <= 30.0 else "FAIL")]


def summarise(paths, keep_warmup):
    events, warnings = parse(paths)
    rows, unpaired = tap_groups(events, keep_warmup)
    if unpaired:
        warnings.append("%d tap(s) without first-frame (no frame, or the log was cut)" % unpaired)
    rows += seek_rows(events, keep_warmup)
    rows += switch_rows(events, keep_warmup)
    rows += background_rows(events)
    rows += sample_rows(events)
    rows += speculative_rows(events)
    return rows, warnings


def main():
    parser = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    parser.add_argument("logs", nargs="+")
    parser.add_argument("--tsv", action="store_true", help="tab-separated output")
    parser.add_argument("--keep-warmup", action="store_true", help="do not drop the first run of a series")
    parser.add_argument("--baseline", help="E12: log of the A run (speculative resolve off)")
    args = parser.parse_args()
    try:
        rows, warnings = summarise(args.logs, args.keep_warmup)
        if args.baseline:
            base_rows, base_warnings = summarise([args.baseline], args.keep_warmup)
            warnings += ["baseline: " + warning for warning in base_warnings]
            current = {row.metric: row for row in rows}
            for base in base_rows:
                if base.metric.startswith("ttff.") and base.metric in current and base.values \
                        and current[base.metric].values:
                    saved = percentile(base.values, 0.5) - percentile(current[base.metric].values, 0.5)
                    row = Row("e12.saved-p50.%s" % base.metric[5:], value=saved,
                              n=min(len(base.values), len(current[base.metric].values)))
                    if base.metric == "ttff.fast.wifi":
                        waste = current.get("speculative.waste")
                        row.budget = "value>=150,waste<=30"
                        row.verdict = "PASS" if saved >= 150 and waste is not None and waste.verdict == "PASS" \
                            else "FAIL"
                    rows.append(row)
    except (OSError, ValueError) as error:
        sys.stderr.write("perf_summary: %s\n" % error)
        return 2
    header = ["metric", "n", "p50", "p95", "max", "value", "budget", "verdict"]
    table = [header] + [row.cells() for row in rows]
    if args.tsv:
        for cells in table:
            print("\t".join(cells))
    else:
        widths = [max(len(cells[i]) for cells in table) for i in range(len(header))]
        for cells in table:
            print("  ".join(cell.ljust(width) for cell, width in zip(cells, widths)).rstrip())
    for warning in warnings:
        sys.stderr.write("warning: %s\n" % warning)
    return 1 if any(row.verdict == "FAIL" for row in rows) else 0


if __name__ == "__main__":
    sys.exit(main())
