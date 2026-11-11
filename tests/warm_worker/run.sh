#!/bin/sh
# SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
# Warm resolver worker (RES-01, NFR-ROB-03/04): the C++ client and Core/Worker/Runtime/worker.py over a
# fake yt-dlp archive; with the real archive present (the build container's runtime stage, or
# WARM_REAL_ARCHIVE), also the measured cold/warm cost. Linux + Qt5Core + python3 >= 3.11.
set -e
root=$(cd "$(dirname "$0")/../.." && pwd)
py=$(command -v python3 || true)
if [ "$(uname -s)" != Linux ] || ! pkg-config --exists Qt5Core 2>/dev/null || [ -z "$py" ] || \
   ! "$py" -c 'import sys; sys.exit(0 if sys.version_info >= (3, 11) else 1)'; then
    echo "warm_worker: SKIPPED (need Linux, Qt5Core and python3 >= 3.11)"; echo "warm_worker: PASS"; exit 0
fi
out=$(mktemp -d "${TMPDIR:-/tmp}/sailtube-warm-worker.XXXXXX")
trap 'rm -rf "$out"' EXIT
mkdir -p "$out/runtime"
cp "$root/Core/Worker/Runtime/worker.py" "$root/Shared/Runtime/Python/resolver.py" "$root/tests/warm_worker/make_fake_archive.py" "$out/runtime/"
moc=$(pkg-config --variable=host_bins Qt5Core 2>/dev/null)/moc
[ -x "$moc" ] || moc=moc
"$moc" -I"$root" "$root/Core/Worker/Infrastructure/WarmResolver.h" -o "$out/moc_WarmResolver.cpp"
"$moc" -I"$root" "$root/Core/Process/Infrastructure/HelperProcess.h" -o "$out/moc_HelperProcess.cpp"
"${CXX:-c++}" -std=c++11 -fPIC -Wall -Wextra -pthread -I"$root" $(pkg-config --cflags Qt5Core) -o "$out/t" \
    "$root/tests/warm_worker/WarmResolverTest.cpp" "$root/Core/Worker/Infrastructure/WarmResolver.cpp" \
    "$root/Core/Process/Infrastructure/HelperProcess.cpp" "$out/moc_WarmResolver.cpp" "$out/moc_HelperProcess.cpp" \
    $(pkg-config --libs Qt5Core)
real=${WARM_REAL_ARCHIVE:-}
[ -z "$real" ] && [ -s /builds/_vendor/python-runtime/runtime/yt-dlp.pyz ] && real=/builds/_vendor/python-runtime/runtime/yt-dlp.pyz
WARM_RUNTIME="$out/runtime" WARM_PYTHON="$py" WARM_REAL_ARCHIVE="$real" QT_QPA_PLATFORM=minimal timeout -s KILL 240 "$out/t"
echo "warm_worker: PASS"
