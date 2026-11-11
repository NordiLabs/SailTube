#!/bin/sh
# SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
# NFR-MNT-03 release hygiene: self-test of tools/release_check.py. Needs only python3; the tag
# cases also need git (skipped without it). Runs on a tree without .git and on the Mac.
#  1. `release_check.py version` on this tree (spec Version == sailtube.pro VERSION).
#  2. selftest.py: version mismatch in a scratch copy, tag checks in a temp git repo, payload
#     comparison of synthetic newc archives and synthetic RPMs.
set -eu
here=$(cd "$(dirname "$0")" && pwd)
root=$(cd "$here/../.." && pwd)
scratch=$(mktemp -d "${TMPDIR:-/tmp}/sailtube-release.XXXXXX")
trap 'rm -rf "$scratch"' EXIT
version=$(python3 -I "$root/tools/release_check.py" version) || { echo "release: FAIL (version check)"; exit 1; }
echo "release: single-source version $version"
python3 -I "$here/selftest.py" "$scratch" || { echo "release: FAIL"; exit 1; }
echo "release: PASS"
