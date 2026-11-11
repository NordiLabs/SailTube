#!/bin/sh
# SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
# Checks tools/experiments/perf_summary.py (NFR-PERF-02..06, DIA-03, E12) on a synthetic log.
# Needs only python3. Run from anywhere.
set -e
root=$(cd "$(dirname "$0")/../.." && pwd)
here="$root/tests/perf_summary"
out=${TMPDIR:-/tmp}/sailtube-perf-summary.$$
checks=0
failures=0

check() { # description, then a command that must succeed
    _what=$1; shift
    checks=$((checks + 1))
    if ! "$@"; then
        failures=$((failures + 1))
        echo "FAIL: $_what"
    fi
}

# Row lookup: row METRIC COLUMN (n p50 p95 max value budget verdict).
row() {
    awk -F '\t' -v m="$1" -v c="$2" '
        NR == 1 { for (i = 1; i <= NF; i++) col[$i] = i; next }
        $1 == m { print $col[c]; found = 1 }
        END { if (!found) print "<missing>" }' "$3"
}
is() { # metric column expected [file]
    _got=$(row "$1" "$2" "${4:-$out.tsv}")
    [ "$_got" = "$3" ] || { echo "  $1 $2: got '$_got', want '$3'"; return 1; }
}
status_is() { [ "$1" = "$2" ] || { echo "  exit status $1, want $2"; return 1; }; }

set +e
python3 -I "$root/tools/experiments/perf_summary.py" --tsv "$here/sample_timing.txt" > "$out.tsv" 2> "$out.err"
rc=$?
python3 -I "$root/tools/experiments/perf_summary.py" --tsv --baseline "$here/baseline_timing.txt" \
    "$here/sample_timing.txt" > "$out.ab.tsv" 2>/dev/null
python3 -I "$root/tools/experiments/perf_summary.py" --tsv --keep-warmup "$here/sample_timing.txt" \
    > "$out.keep.tsv" 2>/dev/null
python3 -I "$root/tools/experiments/perf_summary.py" --tsv "$here/baseline_timing.txt" > /dev/null 2>&1
rc_base=$?
python3 -I "$root/tools/experiments/perf_summary.py" "$here/does-not-exist.log" > /dev/null 2>&1
rc_missing=$?
set -e

# nfrPerf02: tap to first frame per path/network, warm-up dropped, nearest-rank percentiles.
check nfrPerf02_fastWifiCount is ttff.fast.wifi n 20
check nfrPerf02_fastWifiP50 is ttff.fast.wifi p50 1000
check nfrPerf02_fastWifiP95 is ttff.fast.wifi p95 1900
check nfrPerf02_fastWifiPass is ttff.fast.wifi verdict PASS
check nfrPerf02_warmupKeptWhenAsked is ttff.fast.wifi max 5000 "$out.keep.tsv"
check nfrPerf02_under20RunsInconclusive is ttff.fast.4g verdict "INCONCLUSIVE(n<20)"
check nfrPerf02_prefetchP50 is ttff.prefetch p50 750
check nfrPerf02_prefetchOverBudgetFails is ttff.prefetch verdict FAIL
check nfrPerf02_extractorBudget is ttff.extractor budget "p50<=6000,p95<=12000"
check nfrPerf02_earlyTapBudgetPlusHalfSecond is ttff.early.fast.wifi budget "p50<=1700,p95<=3000"
check nfrPerf02_earlyTapValue is ttff.early.fast.wifi p50 1500
check p1_fastPathShare is fast-path-share value 90
check p1_fastPathSharePass is fast-path-share verdict PASS
check dia03_unpairedTapWarned grep -q "1 tap(s) without first-frame" "$out.err"
check dia03_malformedMarkerWarned grep -q "sample_timing.txt:159: malformed marker" "$out.err"

# nfrPerf03: seeks in the buffer (p50), outside (max per container/network), bursts, PLY-04.
check nfrPerf03_inBufferP50 is seek.inbuf p50 190
check nfrPerf03_inBufferPass is seek.inbuf verdict PASS
check nfrPerf03_mp4WifiMax is seek.out.mp4.wifi max 2100
check nfrPerf03_mp4WifiOverBudgetFails is seek.out.mp4.wifi verdict FAIL
check nfrPerf03_webmWifiPass is seek.out.webm.wifi verdict PASS
check nfrPerf03_burstWithTwoFlushesFails is seek.burst-flushes verdict FAIL
check nfrPerf03_burstCount is seek.burst-flushes n 2
check ply04_accurateMp4FlushFails is seek.mp4-accurate value 1

# nfrPerf04: quality or dub switch to resumed playback on Wi-Fi.
check nfrPerf04_switchMax is switch.wifi max 1800
check nfrPerf04_switchPass is switch.wifi verdict PASS

# nfrPerf05: PSS per context (MB = 10^6 bytes) and spool.
check nfrPerf05_playbackBudgetKiB is pss.playback-1080p-sw budget "max<=292968KiB"
check nfrPerf05_playbackPass is pss.playback-1080p-sw verdict PASS
check nfrPerf05_feedOverBudgetFails is pss.feed verdict FAIL
check nfrPerf05_spoolPass is spool.playback-1080p-sw verdict PASS

# nfrPerf06: background decode stop and mean CPU over 10 min.
check nfrPerf06_decodeStopMax is background.decode-stop max 1500
check nfrPerf06_decodeStopPass is background.decode-stop verdict PASS
check nfrPerf06_cpuMeanPercent is cpu.background-opus value 7
check nfrPerf06_cpuPass is cpu.background-opus verdict PASS
check nfrPerf06_wakeupRate is wakeups-per-s.background-opus value 10

# e12: speculative waste and saved p50 against a baseline log.
check e12_waste is speculative.waste value 20
check e12_savedP50 is e12.saved-p50.fast.wifi value 200 "$out.ab.tsv"
check e12_pass is e12.saved-p50.fast.wifi verdict PASS "$out.ab.tsv"

# Exit status: 1 with a FAIL row, 0 without, 2 on a missing file.
check exit_failRowGivesOne status_is "$rc" 1
check exit_cleanLogGivesZero status_is "$rc_base" 0
check exit_missingFileGivesTwo status_is "$rc_missing" 2

# The pre-buffer A/B summary (device step B5, tools/perf_playback_ab.sh) on synthetic logs.
check prebufferAbSelftest python3 -I "$root/tools/experiments/prebuffer_ab.py" --selftest

rm -f "$out.tsv" "$out.err" "$out.ab.tsv" "$out.keep.tsv"
echo "$checks checks, $failures failures"
[ "$failures" -eq 0 ]
