#!/bin/sh
# Shared playback component: Qt-free domain checks, then the parser and the fast path against a
# local TLS server playing the provider (Qt5Core + Qt5Network, moc and a C++11 compiler).
set -e
dir=$(cd "$(dirname "$0")" && pwd)
root=$(cd "$dir/../.." && pwd)
out=${TMPDIR:-/tmp}/sailtune-sharedplayback-tests
mkdir -p "$out"
"${CXX:-c++}" -std=c++11 -Wall -Wextra -o "$out/domain" "$dir/DomainTest.cpp"
"$out/domain"
if ! command -v pkg-config >/dev/null 2>&1 || ! pkg-config --exists Qt5Core Qt5Network 2>/dev/null; then
    echo "sharedplayback: Qt5Core/Qt5Network not available, fast path tests skipped"; exit 0
fi
moc=$(pkg-config --variable=host_bins Qt5Core 2>/dev/null)/moc
[ -x "$moc" ] || moc=$(command -v moc || command -v moc-qt5)
infra="$root/Core/SharedPlayback/Infrastructure"
"$moc" "$infra/HttpsConnection.h" -o "$out/moc_HttpsConnection.cpp"
"$moc" "$infra/FastStreamResolver.h" -o "$out/moc_FastStreamResolver.cpp"
"${CXX:-c++}" -std=c++11 -fPIC -Wall -Wextra -Wno-deprecated-copy $(pkg-config --cflags Qt5Core Qt5Network) \
    -DFIXTURE_DIR="\"$dir/fixtures\"" -o "$out/fast" "$dir/FastResolverTest.cpp" \
    "$infra/HttpsConnection.cpp" "$infra/FastStreamResolver.cpp" "$infra/PlayerResponseParser.cpp" \
    "$out/moc_HttpsConnection.cpp" "$out/moc_FastStreamResolver.cpp" $(pkg-config --libs Qt5Core Qt5Network)
QT_QPA_PLATFORM=minimal timeout -s KILL 120 "$out/fast" 2>&1 || { echo "sharedplayback: FAIL"; exit 1; }
# The loopback media proxy against a local TLS media host.
"${CXX:-c++}" -std=c++11 -fPIC -Wall -Wextra -Wno-deprecated-copy $(pkg-config --cflags Qt5Core Qt5Network) \
    -DFIXTURE_DIR="\"$dir/fixtures\"" -o "$out/proxy" "$dir/MediaProxyTest.cpp" \
    "$infra/MediaProxy.cpp" "$infra/HttpsConnection.cpp" "$out/moc_HttpsConnection.cpp" $(pkg-config --libs Qt5Core Qt5Network)
QT_QPA_PLATFORM=minimal timeout -s KILL 180 "$out/proxy" 2>&1 || { echo "sharedplayback: media proxy FAIL"; exit 1; }
