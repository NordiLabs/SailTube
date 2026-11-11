#!/bin/sh
# SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
# PLT-14 process discipline: static check (no qSetMessagePattern in shipped code), then the
# Linux host tests of HelperProcess/HelperHost (Qt5Core; about 12 s).
set -e
root=$(cd "$(dirname "$0")/../.." && pwd)
if grep -rnE --include=*.cpp --include=*.h "qSetMessagePattern[[:space:]]*\(" "$root/Core" "$root/Clients" "$root/Shared/Runtime" "$root/Shared/Playback" 2>/dev/null; then
    echo "process: FAIL (qSetMessagePattern in shipped code; set QT_MESSAGE_PATTERN in the environment instead)"; exit 1
fi
if [ "$(uname -s)" != Linux ] || ! pkg-config --exists Qt5Core 2>/dev/null; then
    echo "process: host tests SKIPPED (need Linux and Qt5Core)"; echo "process: PASS"; exit 0
fi
out=$(mktemp -d "${TMPDIR:-/tmp}/sailtube-process.XXXXXX")
trap 'rm -rf "$out"' EXIT
moc=$(pkg-config --variable=host_bins Qt5Core 2>/dev/null)/moc
[ -x "$moc" ] || moc=moc
"$moc" "$root/Core/Process/Infrastructure/HelperProcess.h" -o "$out/moc_HelperProcess.cpp"
"${CXX:-c++}" -std=c++11 -fPIC -Wall -Wextra -pthread $(pkg-config --cflags Qt5Core) -I"$root/Core/Process/Infrastructure" -o "$out/t" \
    "$root/tests/process/ProcessTest.cpp" "$root/Core/Process/Infrastructure/HelperProcess.cpp" "$out/moc_HelperProcess.cpp" \
    $(pkg-config --libs Qt5Core)
QT_QPA_PLATFORM=minimal timeout -s KILL 90 "$out/t"
echo "process: PASS"
