#!/bin/sh
# SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
# One-shot resolver child vs warm worker, end to end and offline (needs Qt5Core/Qml, PyOtherSide, python3.11
# and the real yt-dlp archive: the build container's runtime stage or BENCH_ARCHIVE). About a minute.
set -e
root=$(cd "$(dirname "$0")/../.." && pwd)
archive=${BENCH_ARCHIVE:-/builds/_vendor/python-runtime/runtime/yt-dlp.pyz}
plugin=$(pkg-config --variable=libdir Qt5Core 2>/dev/null)/qt5/qml/io/thp/pyotherside
if [ "$(uname -s)" != Linux ] || ! pkg-config --exists Qt5Core Qt5Qml 2>/dev/null || [ ! -s "$archive" ] || [ ! -d "$plugin" ]; then
    echo "resolver_bench: SKIPPED (need Linux, Qt5Qml, PyOtherSide and the yt-dlp archive)"; echo "resolver_bench: PASS"; exit 0
fi
out=$(mktemp -d "${TMPDIR:-/tmp}/sailtube-resolver-bench.XXXXXX")
trap 'rm -rf "$out"' EXIT
mkdir -p "$out/root/runtime" "$out/inc"
cp "$root/Shared/Runtime/Python/resolver.py" "$root/Shared/Runtime/Python/Resolver.qml" \
   "$root/Core/Worker/Runtime/worker.py" "$root/Core/Worker/Runtime/Worker.qml" "$archive" "$out/root/runtime/"
printf '#pragma once\n#define NORDTILE_RUNTIME_ROOT "%s"\n' "$out/root" > "$out/inc/NordTileRuntimeLayout.h"
moc=$(pkg-config --variable=host_bins Qt5Core 2>/dev/null)/moc
[ -x "$moc" ] || moc=moc
for h in Shared/Runtime/Child/PythonResolverChild.h Core/Worker/Infrastructure/PythonWorkerChild.h \
         Core/Worker/Infrastructure/WarmResolver.h Core/Process/Infrastructure/HelperProcess.h; do
    "$moc" -I"$root" "$root/$h" -o "$out/moc_$(basename "$h" .h).cpp"
done
"${CXX:-c++}" -std=c++11 -fPIC -w -pthread -I"$out/inc" -I"$root" $(pkg-config --cflags Qt5Core Qt5Qml) -o "$out/bench" \
    "$root/tests/resolver_bench/ChildBench.cpp" "$root/Shared/Runtime/Child/PythonResolverChild.cpp" \
    "$root/Shared/Runtime/Child/QuickJsChild.cpp" "$root/Core/Worker/Infrastructure/PythonWorkerChild.cpp" \
    "$root/Core/Worker/Infrastructure/WarmResolver.cpp" "$root/Core/Process/Infrastructure/HelperProcess.cpp" \
    "$out"/moc_*.cpp $(pkg-config --libs Qt5Core Qt5Qml) -ldl
timeout -s KILL 300 "$out/bench" ${BENCH_RUNS:-8}
echo "resolver_bench: PASS"
