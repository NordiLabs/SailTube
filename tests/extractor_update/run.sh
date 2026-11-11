#!/bin/sh
# SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
# [FULL] NFR-MNT-01 / NFR-SEC-04: extractor updates.
#  1. python3 (Mac and Linux): unit tests of Core/Extractor/Runtime/verify_release.py and of worker.py's
#     verify task, including the real 2026.10.26 SHA2-256SUMS + .sig against the pinned release key.
#  2. Linux + Qt5Core/Qt5Network: Domain/ReleasePolicy and Infrastructure/ExtractorUpdater against a fake
#     HTTPS release host (tests/extractor_update/ExtractorUpdateTest.cpp). Skipped elsewhere.
# The real bundled archive (XUPD_REAL_ARCHIVE, or the build container's runtime stage) adds the
# end-to-end install of the real 2026.10.26 release through the worker's verify task.
# Fixtures: tests/extractor_update/make_fixtures.sh (needs gpg; run by hand, results committed).
set -eu
here=$(cd "$(dirname "$0")" && pwd)
root=$(cd "$here/../.." && pwd)
py=$(command -v python3 || true)
if [ -z "$py" ] || ! "$py" -c 'import sys; sys.exit(0 if sys.version_info >= (3, 9) else 1)'; then
    echo "extractor_update: SKIPPED (need python3 >= 3.9)"; echo "extractor_update: PASS"; exit 0
fi
real=${XUPD_REAL_ARCHIVE:-}
[ -z "$real" ] && [ -s /builds/_vendor/python-runtime/runtime/yt-dlp.pyz ] && real=/builds/_vendor/python-runtime/runtime/yt-dlp.pyz
XUPD_REAL_ARCHIVE="$real" "$py" -I "$here/test_verify_release.py" -v \
    || { echo "extractor_update: FAIL (python unit tests)"; exit 1; }
echo "extractor_update: python unit tests passed"

if [ "$(uname -s)" != Linux ] || ! pkg-config --exists Qt5Core Qt5Network 2>/dev/null; then
    echo "extractor_update: host tests SKIPPED (need Linux, Qt5Core and Qt5Network)"; echo "extractor_update: PASS"; exit 0
fi
out=$(mktemp -d "${TMPDIR:-/tmp}/sailtube-xupd.XXXXXX")
trap 'rm -rf "$out"' EXIT
moc=$(pkg-config --variable=host_bins Qt5Core 2>/dev/null)/moc
[ -x "$moc" ] || moc=moc
"$moc" -I"$root" "$root/Core/Extractor/Infrastructure/ExtractorUpdater.h" -o "$out/moc_ExtractorUpdater.cpp"
"$moc" -I"$root" "$root/Core/Process/Infrastructure/HelperProcess.h" -o "$out/moc_HelperProcess.cpp"
"${CXX:-c++}" -std=c++11 -fPIC -Wall -Wextra -pthread -I"$root" -I"$root/Shared/SailTune/tests/sharedplayback" \
    $(pkg-config --cflags Qt5Core Qt5Network) -o "$out/t" \
    "$here/ExtractorUpdateTest.cpp" "$root/Core/Extractor/Domain/ReleasePolicy.cpp" \
    "$root/Core/Extractor/Infrastructure/ExtractorUpdater.cpp" "$root/Core/Process/Infrastructure/HelperProcess.cpp" \
    "$out/moc_ExtractorUpdater.cpp" "$out/moc_HelperProcess.cpp" $(pkg-config --libs Qt5Core Qt5Network)
XUPD_ROOT="$root" XUPD_PYTHON="$py" XUPD_REAL_ARCHIVE="$real" QT_QPA_PLATFORM=minimal timeout -s KILL 600 "$out/t"
echo "extractor_update: PASS"
