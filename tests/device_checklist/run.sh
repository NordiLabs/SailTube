#!/bin/sh
# SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
# NFR-TST-08: the device checklist (docs/DEVICE_CHECKLIST.md) passes tools/check_device_checklist.py,
# and scratch copies with one seeded violation each are rejected: a step without an Auto note, an
# area with no step, an unknown step id, a nonexistent suite, a duplicate step id. Needs python3.
set -eu
here=$(cd "$(dirname "$0")" && pwd)
root=$(cd "$here/../.." && pwd)
checker="$root/tools/check_device_checklist.py"
doc="$root/docs/DEVICE_CHECKLIST.md"
python3 -I "$checker" "$doc"
scratch=$(mktemp -d "${TMPDIR:-/tmp}/sailtube-device-checklist.XXXXXX")
trap 'rm -rf "$scratch"' EXIT

# seed MODE: writes $scratch/MODE.md, a copy of the doc with one violation.
seed() {
    python3 -I - "$1" "$doc" "$scratch/$1.md" <<'PY'
import re, sys
mode, src, dst = sys.argv[1:4]
lines = open(src, encoding="utf-8").read().split("\n")
step = re.compile(r"^- \*\*[A-Z][0-9]+\.\*\*")
steps = [i for i, l in enumerate(lines) if step.match(l)]
head = next(i for i, l in enumerate(lines) if l.startswith("#") and "change area" in l.lower())
rows = [i for i in range(head, len(lines)) if lines[i].startswith("|") and not lines[i].startswith("|---")][1:]
if mode == "no_auto":
    lines[steps[0]] = re.sub(r"\s*\*Auto:.*\*\s*$", "", lines[steps[0]])
elif mode == "empty_area":
    cs = lines[rows[0]].split("|")
    cs[-2] = " "
    lines[rows[0]] = "|".join(cs)
elif mode == "unknown_step":
    lines[rows[0]] = lines[rows[0]].rstrip().rstrip("|").rstrip() + ", Z99 |"
elif mode == "no_suite":
    i = next(i for i in steps if "tests/" in lines[i])
    lines[i] = re.sub(r"tests/[A-Za-z0-9_-]+", "tests/no_such_suite", lines[i], count=1)
elif mode == "duplicate":
    lines.insert(steps[0] + 1, lines[steps[0]])
else:
    sys.exit("unknown mode " + mode)
out = "\n".join(lines)
if out == open(src, encoding="utf-8").read():
    sys.exit("seed %s changed nothing" % mode)
open(dst, "w", encoding="utf-8").write(out)
PY
}

# expect_fail MODE TEXT: the checker must reject the MODE copy with TEXT in its output.
expect_fail() {
    seed "$1"
    if python3 -I "$checker" "$scratch/$1.md" > "$scratch/$1.out" 2>&1; then
        echo "device_checklist: FAIL (seeded '$1' was accepted)"; cat "$scratch/$1.out"; exit 1
    fi
    grep -q "$2" "$scratch/$1.out" || { echo "device_checklist: FAIL ('$1' rejected for another reason)"; cat "$scratch/$1.out"; exit 1; }
    echo "device_checklist: seeded '$1' rejected as expected"
}

expect_fail no_auto "has no \*Auto: \.\.\.\* note"
expect_fail empty_area "maps to no step"
expect_fail unknown_step "step Z99, which does not exist"
expect_fail no_suite "tests/no_such_suite/run.sh does not exist"
expect_fail duplicate "duplicate step id"
echo "device_checklist: PASS"
