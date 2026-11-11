#!/bin/sh
# SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
# NFR-SEC-05 download safety (docs/FAULT_INJECTION.md). Acceptance: the DL-05 name tests, the
# NFR-SEC-01 inventory test and (FULL) the RES-14 bind check pass. Steps:
#   1. the DL-05 name tests of the library component (tests/downloads_domain, run as they are);
#   2. path, size, type and host checks against the real DownloadEngine (DownloadSafetyTest.cpp,
#      Qt5Core + Qt5Network + Qt5Sql, local host on 127.0.0.1);
#   3. a static probe for a free-space check before a transfer;
#   4. the NFR-SEC-01 network inventory (tests/privacy, run as it is);
#   5. RES-14 bind check: not applicable yet (no PO-token provider exists).
# POSIX sh, run from anywhere. Prints one "CASE <name>: PASS|FAIL|GAP|N/A" line per case.
set -e
here=$(cd "$(dirname "$0")" && pwd)
root=$(cd "$here/../.." && pwd)
out=$(mktemp -d "${TMPDIR:-/tmp}/sailtube-dlsafety.XXXXXX")
trap 'rm -rf "$out"' EXIT

# ---- 1. DL-05 names (library component's suite, unchanged) --------------------------------------
TMPDIR="$out" sh "$root/tests/downloads_domain/run.sh" > "$out/domain.log" 2>&1 || { cat "$out/domain.log"; echo "CASE dl05_name_tests: FAIL"; exit 1; }
grep -q "downloads domain tests: PASS" "$out/domain.log" || { cat "$out/domain.log"; echo "CASE dl05_name_tests: FAIL"; exit 1; }
echo "CASE dl05_name_tests: PASS (tests/downloads_domain naming(): traversal, separators, NUL/controls, bidi, FAT-reserved, leading dot/dash, reserved names, 255-byte limit, UTF-8 cuts, id and extension allowlists)"

# ---- 2. engine checks -------------------------------------------------------------------------
engine=skipped
for m in Qt5Core Qt5Network Qt5Sql; do
    if ! command -v pkg-config >/dev/null 2>&1 || ! pkg-config --exists $m 2>/dev/null; then engine="no $m"; fi
done
if [ "$engine" = skipped ]; then
    moc=$(pkg-config --variable=host_bins Qt5Core 2>/dev/null)/moc
    [ -x "$moc" ] || moc=$(command -v moc || command -v moc-qt5)
    "$moc" -I"$root" "$root/Core/Downloads/Infrastructure/DownloadEngine.h" -o "$out/moc_engine.cpp"
    # shellcheck disable=SC2046
    "${CXX:-c++}" -std=c++11 -fPIC -Wall -Wextra -I"$root" \
        $(pkg-config --cflags Qt5Core Qt5Network Qt5Sql | sed "s/-I/-isystem /g") -o "$out/t" \
        "$here/DownloadSafetyTest.cpp" \
        "$root/Core/Downloads/Domain/FileNaming.cpp" "$root/Core/Downloads/Domain/Mp4Layout.cpp" \
        "$root/Core/Downloads/Domain/DownloadPolicy.cpp" \
        "$root/Core/Downloads/Infrastructure/DownloadQueueStore.cpp" \
        "$root/Core/Downloads/Infrastructure/DownloadEngine.cpp" \
        "$root/Core/Resolver/Infrastructure/YtDlpJson.cpp" \
        "$root/Core/Media/Domain/StreamSelection.cpp" "$root/Core/Media/Domain/AudioTrackPolicy.cpp" \
        "$root/Core/Media/Domain/LivePolicy.cpp" \
        "$out/moc_engine.cpp" \
        $(pkg-config --libs Qt5Core Qt5Network Qt5Sql) -lpthread
    TMPDIR="$out" QT_QPA_PLATFORM=minimal timeout -s KILL 240 "$out/t"
else
    echo "download_safety: engine checks SKIPPED ($engine)"
fi

# ---- 3. free-space precheck (static probe) ---------------------------------------------------
if grep -rqE "QStorageInfo|bytesAvailable|statvfs|freeSpace" "$root/Core/Downloads"; then
    echo "CASE size_free_space_precheck: PRESENT (a free-space check exists in Core/Downloads; add a behavioural case)"
else
    echo "CASE size_free_space_precheck: GAP (Core/Downloads checks no free space before a transfer; a full disk ends as a write failure)"
fi

# ---- 4. NFR-SEC-01 inventory (unchanged) ----------------------------------------------------
if command -v python3 >/dev/null 2>&1; then
    TMPDIR="$out" sh "$root/tests/privacy/run.sh" > "$out/privacy.log" 2>&1 || { cat "$out/privacy.log"; echo "CASE nfr_sec_01_inventory: FAIL"; exit 1; }
    tail -n 1 "$out/privacy.log" | grep -q "privacy: PASS" || { cat "$out/privacy.log"; echo "CASE nfr_sec_01_inventory: FAIL"; exit 1; }
    echo "CASE nfr_sec_01_inventory: PASS (tests/privacy: strict FULL and STORE runs, unlisted-host rejection)"
else
    echo "CASE nfr_sec_01_inventory: SKIPPED (no python3)"
fi

# ---- 5. RES-14 ------------------------------------------------------------------------------
# RES-14 bind check: the PO-token provider (playback, FULL) refuses a helper that listens outside
# 127.0.0.1; its own suite tests/potoken exercises that, so it is required to exist here.
if [ -f "$root/tests/potoken/run.sh" ] && grep -q -i -E "listen|loopback" "$root/tests/potoken/PoTokenQtTest.cpp" "$root/tests/potoken/PoTokenTest.cpp" 2>/dev/null; then
    echo "CASE res14_bind_check: COVERED (tests/potoken: the helper's listening sockets are checked against 127.0.0.1)"
else
    echo "CASE res14_bind_check: FAIL (the PO-token provider exists but tests/potoken no longer checks the loopback bind)"; exit 1
fi

echo "download_safety: PASS"
