#!/bin/sh
# SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
# NFR-TST-07: weekly live check against YouTube (docs/LIVE_CHECK.md). Three steps, one request
# budget each, never retried:
#
#   discovery   one InnerTube WEB search with catalog's built-in client version
#               (tools/record_fixtures.py transport, Core/Catalog/Infrastructure/ClientIdentity.cpp)
#   fast_path   one cold resolve-to-first-frame round of the pinned SailTune fast path + player
#               (tools/perf_playback.sh --local, one video, mode cold)
#   download    one small download by the packaged extractor through the app's warm worker
#               (Core/Worker/Runtime/worker.py + yt-dlp.pyz, the app's common yt-dlp arguments)
#
# Each step is PASS, FAIL or SKIPPED. A bot wall ("Sign in to confirm you're not a bot",
# BotWall / BotCheck, LOGIN_REQUIRED) or HTTP 429 is SKIPPED, never FAIL. Writes a dated summary
# (live-check-YYYY-MM-DDTHHMMZ.txt) and the full log (.log) to --out.
#
#   tools/live_check.sh [--local | --container NAME] [--out DIR] [--skip STEP[,STEP]]
#                       [--query TEXT] [--video ID] [--download-video ID] [--archive PATH]
#
#   --local       run here (Linux with network; the aarch64 runner). Default on Linux.
#   --container   copy what the steps need into NAME (default sfos-m1-sailtube; Docker via
#                 $DOCKER_HOST or OrbStack's socket) and run there. Default elsewhere (macOS).
#   --out DIR     summary directory (default ${XDG_STATE_HOME:-$HOME/.local/state}/sailtube-live)
#   --skip        discovery, fast_path and/or download: reported SKIPPED (requested)
#   --archive     packaged yt-dlp.pyz, a path where the steps run (default: $SAILTUBE_YTDLP_ARCHIVE, vendor/python-runtime,
#                 /usr/share/harbour-sailtube/runtime, /builds/_vendor/python-runtime/runtime)
# Exit: 0 no step failed, 1 a step failed, 2 usage or infrastructure error.
set -u
root=$(cd "$(dirname "$0")/.." && pwd)
mode=
container=${SAILTUBE_LIVE_CONTAINER:-sfos-m1-sailtube}
out=${XDG_STATE_HOME:-$HOME/.local/state}/sailtube-live
skip=
query=kurzgesagt
video=aqz-KE-bpKQ
dlvideo=jNQXAC9IVRw
archive=${SAILTUBE_YTDLP_ARCHIVE:-}
pass_args=
while [ $# -gt 0 ]; do
    case "$1" in
        --local) mode=local; shift;;
        --container) mode=container; container=$2; shift 2;;
        --out) out=$2; shift 2;;
        --skip) skip=$2; pass_args="$pass_args --skip $2"; shift 2;;
        --query) query=$2; shift 2;;
        --video) video=$2; pass_args="$pass_args --video $2"; shift 2;;
        --download-video) dlvideo=$2; pass_args="$pass_args --download-video $2"; shift 2;;
        --archive) archive=$2; shift 2;;
        -h|--help) sed -n '2,29p' "$0"; exit 0;;
        *) echo "live_check: unknown option $1" >&2; exit 2;;
    esac
done
for id in "$video" "$dlvideo"; do
    printf '%s' "$id" | grep -Eq '^[A-Za-z0-9_-]{11}$' || { echo "live_check: invalid video id $id" >&2; exit 2; }
done
[ -n "$mode" ] || { [ "$(uname -s)" = Linux ] && mode=local || mode=container; }

commit=${LIVE_CHECK_COMMIT:-}
if [ -z "$commit" ] && command -v git >/dev/null 2>&1 && git -C "$root" rev-parse --git-dir >/dev/null 2>&1; then
    commit="$(git -C "$root" rev-parse --short HEAD) (+$(git -C "$root" status --porcelain 2>/dev/null | wc -l | tr -d ' ') local changes)"
fi
[ -n "$commit" ] || commit="unknown (no .git)"

