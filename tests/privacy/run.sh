#!/bin/sh
# SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
# NFR-SEC-01: static network inventory. Needs only python3.
#  1. SailTune's self-test of the pinned checker on its fixture trees (selftest.py).
#  2. The inventory of this tree against tests/privacy/network_allowlist.json (strict).
#  3. The STORE run (--variant store): the FULL-only hosts are not allowed.
#  4. Scratch copies: an unlisted host must fail; a FULL-only host listed for its file must pass
#     the full run and fail the STORE run.
set -eu
here=$(cd "$(dirname "$0")" && pwd)
root=$(cd "$here/../.." && pwd)
python3 -I "$here/selftest.py"
python3 -I "$here/inventory.py" --strict
python3 -I "$here/inventory.py" --variant store
scratch=$(mktemp -d "${TMPDIR:-/tmp}/sailtube-privacy.XXXXXX")
trap 'rm -rf "$scratch"' EXIT
(cd "$root" && tar -cf - sailtube.pro harbour-sailtube.desktop rpm Core Clients Shared docs) | tar -xf - -C "$scratch"
printf '%s\n' '// scratch change' 'static const char kUnlisted[] = "https://tracker.example-unlisted.com/collect";' \
    > "$scratch/Core/ScratchUnlisted.cpp"
if python3 -I "$here/inventory.py" --root "$scratch" --strict > "$scratch/out.txt" 2>&1; then
    echo "privacy: FAIL (an unlisted host was accepted)"; exit 1
fi
grep -q "tracker.example-unlisted.com" "$scratch/out.txt" || { echo "privacy: FAIL (unexpected error text)"; cat "$scratch/out.txt"; exit 1; }
echo "privacy: unlisted host rejected as expected"
rm "$scratch/Core/ScratchUnlisted.cpp"
printf '%s\n' 'static const char kUpdate[] = "https://github.com/yt-dlp/yt-dlp/releases";' > "$scratch/Core/ScratchFull.cpp"
python3 -I - "$here/network_allowlist.json" "$scratch/allow.json" <<'PY'
import json, sys
d = json.load(open(sys.argv[1]))
d["hosts"]["github.com"]["files"].append("Core/ScratchFull.cpp")
json.dump(d, open(sys.argv[2], "w"))
PY
python3 -I "$here/inventory.py" --root "$scratch" --allowlist "$scratch/allow.json" > "$scratch/out.txt" 2>&1 \
    || { echo "privacy: FAIL (a listed FULL-only host was rejected on FULL)"; cat "$scratch/out.txt"; exit 1; }
if python3 -I "$here/inventory.py" --root "$scratch" --allowlist "$scratch/allow.json" --variant store > "$scratch/out.txt" 2>&1; then
    echo "privacy: FAIL (a FULL-only host was accepted on STORE)"; exit 1
fi
grep -q "github.com" "$scratch/out.txt" || { echo "privacy: FAIL (unexpected STORE error text)"; cat "$scratch/out.txt"; exit 1; }
echo "privacy: FULL-only host accepted on FULL, rejected on STORE as expected"
echo "privacy: PASS"
