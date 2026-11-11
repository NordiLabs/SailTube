#!/usr/bin/env python3
# SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
"""E2 probe: does a loopback hand-off work in this process (inside or outside Sailjail)?

Stages, each logged separately as `e2: stage=<name> result=ok|fail|skip detail=...`:
  datadir  write/read/delete a probe file in each PLT-03 directory, TMPDIR and /tmp
  bind     an in-process HTTP server binds 127.0.0.1 on an ephemeral port (socket/bind errors)
  connect  a Python client in the same process GETs a Range from it (connect errors)
  gst      GStreamer in the same process (python3-gobject) fetches the clip with souphttpsrc and
           decodes it with decodebin into fakesink for 3 s: bytes and decoded frames
  exec     a child gst-launch-1.0 fetches the same URL (exec errors kept apart from fetch errors)

Python 3 stdlib (+ optional gi); run with `python3 -I`. Loads range_server.py from its own
directory by path, so -I isolation stays on.

    python3 -I e2_loopback.py --clip FILE [--label sandbox-internet]
"""
import argparse
import errno
import importlib.util
import os
import socket
import subprocess
import sys
import time
import urllib.request

HERE = os.path.dirname(os.path.abspath(__file__))
ORG, APP = "org.nordilabs.sailtube", "SailTube"


def log(label, stage, result, detail=""):
    print("e2: label=%s stage=%s result=%s detail=%s" % (label, stage, result, str(detail).replace(" ", "_")),
          flush=True)


def describe(error):
    code = getattr(error, "errno", None)
    name = errno.errorcode.get(code, "") if code else ""
    return "%s:%s:%s" % (type(error).__name__, name, error)


def load_range_server():
    spec = importlib.util.spec_from_file_location("range_server", os.path.join(HERE, "range_server.py"))
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def stage_datadirs(label):
    home = os.path.expanduser("~")
    dirs = [os.path.join(home, base, ORG, APP) for base in (".local/share", ".cache", ".config")]
    dirs += [os.environ.get("TMPDIR", ""), "/tmp"]
    for directory in [d for d in dirs if d]:
        probe = os.path.join(directory, ".e2-probe-%d" % os.getpid())
        try:
            os.makedirs(directory, exist_ok=True)
            with open(probe, "w") as handle:
                handle.write("e2")
            with open(probe) as handle:
                handle.read()
            os.remove(probe)
            log(label, "datadir", "ok", directory)
        except OSError as error:
            log(label, "datadir", "fail", directory + ":" + describe(error))


def stage_bind(label, clip):
    # A raw socket first, so a seccomp "protocol unix" denial shows as socket(), not as bind().
    try:
        probe = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    except OSError as error:
        log(label, "bind", "fail", "socket(AF_INET):" + describe(error))
        return None
    try:
        probe.bind(("127.0.0.1", 0))
        probe.listen(1)
    except OSError as error:
        log(label, "bind", "fail", "bind/listen:" + describe(error))
        return None
    finally:
        probe.close()
    try:
        server, port, token = load_range_server().start(os.path.dirname(os.path.abspath(clip)))
    except OSError as error:
        log(label, "bind", "fail", "server:" + describe(error))
        return None
    log(label, "bind", "ok", "127.0.0.1:%d" % port)
    return server, "http://127.0.0.1:%d/%s/%s" % (port, token, os.path.basename(clip))


def stage_connect(label, url):
    request = urllib.request.Request(url, headers={"Range": "bytes=0-65535"})
    try:
        with urllib.request.urlopen(request, timeout=10) as response:
            body = response.read()
            log(label, "connect", "ok" if response.status == 206 else "fail",
                "status=%d bytes=%d" % (response.status, len(body)))
    except Exception as error:  # noqa: BLE001 - every failure kind is a result here
        log(label, "connect", "fail", describe(error))


def stage_gst(label, url):
    try:
        import gi
        gi.require_version("Gst", "1.0")
        from gi.repository import Gst
    except (ImportError, ValueError) as error:
        log(label, "gst", "skip", "python3-gobject:" + describe(error))
        return
    Gst.init(None)
    pipeline = Gst.parse_launch("souphttpsrc name=src location=%s ! identity name=bytes ! "
                                "decodebin ! fakesink name=sink sync=false" % url)
    counts = {"bytes": 0, "frames": 0, "first_ms": -1}
    start = time.monotonic()

    def on_bytes(_element, buffer):
        counts["bytes"] += buffer.get_size()

    def on_frame(_element, _buffer, _pad):
        if counts["first_ms"] < 0:
            counts["first_ms"] = int((time.monotonic() - start) * 1000)
        counts["frames"] += 1

    pipeline.get_by_name("bytes").connect("handoff", on_bytes)
    sink = pipeline.get_by_name("sink")
    sink.set_property("signal-handoffs", True)
    sink.connect("handoff", on_frame)
    pipeline.set_state(Gst.State.PLAYING)
    message = pipeline.get_bus().timed_pop_filtered(3 * Gst.SECOND, Gst.MessageType.ERROR | Gst.MessageType.EOS)
    error = ""
    if message is not None and message.type == Gst.MessageType.ERROR:
        error = message.parse_error()[0].message
    pipeline.set_state(Gst.State.NULL)
    ok = counts["frames"] > 0 and not error
    log(label, "gst", "ok" if ok else "fail", "bytes=%d frames=%d first_ms=%d error=%s"
        % (counts["bytes"], counts["frames"], counts["first_ms"], error))


def stage_exec(label, url):
    command = ["gst-launch-1.0", "-q", "souphttpsrc", "location=" + url, "!", "fakesink"]
    try:
        child = subprocess.run(command, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, timeout=20)
    except OSError as error:
        log(label, "exec", "fail", "exec:" + describe(error))
        return
    except subprocess.TimeoutExpired:
        log(label, "exec", "fail", "timeout-20s")
        return
    tail = child.stdout.decode("utf-8", "replace").strip().splitlines()[-1:] or [""]
    log(label, "exec", "ok" if child.returncode == 0 else "fail", "rc=%d last=%s" % (child.returncode, tail[0]))


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--clip", required=True)
    parser.add_argument("--label", default="run")
    args = parser.parse_args()
    log(args.label, "start", "ok", "pid=%d uid=%d python=%s" % (os.getpid(), os.getuid(), sys.version.split()[0]))
    stage_datadirs(args.label)
    bound = stage_bind(args.label, args.clip)
    if bound is None:
        for stage in ("connect", "gst", "exec"):
            log(args.label, stage, "skip", "no-server")
        return 1
    server, url = bound
    stage_connect(args.label, url)
    stage_gst(args.label, url)
    stage_exec(args.label, url)
    server.shutdown()
    return 0


if __name__ == "__main__":
    sys.exit(main())
