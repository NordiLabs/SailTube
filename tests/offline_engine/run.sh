#!/bin/sh
# SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
# NFR-TST-02: the offline engine suite with networking disabled. The container may not create an
# empty network namespace (unshare is refused), so every fetcher test runs under no_network.so, an
# LD_PRELOAD guard that refuses connect/sendto/sendmsg to anything but loopback or a local socket and
# any lookup of a non-local name (docs/BUILDING.md, "Offline engine suite").
#   1. build the guard and prove it: probe.c's connect to 1.1.1.1:443 must be refused;
#   2. under the guard: SailTune's pinned shared playback tests (loopback media proxy against a local
#      TLS media host, run unchanged from the pin), the download engine suite, and OfflineEngineTest
#      (the six named cases against the real download engine and a fake googlevideo on 127.0.0.1);
#   3. mutation: with the 416 handling removed from a scratch copy of MediaProxy.cpp and of
#      DownloadPolicy.cpp the rebuilt tests must FAIL;
#   4. no blocked network attempt during the runs; print the case table.
# Skipped (still PASS) on non-Linux hosts or without cc / c++ / Qt5Core / Qt5Network / Qt5Sql.
set -eu
here=$(cd "$(dirname "$0")" && pwd)
root=$(cd "$here/../.." && pwd)
pin="$root/Shared/SailTune"
infra="$pin/Core/SharedPlayback/Infrastructure"

skip() { echo "offline_engine: SKIPPED ($1)"; echo "offline_engine: PASS"; exit 0; }
fail() { echo "offline_engine: $1" >&2; echo "offline_engine: FAIL"; exit 1; }

[ "$(uname -s)" = Linux ] || skip "not Linux"
command -v cc >/dev/null 2>&1 || skip "no cc"
CXX=${CXX:-c++}
command -v "$CXX" >/dev/null 2>&1 || skip "no $CXX"
command -v pkg-config >/dev/null 2>&1 || skip "no pkg-config"
for m in Qt5Core Qt5Network Qt5Sql; do pkg-config --exists $m 2>/dev/null || skip "no $m"; done
moc=$(pkg-config --variable=host_bins Qt5Core 2>/dev/null)/moc
[ -x "$moc" ] || moc=$(command -v moc || command -v moc-qt5 || true)
[ -n "$moc" ] && [ -x "$moc" ] || skip "no moc"
command -v timeout >/dev/null 2>&1 || skip "no timeout"

out=$(mktemp -d "${TMPDIR:-/tmp}/sailtube-offline-engine.XXXXXX")
trap 'rm -rf "$out"' EXIT
trap 'exit 1' INT TERM
mkdir -p "$out/tmp" "$out/obj" "$out/mut"
export TMPDIR="$out/tmp"
QT_QPA_PLATFORM=minimal
OFFLINE_ENGINE_REQUIRE_GUARD=1
export QT_QPA_PLATFORM OFFLINE_ENGINE_REQUIRE_GUARD

# ---- 1. the guard and its proof ----------------------------------------------------------------
cc -shared -fPIC -O2 -Wall -Wextra -o "$out/no_network.so" "$here/no_network.c" -ldl
cc -shared -fPIC -O2 -Wall -Wextra -o "$out/enospc.so" "$here/enospc.c" -ldl
cc -O2 -Wall -Wextra -o "$out/probe" "$here/probe.c" -ldl
guard() { LD_PRELOAD="$out/no_network.so${LD_PRELOAD:+:$LD_PRELOAD}" "$@"; }
echo "offline_engine: guard proof (LD_PRELOAD=no_network.so)"
NO_NETWORK_LOG="$out/probe.log" guard "$out/probe" 2>&1 || fail "the network guard does not refuse a public address"
[ -s "$out/probe.log" ] || fail "the guard logged no blocked attempt during the probe"

# Every test below logs refusals here; it must stay empty.
NO_NETWORK_LOG="$out/tests.log"
export NO_NETWORK_LOG
: > "$NO_NETWORK_LOG"

