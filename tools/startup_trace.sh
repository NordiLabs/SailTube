#!/bin/sh
# SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
# Start-up cost in the build container (docs/PERF_STARTUP.md): runs the binary of the last
# tools/native_build.sh run with SAILTUBE_STARTUP_TRACE=exit and the QML of that snapshot, N times on a
# fresh home (first launch: empty stores, migrations) and N times on the same home (later launches), and
# prints the median milliseconds since process start per phase. The container has no GL, so the last phase
# is the QML scene (root object created); the first frame is a device measurement.
#
#   tools/startup_trace.sh [RUNS]      (default 5)
set -u
RUNS=${1:-5}
DOCKER_SOCK="${DOCKER_SOCK:-unix://$HOME/.orbstack/run/docker.sock}"
CONTAINER="${SFOS_NATIVE_CONTAINER:-sfos-m1-sailtube}"
KEY="${SAILTUBE_BUILD_KEY:-NordTileProject-b3904586}"
dk() { docker -H "$DOCKER_SOCK" "$@"; }
dk exec -i -u sdk "$CONTAINER" sh -s "$KEY" "$RUNS" <<'EOS'
key=$1; runs=$2
bin=/builds/$key/obj/harbour-sailtube; qml=/builds/$key/src/Clients/Sailfish/qml
[ -x "$bin" ] || { echo "no binary at $bin (run tools/native_build.sh first)"; exit 2; }
work=$(mktemp -d /tmp/sailtube-startup.XXXXXX)
one() {  # one <home> -> trace lines on stdout
    HOME=$1 XDG_RUNTIME_DIR=$work QT_QPA_PLATFORM=minimal SAILTUBE_STARTUP_TRACE=exit SAILTUBE_QML_ROOT=$qml \
        SAILTUBE_NO_WARM_RESOLVER=1 timeout 60 "$bin" 2>&1 | grep '^sailtube-startup '
}
summary() {  # summary <label> <files...>
    label=$1; shift
    python3 -I - "$label" "$@" <<'PY'
import sys, collections
label, files = sys.argv[1], sys.argv[2:]
phases, order = collections.defaultdict(list), []
for f in files:
    for line in open(f):
        parts = line.split()
        if len(parts) >= 4 and parts[-1] == "ms":
            name, ms = parts[1], int(parts[-2])
            if name not in order: order.append(name)
            phases[name].append(ms)
print("%s (%d runs, median ms since process start):" % (label, len(files)))
prev = 0
for name in order:
    v = sorted(phases[name]); m = v[len(v) // 2]
    print("  %-22s %6d   (+%d)" % (name, m, m - prev)); prev = m
PY
}
i=0; fresh=""
while [ $i -lt "$runs" ]; do
    h=$work/fresh$i; mkdir -p "$h"; one "$h" > "$work/f$i.txt"; fresh="$fresh $work/f$i.txt"; i=$((i+1))
done
h=$work/warm; mkdir -p "$h"; one "$h" > /dev/null
i=0; warm=""
while [ $i -lt "$runs" ]; do one "$h" > "$work/w$i.txt"; warm="$warm $work/w$i.txt"; i=$((i+1)); done
summary "first launch (fresh home)" $fresh
summary "later launch (existing stores)" $warm
grep -h "qml status" "$work"/f0.txt 2>/dev/null
EOS