# ------------------------------------------------------------------------------------ container
if [ "$mode" = container ]; then
    if [ -z "${DOCKER_HOST:-}" ] && [ -S "$HOME/.orbstack/run/docker.sock" ]; then
        DOCKER_HOST="unix://$HOME/.orbstack/run/docker.sock"; export DOCKER_HOST
    fi
    command -v docker >/dev/null 2>&1 || { echo "live_check: no docker for --container" >&2; exit 2; }
    dk() { docker exec -u sdk "$container" "$@"; }
    dir=$(dk mktemp -d /tmp/sailtube-live.XXXXXX) || { echo "live_check: container $container unavailable" >&2; exit 2; }
    trap 'docker exec -u sdk "$container" rm -rf "$dir" >/dev/null 2>&1' EXIT INT TERM
    files="tools/live_check.sh tools/record_fixtures.py tools/perf_playback.sh tools/perf_playback Core/Player
           Core/Catalog/Infrastructure/ClientIdentity.cpp Core/Resolver/Infrastructure/YtDlpRunner.cpp
           Core/Worker/Runtime/worker.py Shared/Runtime/Python/resolver.py Shared/SailTune/Core/SharedPlayback"
    # shellcheck disable=SC2086
    (cd "$root" && COPYFILE_DISABLE=1 tar -cf - $files) | docker exec -i -u sdk "$container" tar -xf - -C "$dir" 2>/dev/null ||
        { echo "live_check: copy into $container failed" >&2; exit 2; }
    echo "live_check: running in $container:$dir" >&2
    rc=0
    # shellcheck disable=SC2086
    docker exec -u sdk -w "$dir" -e TMPDIR="$dir" -e LIVE_CHECK_COMMIT="$commit" -e LIVE_CHECK_WHERE="container $container" \
        "$container" sh tools/live_check.sh --local --out "$dir/out" --query "$query" ${archive:+--archive "$archive"} $pass_args || rc=$?
    mkdir -p "$out"
    for f in $(dk sh -c "cd '$dir/out' 2>/dev/null && ls"); do
        dk cat "$dir/out/$f" > "$out/$f"
        echo "live_check: wrote $out/$f" >&2
    done
    exit $rc
fi

# ------------------------------------------------------------------------------------ local
command -v python3 >/dev/null 2>&1 || { echo "live_check: python3 missing" >&2; exit 2; }
mkdir -p "$out" || exit 2
stamp=$(date -u +%Y-%m-%dT%H%MZ)
summary="$out/live-check-$stamp.txt"
log="$out/live-check-$stamp.log"
tmp=$(mktemp -d "${TMPDIR:-/tmp}/sailtube-live.XXXXXX") || exit 2
trap 'rm -rf "$tmp"' EXIT INT TERM
: > "$log"
{
    echo "SailTube live check (NFR-TST-07) $(date -u +%Y-%m-%dT%H:%M:%SZ)"
    echo "where: ${LIVE_CHECK_WHERE:-local} $(uname -s) $(uname -m), $(python3 --version 2>&1)"
    echo "commit: $commit"
} > "$summary"
npass=0; nfail=0; nskip=0
report() {  # step status detail
    case "$2" in PASS) npass=$((npass + 1));; FAIL) nfail=$((nfail + 1));; *) nskip=$((nskip + 1));; esac
    echo "$1: $2 $3" | tee -a "$summary" >&2
}
skipped() { case ",$skip," in *",$1,"*) return 0;; esac; return 1; }
section() { printf '\n===== %s %s\n' "$1" "$(date -u +%H:%M:%SZ)" >> "$log"; }

# Bot wall / rate limit classes shared by every step (same markers as Playability.h and YtDlpJson.cpp).
classify_text() {  # file -> prints bot_wall | rate_limited | nothing
    if grep -Eqi "sign in to confirm you.?re not a bot|not a bot|bot_wall|BotCheck|bot_check" "$1"; then echo bot_wall
    elif grep -Eq "login_required|LOGIN_REQUIRED" "$1"; then echo login_required
    elif grep -Eqi "HTTP Error 429|HTTP 429|Too Many Requests|status=429|rate_limited" "$1"; then echo rate_limited
    fi
}

# ---- 1. discovery: one search -----------------------------------------------------------------------
section discovery
if skipped discovery; then report discovery SKIPPED "(requested)"
else
    python3 -I - "$root" "$query" > "$tmp/discovery.out" 2>> "$log" <<'PY'
import importlib.util, json, os, re, sys, time
root, query = sys.argv[1], sys.argv[2]
spec = importlib.util.spec_from_file_location("record_fixtures", os.path.join(root, "tools", "record_fixtures.py"))
rf = importlib.util.module_from_spec(spec)
sys.dont_write_bytecode = True
spec.loader.exec_module(rf)
src = open(os.path.join(root, "Core/Catalog/Infrastructure/ClientIdentity.cpp"), encoding="utf-8").read()
m = re.search(r'builtInVersion\(\)\s*\{\s*return\s*"([^"]+)"', src)
if not m:
    print("FAIL could not read the built-in WEB version from ClientIdentity.cpp"); sys.exit(0)
version = m.group(1)
t0 = time.monotonic()
try:
    status, raw = rf.InnerTube(version).post("search", {"query": query}, "en", "US")