# ---- 2. the fetcher tests under the guard ------------------------------------------------------
cxxflags="-std=c++11 -fPIC -Wall -Wextra -Wno-deprecated-copy"
qt=$(pkg-config --cflags Qt5Core Qt5Network Qt5Sql)
qtlibs=$(pkg-config --libs Qt5Core Qt5Network Qt5Sql)

echo "offline_engine: SailTune shared playback tests (pinned, unchanged) under the guard"
guard sh "$pin/tests/sharedplayback/run.sh" > "$out/sharedplayback.txt" 2>&1 || { cat "$out/sharedplayback.txt"; fail "SailTune shared playback tests failed"; }
grep -E 'MediaProxyTest: [0-9]+ checks passed' "$out/sharedplayback.txt" || { cat "$out/sharedplayback.txt"; fail "the media proxy test did not run"; }

echo "offline_engine: download engine suite under the guard"
guard sh "$root/tests/downloads_engine/run.sh" > "$out/downloads.txt" 2>&1 || { cat "$out/downloads.txt"; fail "the download engine suite failed"; }
grep -q 'downloads engine tests: SKIPPED' "$out/downloads.txt" && { cat "$out/downloads.txt"; skip "download engine suite skipped itself"; }
grep 'downloads engine tests: PASS' "$out/downloads.txt" || { cat "$out/downloads.txt"; fail "the download engine suite did not pass"; }

echo "offline_engine: building OfflineEngineTest"
"$moc" -I"$root" "$root/Core/Downloads/Infrastructure/DownloadEngine.h" -o "$out/moc_engine.cpp"
"$moc" -I"$root" "$root/Core/Downloads/Infrastructure/DownloadManager.h" -o "$out/moc_manager.cpp"
"$moc" -I"$root" "$root/Core/Downloads/Infrastructure/SidecarStore.h" -o "$out/moc_sidecars.cpp"
# Everything but DownloadPolicy.cpp is compiled once and linked with the real and the mutated policy.
sources="$here/OfflineEngineTest.cpp
$root/Core/Downloads/Domain/FileNaming.cpp
$root/Core/Downloads/Domain/Mp4Layout.cpp
$root/Core/Downloads/Domain/SidecarPolicy.cpp
$root/Core/Downloads/Infrastructure/DownloadQueueStore.cpp
$root/Core/Downloads/Infrastructure/DownloadLocation.cpp
$root/Core/Downloads/Infrastructure/DownloadEngine.cpp
$root/Core/Downloads/Infrastructure/DownloadManager.cpp
$root/Core/Downloads/Infrastructure/SidecarStore.cpp
$root/Core/Resolver/Infrastructure/YtDlpJson.cpp
$root/Core/Media/Domain/StreamSelection.cpp
$root/Core/Media/Domain/AudioTrackPolicy.cpp
$root/Core/Media/Domain/LivePolicy.cpp
$out/moc_engine.cpp
$out/moc_manager.cpp
$out/moc_sidecars.cpp"
pids=""
n=0
for src in $sources; do
    n=$((n + 1))
    # shellcheck disable=SC2086
    "$CXX" $cxxflags -I"$root" $qt -c "$src" -o "$out/obj/$n.o" > "$out/obj/$n.log" 2>&1 &
    pids="$pids $!"
