#!/bin/sh
# SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
# DL-07 offline extras (captions, SponsorBlock segments) with a fake source.
set -e
root=$(cd "$(dirname "$0")/../.." && pwd)
for m in Qt5Core Qt5Network Qt5Sql; do
    if ! pkg-config --exists $m 2>/dev/null; then echo "downloads sidecars tests: SKIPPED (no $m)"; exit 0; fi
done
out=${TMPDIR:-/tmp}/sailtube-downloads-sidecars-test.$$
mkdir -p "$out"
moc=$(pkg-config --variable=host_bins Qt5Core 2>/dev/null)/moc
[ -x "$moc" ] || moc=moc
"$moc" -I"$root" "$root/Core/Downloads/Infrastructure/DownloadEngine.h" -o "$out/moc_engine.cpp"
"$moc" -I"$root" "$root/Core/Downloads/Infrastructure/DownloadManager.h" -o "$out/moc_manager.cpp"
"$moc" -I"$root" "$root/Core/Downloads/Infrastructure/SidecarStore.h" -o "$out/moc_sidecars.cpp"
"$moc" -I"$root" "$root/Core/Library/Infrastructure/LibraryStore.h" -o "$out/moc_store.cpp"
"${CXX:-c++}" -std=c++11 -fPIC -Wall -Wextra -I"$root" \
    $(pkg-config --cflags Qt5Core Qt5Network Qt5Sql) -o "$out/t" \
    "$root/tests/downloads_sidecars/SidecarsTest.cpp" \
    "$root/Core/Downloads/Domain/FileNaming.cpp" \
    "$root/Core/Downloads/Domain/Mp4Layout.cpp" \
    "$root/Core/Downloads/Domain/DownloadPolicy.cpp" \
    "$root/Core/Downloads/Infrastructure/DownloadQueueStore.cpp" \
    "$root/Core/Downloads/Infrastructure/DownloadLocation.cpp" \
    "$root/Core/Downloads/Infrastructure/DownloadEngine.cpp" \
    "$root/Core/Downloads/Infrastructure/DownloadManager.cpp" \
    "$root/Core/Downloads/Infrastructure/SidecarStore.cpp" \
    "$root/Core/Downloads/Domain/SidecarPolicy.cpp" \
    "$out/moc_sidecars.cpp" \
    "$root/Core/Library/Infrastructure/LibraryStore.cpp" \
    "$root/Core/Library/Domain/LibraryPolicy.cpp" \
    "$root/Core/Resolver/Infrastructure/YtDlpJson.cpp" \
    "$root/Core/Media/Domain/StreamSelection.cpp" "$root/Core/Media/Domain/AudioTrackPolicy.cpp" \
    "$root/Core/Media/Domain/LivePolicy.cpp" \
    "$out/moc_engine.cpp" "$out/moc_manager.cpp" "$out/moc_store.cpp" \
    $(pkg-config --libs Qt5Core Qt5Network Qt5Sql)
TEST_DIR="$out" "$out/t"
rm -rf "$out"
