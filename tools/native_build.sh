#!/bin/bash
# SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
# Native (Apple Silicon, sfos-m1-native container) build loop for SailTube.
#
#   tools/native_build.sh [--jobs N] [--tests] [--check-only] [--clean]
#
# Thin adapter over the shared SailTune tool (sailtune-internal-notes/tools/native_build.sh),
# which is not edited: a temporary copy is rewritten for sailtube.pro / harbour-sailtube and
# run with this repository as the checkout. Not authoritative for RPMs (see docs/BUILDING.md).
# Exit codes as the shared tool: 0 ok, 1 build/lrelease/test problems, 2 infrastructure error.
set -u
HERE=$(cd "$(dirname "$0")/.." && pwd)
SHARED="${SAILTUNE_NATIVE_BUILD:-$HERE/../sailtune-internal-notes-e5eqvydg/tools/native_build.sh}"
[ -f "$SHARED" ] || { echo "shared native_build.sh not found: $SHARED (set SAILTUNE_NATIVE_BUILD)" >&2; exit 2; }
TMP=$(mktemp -d "${TMPDIR:-/tmp}/sailtube_native.XXXXXX"); trap 'rm -rf "$TMP"' EXIT
# Only project identity changes. The ICU/TagLib vendor flags of the shared driver are
# accepted but unused by sailtube.pro.
sed -e 's/sailtune\.pro/sailtube.pro/g' \
    -e 's/harbour-sailtune/harbour-sailtube/g' \
    -e 's/Native (non-emulated) SailTune build loop/Native SailTube build loop (generated from the SailTune tool)/' \
    "$SHARED" > "$TMP/native_build.sh"
grep -q 'sailtube.pro' "$TMP/native_build.sh" || { echo "could not adapt the shared tool" >&2; exit 2; }
bash "$TMP/native_build.sh" "$HERE" --no-icu "$@"
exit $?