done
"$CXX" $cxxflags -I"$root" $qt -c "$root/Core/Downloads/Domain/DownloadPolicy.cpp" -o "$out/policy.o"
status=0
for p in $pids; do wait "$p" || status=1; done
[ "$status" -eq 0 ] || { cat "$out"/obj/*.log; fail "OfflineEngineTest did not build"; }
"$CXX" -o "$out/offline" "$out"/obj/*.o "$out/policy.o" $qtlibs -ldl

echo "offline_engine: OfflineEngineTest under the guard"
set +e
OFFLINE_ENOSPC_SHIM=1 LD_PRELOAD="$out/enospc.so" guard timeout -s KILL 300 "$out/offline" > "$out/offline.txt" 2>&1
rc=$?
set -e
cat "$out/offline.txt"
[ "$rc" -eq 77 ] && skip "no QSQLITE driver"
[ "$rc" -eq 0 ] || fail "OfflineEngineTest failed (exit $rc)"
grep -q '^CASE guardCanary PASS' "$out/offline.txt" || fail "the guard was not active inside the Qt test process"

# ---- 3. mutation: no 416 handling must make the suite fail ---------------------------------------
echo "offline_engine: mutation check (416 handling removed in scratch copies)"
# (a) the media proxy: the branch that answers an unsatisfiable Range with 416 "bytes */total".
grep -v 'respondError(job, 416' "$infra/MediaProxy.cpp" > "$out/mut/MediaProxy.cpp" || true
removed=$(( $(wc -l < "$infra/MediaProxy.cpp") - $(wc -l < "$out/mut/MediaProxy.cpp") ))
[ "$removed" -eq 1 ] || fail "proxy mutation did not apply (removed $removed lines): MediaProxy.cpp changed, update run.sh"
"$moc" "$infra/HttpsConnection.h" -o "$out/mut/moc_HttpsConnection.cpp"
# shellcheck disable=SC2086
"$CXX" $cxxflags $(pkg-config --cflags Qt5Core Qt5Network) -I"$infra" -DFIXTURE_DIR="\"$pin/tests/sharedplayback/fixtures\"" \
    -o "$out/mut/proxy" "$pin/tests/sharedplayback/MediaProxyTest.cpp" "$out/mut/MediaProxy.cpp" \
    "$infra/HttpsConnection.cpp" "$out/mut/moc_HttpsConnection.cpp" $(pkg-config --libs Qt5Core Qt5Network)
set +e
guard timeout -s KILL 180 "$out/mut/proxy" > "$out/mut/proxy.txt" 2>&1
proxyRc=$?
set -e
[ "$proxyRc" -ne 0 ] || { cat "$out/mut/proxy.txt"; fail "MediaProxyTest still passes without the 416 handling"; }
grep -q 'status == 416' "$out/mut/proxy.txt" || { cat "$out/mut/proxy.txt"; fail "MediaProxyTest failed without the 416 handling, but not on a 416 check"; }
echo "  MediaProxy.cpp without respondError(job, 416, ...): MediaProxyTest FAILS as required (exit $proxyRc):"
grep 'status == 416' "$out/mut/proxy.txt" | sed 's/^/    /'
# (b) the download engine: resumeAfterResponse's "416 -> restart from zero".
grep -v 'if (s == 416) return ResumeAction::RestartFromZero;' "$root/Core/Downloads/Domain/DownloadPolicy.cpp" > "$out/mut/DownloadPolicy.cpp" || true
removed=$(( $(wc -l < "$root/Core/Downloads/Domain/DownloadPolicy.cpp") - $(wc -l < "$out/mut/DownloadPolicy.cpp") ))
[ "$removed" -eq 1 ] || fail "engine mutation did not apply (removed $removed lines): DownloadPolicy.cpp changed, update run.sh"
# The copy's relative includes resolve through -I of the original directory.
"$CXX" $cxxflags -I"$root" -I"$root/Core/Downloads/Domain" $qt -c "$out/mut/DownloadPolicy.cpp" -o "$out/mut/policy.o"
"$CXX" -o "$out/mut/offline" "$out"/obj/*.o "$out/mut/policy.o" $qtlibs -ldl
set +e
guard timeout -s KILL 300 "$out/mut/offline" > "$out/mut/offline.txt" 2>&1
engineRc=$?
set -e
[ "$engineRc" -ne 0 ] || { cat "$out/mut/offline.txt"; fail "OfflineEngineTest still passes without the 416 handling"; }
grep -q '^CASE case416StarTotal FAIL' "$out/mut/offline.txt" || { cat "$out/mut/offline.txt"; fail "OfflineEngineTest failed without the 416 handling, but not in case416StarTotal"; }
echo "  DownloadPolicy.cpp without 'if (s == 416) return ResumeAction::RestartFromZero;': OfflineEngineTest FAILS as required (exit $engineRc):"
grep -E '^CASE|\[416' "$out/mut/offline.txt" | sed 's/^/    /'

# ---- 4. nothing tried to leave the machine; the case table ----------------------------------------
if [ -s "$NO_NETWORK_LOG" ]; then cat "$NO_NETWORK_LOG"; fail "a test tried to reach a non-loopback address"; fi
echo "offline_engine: blocked network attempts during the fetcher tests: 0"

# The proxy and download-suite rows name functions of other suites: make sure they still exist and run.
for f in testServing testSeekMidStream testEarlyRefusalIsRetriedInPlace testRefusalPersistsThenRefreshesAndGivesUp \
         testDroppedConnections testLimitsAndClose; do
    grep -q "^static void $f()" "$pin/tests/sharedplayback/MediaProxyTest.cpp" && grep -q "^    $f();" "$pin/tests/sharedplayback/MediaProxyTest.cpp" \
        || fail "MediaProxyTest no longer has $f: update the table in run.sh"
done
for f in audioEndToEnd resumeFaults restartResume; do
    grep -q "^static void $f()" "$root/tests/downloads_engine/DownloadsEngineTest.cpp" && grep -q "^    $f();" "$root/tests/downloads_engine/DownloadsEngineTest.cpp" \
        || fail "DownloadsEngineTest no longer has $f: update the table in run.sh"
done
result() { grep -q "^CASE $1 PASS" "$out/offline.txt" && echo PASS || echo FAIL; }
echo
echo "offline_engine: NFR-TST-02 cases (all run under no_network.so; the proxy/suite rows pass as whole binaries)"
printf '  %-24s %-62s %s\n' "case" "named test (binary)" "result"
row() { printf '  %-24s %-62s %s\n' "$1" "$2" "$3"; }
row "Range"                  "caseRange (OfflineEngineTest)"                        "$(result caseRange)"
row ""                       "testServing, testSeekMidStream (MediaProxyTest)"      PASS
row ""                       "audioEndToEnd, restartResume (DownloadsEngineTest)"   PASS
row "403 at byte 0"          "case403AtByteZero (OfflineEngineTest)"                "$(result case403AtByteZero)"
row ""                       "testEarlyRefusalIsRetriedInPlace (MediaProxyTest)"    PASS
row ""                       "resumeFaults (DownloadsEngineTest)"                   PASS
row "young-token window"     "caseYoungTokenWindow (OfflineEngineTest)"             "$(result caseYoungTokenWindow)"
row ""                       "testRefusalPersistsThenRefreshesAndGivesUp (MediaProxyTest)" PASS
row "truncation and reopen"  "caseTruncationAndReopen (OfflineEngineTest)"          "$(result caseTruncationAndReopen)"
row ""                       "testDroppedConnections (MediaProxyTest)"              PASS
row ""                       "resumeFaults (DownloadsEngineTest)"                   PASS
row "416 */total"            "case416StarTotal (OfflineEngineTest)"                 "$(result case416StarTotal)"
row ""                       "testServing (MediaProxyTest)"                         PASS
row "503 when space is low"  "case503AndLowSpace (OfflineEngineTest)"               "$(result case503AndLowSpace)"
row ""                       "testLimitsAndClose (MediaProxyTest)"                  PASS
row "guard inside Qt"        "guardCanary: QTcpSocket to 1.1.1.1:443 (OfflineEngineTest)"   "$(result guardCanary)"
row "mutation: no 416"       "MediaProxyTest, OfflineEngineTest"                    "FAIL as required"
echo "  note: '503 when space is low' covers an upstream 503 (engine), the proxy's 503 when no stream slot is free,"
echo "        the engine's free-space precheck (no_space, then a retry) and a planted /dev/full part link being replaced."
echo "offline_engine: PASS"