except Exception as e:   # no response at all: the runner's network
    print("FAIL network error %s (WEB %s)" % (type(e).__name__, version)); sys.exit(0)
ms = int((time.monotonic() - t0) * 1000)
text = raw.decode("utf-8", "replace")
print("search HTTP %d %d bytes %d ms WEB %s" % (status, len(raw), ms, version), file=sys.stderr)
if status == 429:
    print("SKIPPED rate_limited (HTTP 429, WEB %s)" % version); sys.exit(0)
if re.search(r"(?i)sign in to confirm you.?re not a bot|not a bot", text) or "LOGIN_REQUIRED" in text:
    print("SKIPPED bot_wall (HTTP %d, WEB %s)" % (status, version)); sys.exit(0)
if status != 200:
    hint = " (built-in version rejected? see docs/CLIENT_DEFINITIONS.md)" if status == 400 else ""
    print("FAIL HTTP %d%s (WEB %s)" % (status, hint, version)); sys.exit(0)
try:
    doc = json.loads(text)
except ValueError:
    print("FAIL HTTP 200 but not JSON (WEB %s)" % version); sys.exit(0)
ids = set()
for key in ("videoRenderer", "lockupViewModel"):
    for r in rf.find(doc, key):
        vid = r.get("videoId") or r.get("contentId") if isinstance(r, dict) else None
        if vid:
            ids.add(vid)
if not ids:
    print("FAIL HTTP 200 but no video rows (shape drift?) (WEB %s, %d ms)" % (version, ms)); sys.exit(0)
print("PASS %d videos for one search, %d ms (WEB %s built-in)" % (len(ids), ms, version))
PY
    cat "$tmp/discovery.out" >> "$log"
    line=$(tail -n 1 "$tmp/discovery.out")
    [ -n "$line" ] || line="FAIL helper produced no result (see log)"
    report discovery "${line%% *}" "${line#* }"
fi

# ---- 2. fast path: one cold resolve -> first frame -------------------------------------------------
section fast_path
if skipped fast_path; then report fast_path SKIPPED "(requested)"
else
    rc=0
    TMPDIR="$tmp" sh "$root/tools/perf_playback.sh" --local --rounds 1 --videos "$video" --modes cold --pause 0 \
        > "$tmp/fast.out" 2>&1 || rc=$?
    cat "$tmp/fast.out" >> "$log"
    result=$(grep 'sailtube-perf: result ' "$tmp/fast.out" | tail -n 1)
    reason=$(printf '%s' "$result" | sed -n 's/.* reason=\([^ ]*\).*/\1/p')
    frame=$(grep 'sailtube-timing: first-frame ' "$tmp/fast.out" | tail -n 1 | sed -n 's/.* t=\([0-9]*\).*/\1/p')
    path=$(grep 'sailtube-timing: resolve-done ' "$tmp/fast.out" | tail -n 1 | sed -n 's/.* path=\([a-z]*\).*/\1/p')
    if printf '%s' "$result" | grep -q ' ok=1 '; then
        report fast_path PASS "video $video tap-to-first-frame ${frame:-?} ms (path ${path:-?}, cold)"
    elif [ -z "$result" ]; then
        if grep -q 'development files missing' "$tmp/fast.out"; then report fast_path FAIL "runner lacks GStreamer/Qt5 development files (exit $rc)"
        else report fast_path FAIL "harness produced no round (exit $rc; build or start failure, see log)"; fi
    else
        printf '%s\n' "$reason" > "$tmp/fast.reason"
        class=$(classify_text "$tmp/fast.reason")
        if [ -n "$class" ]; then report fast_path SKIPPED "$class (video $video, reason $reason)"
        else report fast_path FAIL "video $video reason $reason"; fi
    fi
fi

# ---- 3. download: one small download through the packaged extractor -------------------------------
section download
if skipped download; then report download SKIPPED "(requested)"
else
    if [ -z "$archive" ]; then
        for c in "$root/vendor/python-runtime/runtime/yt-dlp.pyz" /usr/share/harbour-sailtube/runtime/yt-dlp.pyz \
                 /builds/_vendor/python-runtime/runtime/yt-dlp.pyz; do
            [ -s "$c" ] && { archive=$c; break; }
        done
    fi
    if [ -z "$archive" ] || [ ! -s "$archive" ]; then
        report download FAIL "no packaged yt-dlp.pyz (pass --archive or SAILTUBE_YTDLP_ARCHIVE)"
    elif ! python3 -c 'import sys; sys.exit(0 if sys.version_info >= (3, 10) else 1)'; then
        report download FAIL "python3 >= 3.10 needed by the packaged extractor ($(python3 --version 2>&1))"
    else
        mkdir -p "$tmp/runtime" "$tmp/worker" "$tmp/rtmp" "$tmp/dl"
        chmod 700 "$tmp/worker" "$tmp/rtmp"
        cp "$root/Core/Worker/Runtime/worker.py" "$root/Shared/Runtime/Python/resolver.py" "$tmp/runtime/"
        python3 -I - "$root" "$tmp" "$archive" "$dlvideo" > "$tmp/download.out" 2>> "$log" <<'PY'
