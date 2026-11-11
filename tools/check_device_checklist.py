#!/usr/bin/env python3
# SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
"""Check the device checklist (NFR-TST-08).

    tools/check_device_checklist.py [FILE] [--root DIR]

FILE defaults to docs/DEVICE_CHECKLIST.md; DIR (where tests/<suite>/run.sh is looked up) defaults
to the repository that holds this script. Step lines look like

    - **P3.** [smoke] What to do and what to see (PLY-03). *Auto: tests/domain (mp4 index).*

Fails (exit 1, one line per problem) when:
  * a step line has no trailing `*Auto: ...*` note, or the note neither says `none` nor names a
    suite, or says `none` and names a suite as well;
  * a note names `tests/<x>` and `tests/<x>/run.sh` does not exist under DIR;
  * two step lines carry the same id;
  * the change table ("Change area" heading, column "Steps") is missing, has an area with no step,
    or names a step id that is not a step line in FILE;
  * there are no steps or no [smoke] steps.
Python 3.9 stdlib only.
"""
import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
DEFAULT_DOC = os.path.join("docs", "DEVICE_CHECKLIST.md")

STEP = re.compile(r"^\s*[-*]\s+\*\*([A-Z][0-9]+)\.\*\*\s*(.*)$")
AUTO = re.compile(r"\*Auto:\s*(.*?)\*\s*$")
SUITE = re.compile(r"\btests/([A-Za-z0-9_-]+)")
STEP_ID = re.compile(r"^[A-Z][0-9]+$")


def cells(line):
    return [c.strip() for c in line.strip().strip("|").split("|")]


def parse_steps(lines, errors):
    steps = {}
    smoke = []
    for no, line in enumerate(lines, 1):
        m = STEP.match(line)
        if not m:
            continue
        sid, rest = m.group(1), m.group(2)
        if sid in steps:
            errors.append("line %d: duplicate step id %s (first on line %d)" % (no, sid, steps[sid]["line"]))
            continue
        auto = AUTO.search(rest)
        note = auto.group(1).strip() if auto else None
        steps[sid] = {"line": no, "note": note}
        if rest.startswith("[smoke]"):
            smoke.append(sid)
        if note is None:
            errors.append("line %d: step %s has no *Auto: ...* note at the end of its line" % (no, sid))
            continue
        suites = SUITE.findall(note)
        says_none = re.match(r"none\b", note, re.I) is not None
        if says_none and suites:
            errors.append("line %d: step %s says Auto: none but also names %s" % (no, sid, ", ".join("tests/" + s for s in suites)))
        elif not says_none and not suites:
            errors.append("line %d: step %s Auto note names no tests/<suite> and does not say none" % (no, sid))
        steps[sid]["suites"] = suites
    return steps, smoke


def parse_change_table(lines, errors):
    """Return [(line, area, [ids])] from the first table under a 'Change area' heading."""
    start = None
    for i, line in enumerate(lines):
        if line.startswith("#") and "change area" in line.lower():
            start = i + 1
            break
    if start is None:
        errors.append("no 'Change area' heading with the change-to-section table")
        return []
    header = None
    rows = []
    for i in range(start, len(lines)):
        line = lines[i]
        if line.startswith("#"):
            break
        if not line.lstrip().startswith("|"):
            if header is not None:
                break
            continue
        cs = cells(line)
        if header is None:
            header = [c.lower() for c in cs]
            if "steps" not in header:
                errors.append("line %d: change table has no 'Steps' column" % (i + 1))
                return []
            col = header.index("steps")
            continue
        if all(re.match(r"^:?-+:?$", c) for c in cs if c):
            continue
        area = cs[0] if cs else ""
        raw = cs[col] if col < len(cs) else ""
        ids = [t for t in re.split(r"[,\s]+", raw) if t]
        rows.append((i + 1, area, ids))
    if header is None:
        errors.append("the 'Change area' section has no table")
    elif not rows:
        errors.append("the change table has no areas")
    return rows


def check(path, root):
    try:
        with open(path, encoding="utf-8") as f:
            lines = f.read().splitlines()
    except OSError as e:
        return ["cannot read %s: %s" % (path, e)], None
    errors = []
    steps, smoke = parse_steps(lines, errors)
    if not steps:
        errors.append("no step lines ('- **<ID>.** ...') found")
    elif not smoke:
        errors.append("no [smoke] steps: the smoke subset is empty")
    missing_suites = {}
    for sid, s in sorted(steps.items(), key=lambda kv: kv[1]["line"]):
        for suite in s.get("suites", []):
            if not os.path.isfile(os.path.join(root, "tests", suite, "run.sh")):
                missing_suites.setdefault(suite, []).append(sid)
    for suite, sids in sorted(missing_suites.items()):
        errors.append("tests/%s/run.sh does not exist (named by %s)" % (suite, ", ".join(sids)))
    for no, area, ids in parse_change_table(lines, errors):
        if not area:
            errors.append("line %d: change table row without an area name" % no)
        if not ids:
            errors.append("line %d: area '%s' maps to no step" % (no, area))
        for t in ids:
            if not STEP_ID.match(t):
                errors.append("line %d: area '%s' names '%s', which is not a step id" % (no, area, t))
            elif t not in steps:
                errors.append("line %d: area '%s' names step %s, which does not exist" % (no, area, t))
    return errors, (steps, smoke)


def main(argv):
    args = list(argv)
    root = ROOT
    if "--root" in args:
        i = args.index("--root")
        if i + 1 >= len(args):
            print("usage: check_device_checklist.py [FILE] [--root DIR]", file=sys.stderr)
            return 2
        root = os.path.abspath(args[i + 1])
        del args[i:i + 2]
    if len(args) > 1 or (args and args[0].startswith("-")):
        print("usage: check_device_checklist.py [FILE] [--root DIR]", file=sys.stderr)
        return 2
    path = args[0] if args else os.path.join(root, DEFAULT_DOC)
    errors, parsed = check(path, root)
    if errors:
        for e in errors:
            print("check_device_checklist: %s: %s" % (path, e))
        print("check_device_checklist: FAIL (%d problem%s)" % (len(errors), "" if len(errors) == 1 else "s"))
        return 1
    steps, smoke = parsed
    covered = sum(1 for s in steps.values() if s.get("suites"))
    print("check_device_checklist: OK: %d steps (%d with suites, %d none), smoke %d: %s"
          % (len(steps), covered, len(steps) - covered, len(smoke), ", ".join(smoke)))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
