#!/bin/sh
# SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
# Time-limited beta (docs/BETA.md): the date gate, the static "compiled out of FULL/STORE" check, and the
# policy + clock tests with an injected clock in two timezones (Qt5Core; skipped where it is absent).
set -e
here=$(cd "$(dirname "$0")" && pwd)
root=$(cd "$here/../.." && pwd)
gate="$root/tools/check_beta_date.py"
ok()   { python3 -I "$gate" "$@" >/dev/null || { echo "beta: FAIL (date gate refused $*)"; exit 1; }; }
bad()  { if python3 -I "$gate" "$@" >/dev/null; then echo "beta: FAIL (date gate accepted $*)"; exit 1; fi; }
ok 2026-12-30 --today 2026-11-08
ok 2026-11-08 --today 2026-11-08
bad 2026-12-31 --today 2026-11-08
bad 2026-11-07 --today 2026-11-08
bad 2026-02-30 --today 2026-01-01
bad "30-12-2026" --today 2026-11-08
bad "" --today 2026-11-08
grep -q 'SAILTUBE_BETA_EXPIRES=%{beta_expires}' "$root/rpm/harbour-sailtube.spec" || { echo "beta: FAIL (spec does not pass the expiry)"; exit 1; }
grep -q '^%global beta_expires 2026-12-30$' "$root/rpm/harbour-sailtube.spec" || { echo "beta: FAIL (spec expiry is not 2026-12-30)"; exit 1; }
grep -q 'error:The beta profile is FULL only' "$root/rpm/harbour-sailtube.spec" || { echo "beta: FAIL (spec allows a STORE beta)"; exit 1; }
echo "beta: date gate and spec checks passed"
# Normal and STORE builds contain no beta code path: the sources are only in the sailtube_beta block and the
# composition code only under SAILTUBE_BETA.
python3 -I - "$root" <<'PY'
import re, sys, os
root = sys.argv[1]
pro = open(os.path.join(root, "sailtube.pro")).read()
block = re.search(r"\nsailtube_beta \{(.*?)\n\}", pro, re.S)
assert block, "no sailtube_beta block"
outside = pro[:block.start()] + pro[block.end():]
for f in ("BetaClock.cpp", "BetaStatus.cpp", "BetaClock.h", "BetaStatus.h", "BetaPolicy.h"):
    assert f in block.group(1), f + " not in the beta block"
    assert f not in outside, f + " compiled outside the beta block"
main = open(os.path.join(root, "Clients/Sailfish/Composition/main.cpp")).read()
depth, guarded = 0, []
for line in main.splitlines():
    s = line.strip()
    if s.startswith("#ifdef SAILTUBE_BETA"): depth += 1; continue
    if depth and s.startswith(("#if", "#ifdef", "#ifndef")): depth += 1; continue
    if depth and s.startswith("#endif"): depth -= 1; continue
    if not depth and re.search(r"\b(BetaStatus|BetaClock|runBetaEnded|SAILTUBE_BETA_)", line):
        raise SystemExit("beta code outside #ifdef SAILTUBE_BETA: " + s)
print("beta: compiled out of FULL and STORE (sources and composition)")
PY
if ! pkg-config --exists Qt5Core 2>/dev/null; then echo "beta: clock tests SKIPPED (no Qt5Core)"; echo "beta: PASS"; exit 0; fi
out=$(mktemp -d "${TMPDIR:-/tmp}/sailtube-beta.XXXXXX")
trap 'rm -rf "$out"' EXIT
"${CXX:-c++}" -std=c++11 -fPIC -Wall -Wextra -I"$root" $(pkg-config --cflags Qt5Core) -o "$out/t" \
    "$here/BetaTest.cpp" "$root/Core/Beta/Infrastructure/BetaClock.cpp" $(pkg-config --libs Qt5Core)
for tz in UTC America/Mexico_City Pacific/Kiritimati; do TZ=$tz "$out/t" | sed "s|^|[$tz] |"; done
echo "beta: PASS"
