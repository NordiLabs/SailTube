#!/bin/sh
# SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
# Qt-free download domain tests (DL-01..DL-08 decisions); needs only a C++11 compiler.
set -e
root=$(cd "$(dirname "$0")/../.." && pwd)
out=${TMPDIR:-/tmp}/sailtube-downloads-domain-test.$$
"${CXX:-c++}" -std=c++11 -Wall -Wextra -I"$root" -o "$out" "$root/tests/downloads_domain/DownloadsDomainTest.cpp" \
    "$root/Core/Downloads/Domain/FileNaming.cpp" "$root/Core/Downloads/Domain/DownloadPolicy.cpp" \
    "$root/Core/Downloads/Domain/Mp4Layout.cpp" \
    "$root/Core/Media/Domain/StreamSelection.cpp" "$root/Core/Media/Domain/AudioTrackPolicy.cpp" \
    "$root/Core/Media/Domain/LivePolicy.cpp"
"$out"
rm -f "$out"
