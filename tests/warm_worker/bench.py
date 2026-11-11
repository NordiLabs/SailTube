#!/usr/bin/env python3
# SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
"""Cold vs warm resolver cost, offline: N yt-dlp runs of a local file (file:// URL, generic extractor),
once with a fresh worker per run (what one child per resolve costs on the Python side) and once through
one warm worker. Usage: bench.py RUNTIME_DIR ARCHIVE [N]. Prints one JSON line."""
import json, os, subprocess, sys, tempfile, time

def spawn(runtime, archive, work):
    tmp = tempfile.mkdtemp(dir=work); os.chmod(tmp, 0o700)
    env = dict(os.environ, NORDTILE_WORKER_ARCHIVE=archive, NORDTILE_WORKER_DIR=work, SAILTUNE_RESOLVER_TMPDIR=os.path.realpath(tmp))
    p = subprocess.Popen([sys.executable, "-I", os.path.join(runtime, "worker.py")], stdin=subprocess.PIPE,
                         stdout=subprocess.PIPE, env=env)
    ready = json.loads(p.stdout.readline())
    assert ready.get("ready"), ready
    return p, ready

def ask(p, rid, args):
    p.stdin.write((json.dumps({"id": rid, "args": args}) + "\n").encode()); p.stdin.flush()
    return json.loads(p.stdout.readline())

def main():
    runtime, archive = sys.argv[1], sys.argv[2]
    n = int(sys.argv[3]) if len(sys.argv) > 3 else 10
    work = tempfile.mkdtemp(); os.chmod(work, 0o700)
    clip = os.path.join(work, "clip.mp4")
    with open(clip, "wb") as f:
        f.write(b"\x00\x00\x00\x18ftypmp42\x00\x00\x00\x00mp42isom" + b"\x00" * 4096)
    args = ["--ignore-config", "--no-plugin-dirs", "--no-cache-dir", "--no-warnings", "--enable-file-urls",
            "--simulate", "--print", "%(id)s %(ext)s", "file://" + clip]
    cold = []
    for i in range(n):
        t0 = time.monotonic()
        p, ready = spawn(runtime, archive, work)
        r = ask(p, 1, args)
        p.stdin.close(); p.wait()
        cold.append((time.monotonic() - t0) * 1000)
        assert r["code"] == 0, r
    t0 = time.monotonic()
    p, ready = spawn(runtime, archive, work)
    start_ms = (time.monotonic() - t0) * 1000
    warm = []
    for i in range(n):
        t0 = time.monotonic()
        r = ask(p, i + 1, args)
        warm.append((time.monotonic() - t0) * 1000)
        assert r["code"] == 0 and "clip mp4" in r["out"], r
    p.stdin.close(); p.wait()
    med = lambda v: sorted(v)[len(v) // 2]
    print(json.dumps({"runs": n, "cold_median_ms": round(med(cold), 1), "cold_min_ms": round(min(cold), 1),
                      "warm_start_ms": round(start_ms, 1), "import_ms": ready["importMs"],
                      "warm_median_ms": round(med(warm), 1), "warm_max_ms": round(max(warm), 1), "yt_dlp": ready["version"]}))

if __name__ == "__main__":
    main()