import json, os, re, subprocess, sys, time
root, tmp, archive, video = sys.argv[1:5]
src = open(os.path.join(root, "Core/Resolver/Infrastructure/YtDlpRunner.cpp"), encoding="utf-8").read()
m = re.search(r"commonArguments\(bool javascript\) const \{(.*?)if \(javascript", src, re.S)
common = re.findall(r'QStringLiteral\("([^"]*)"\)', m.group(1)) if m else []
if "--no-js-runtimes" not in common:
    print("FAIL could not read the app's common yt-dlp arguments from YtDlpRunner.cpp"); sys.exit(0)
dl = os.path.join(tmp, "dl")
args = common + ["--no-playlist", "--format", "worstaudio/worst", "--max-filesize", "10M",
                 "--output", os.path.join(dl, "download.%(ext)s"), "--", "https://www.youtube.com/watch?v=" + video]
env = dict(os.environ, NORDTILE_WORKER_ARCHIVE=archive, NORDTILE_WORKER_DIR=os.path.join(tmp, "worker"),
           SAILTUNE_RESOLVER_TMPDIR=os.path.realpath(os.path.join(tmp, "rtmp")))
t0 = time.monotonic()
p = subprocess.Popen([sys.executable, "-I", os.path.join(tmp, "runtime", "worker.py")], stdin=subprocess.PIPE,
                     stdout=subprocess.PIPE, stderr=subprocess.PIPE, env=env, cwd=tmp)
try:
    o, e = p.communicate((json.dumps({"id": 1, "args": args}) + "\n").encode(), timeout=240)
except subprocess.TimeoutExpired:
    p.kill(); p.communicate()
    print("FAIL worker timed out after 240 s"); sys.exit(0)
ms = int((time.monotonic() - t0) * 1000)
lines = [json.loads(l) for l in o.decode("utf-8", "replace").splitlines() if l.strip().startswith("{")]
ready = lines[0] if lines else {}
if not ready.get("ready"):
    print("FAIL worker not ready: %s" % ready.get("error", "no output")); sys.exit(0)
reply = next((l for l in lines[1:] if l.get("id") == 1), None)
if reply is None:
    print("FAIL worker gave no reply (yt-dlp %s)" % ready.get("version")); sys.exit(0)
err = reply.get("err", "") + reply.get("out", "")
sys.stderr.write(re.sub(r"https?://\S+", "<url>", err)[-4000:] + "\n")
files = [f for f in os.listdir(dl) if not f.endswith((".part", ".ytdl"))]
size = sum(os.path.getsize(os.path.join(dl, f)) for f in files)
info = "yt-dlp %s, video %s, %d ms" % (ready.get("version"), video, ms)
if re.search(r"(?i)sign in to confirm you.?re not a bot|not a bot", err):
    print("SKIPPED bot_wall (%s)" % info); sys.exit(0)
if re.search(r"(?i)HTTP Error 429|Too Many Requests", err):
    print("SKIPPED rate_limited (HTTP 429, %s)" % info); sys.exit(0)
if reply.get("code") == 0 and size > 0:
    print("PASS %d bytes (%s, %s)" % (size, ",".join(os.path.splitext(f)[1] for f in files), info)); sys.exit(0)
last = [l for l in err.splitlines() if l.startswith("ERROR")]
detail = re.sub(r"https?://\S+", "<url>", last[-1] if last else "no file written")[:200]
print("FAIL code %s: %s (%s)" % (reply.get("code"), detail, info))
PY
        cat "$tmp/download.out" >> "$log"
        line=$(tail -n 1 "$tmp/download.out")
        [ -n "$line" ] || line="FAIL helper produced no result (see log)"
        report download "${line%% *}" "${line#* }"
    fi
fi

if [ $nfail -gt 0 ]; then overall=FAIL; elif [ $npass -gt 0 ]; then overall=PASS; else overall=SKIPPED; fi
echo "live_check: $overall (pass $npass, skipped $nskip, fail $nfail)" | tee -a "$summary" >&2
echo "summary: $summary" >&2
[ $nfail -eq 0 ]
