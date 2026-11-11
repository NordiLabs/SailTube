# SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
"""Warm resolver worker (RES-01, NFR-ROB-03/04): imports the packaged yt-dlp once and then runs one
yt-dlp command line per request, so a resolve no longer pays a process start, a QML engine, a Python
start-up and the archive import.

Hosted like SailTune's one-shot resolver (resolver.py, same directory, whose temporary-storage policy and
timing helper are reused unchanged): PyOtherSide in the app binary in worker mode, or `python3 -I worker.py`
in tests. Protocol on the inherited pipes, one JSON object per line:
  worker -> {"ready": true, "importMs": n, "version": "...", "archive": "<sha256 of the loaded copy>"}
  host   -> {"id": n, "args": [...]}                 (same bounds as resolver.run)
  worker -> {"id": n, "code": c, "out": "...", "err": "...", "ms": n, "skew": bool}
EOF on stdin ends the worker. yt-dlp's stdout/stderr are captured per request; nothing else is written to
the pipes. NFR-ROB-04: the archive is copied into the private worker directory before the import, so an
update of the installed file mid-session cannot break lazy imports (zipimport keeps file offsets); `skew`
reports that the installed archive no longer matches the loaded copy, until the worker restarts.
"""
import hashlib
import io
import json
import os
import sys
import time

MAX_REQUEST_BYTES = 1 << 20


class _Redirect(io.TextIOBase):
    """sys.stdout/sys.stderr stand-in whose target is swapped per request (yt-dlp keeps a reference)."""
    def __init__(self):
        self.target = io.TextIOWrapper(io.BytesIO(), encoding="utf-8", write_through=True)

    def writable(self):
        return True

    def write(self, text):
        return self.target.write(text)

    def flush(self):
        self.target.flush()

    def isatty(self):
        return False

    @property
    def encoding(self):
        return "utf-8"

    @property
    def buffer(self):
        return self.target.buffer


def _digest(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for block in iter(lambda: f.read(1 << 16), b""):
            h.update(block)
    return h.hexdigest()


def _signature(path):
    try:
        st = os.stat(path)
        return (st.st_size, st.st_mtime_ns, st.st_ino)
    except OSError:
        return None


def _valid(arguments):
    return (isinstance(arguments, list) and 1 <= len(arguments) <= 63
            and all(isinstance(a, str) and len(a.encode("utf-8")) <= 8192 for a in arguments)
            and sum(len(a.encode("utf-8")) for a in arguments) <= 65536)


VERIFY_KEYS = ("NORDTILE_VERIFY_SUMS", "NORDTILE_VERIFY_SIG", "NORDTILE_VERIFY_ARCHIVE", "NORDTILE_VERIFY_ASSET",
               "NORDTILE_VERIFY_VERSION")


def verify_task(here, stdout_fd=1):
    """[FULL] NFR-MNT-01 / NFR-SEC-04: one verification of a staged extractor update (verify_release.py,
    installed next to this file), then return. The key is always the installed one next to this file (the
    environment cannot name another key or pin); the paths come from NORDTILE_VERIFY_*. Nothing from the
    archive is imported or run, and yt-dlp is not imported. One JSON line on stdout; 0 ok, 1 refused,
    2 the verifier is not installed (STORE ships no verifier)."""
    values = {key: os.environ.pop(key, "") for key in VERIFY_KEYS}
    os.environ.pop("NORDTILE_WORKER_TASK", None)
    out = os.fdopen(os.dup(stdout_fd), "w", encoding="utf-8", buffering=1)
    try:
        import verify_release  # noqa: E402  installed next to this file (FULL only)
    except ImportError:
        out.write(json.dumps({"ok": False, "version": None, "sha256": None, "size": None,
                              "error": "verifier-unavailable"}, separators=(",", ":")) + "\n")
        out.flush()
        return 2
    result = verify_release.verify(os.path.join(here, "yt-dlp-release-key.asc"), values["NORDTILE_VERIFY_SUMS"],
                                   values["NORDTILE_VERIFY_SIG"], values["NORDTILE_VERIFY_ARCHIVE"],
                                   values["NORDTILE_VERIFY_ASSET"], values["NORDTILE_VERIFY_VERSION"] or None)
    out.write(json.dumps(result, separators=(",", ":")) + "\n")
    out.flush()
    return 0 if result["ok"] else 1


def serve(installed_archive=None, private_dir=None, stdin_fd=0, stdout_fd=1):
    here = os.path.dirname(os.path.abspath(__file__))
    sys.path.insert(0, here)
    if os.environ.get("NORDTILE_WORKER_TASK") == "verify":
        return verify_task(here, stdout_fd)
    import resolver  # noqa: E402  pinned SailTune entry (temporary storage policy)

    installed_archive = installed_archive or os.environ.pop("NORDTILE_WORKER_ARCHIVE", "") or os.path.join(here, "yt-dlp.pyz")
    private_dir = private_dir or os.environ.pop("NORDTILE_WORKER_DIR", "")
    out = os.fdopen(os.dup(stdout_fd), "w", encoding="utf-8", buffering=1)
    inp = os.fdopen(os.dup(stdin_fd), "rb", buffering=0)

    def reply(obj):
        out.write(json.dumps(obj, separators=(",", ":")) + "\n")
        out.flush()

    if not private_dir or not os.path.isabs(private_dir) or not os.path.isdir(private_dir):
        reply({"ready": False, "error": "worker directory unavailable"})
        return 2
    # Private session copy of the archive (NFR-ROB-04).
    loaded = os.path.join(private_dir, "yt-dlp-%d.pyz" % os.getpid())
    try:
        with open(installed_archive, "rb") as src, open(os.open(loaded, os.O_WRONLY | os.O_CREAT | os.O_TRUNC, 0o600), "wb") as dst:
            while True:
                block = src.read(1 << 20)
                if not block:
                    break
                dst.write(block)
    except OSError:
        reply({"ready": False, "error": "archive unavailable"})
        return 2
    installed_signature = _signature(installed_archive)
    import sysconfig
    roots = {os.path.realpath(sysconfig.get_path(key)) for key in ("stdlib", "platstdlib")}
    allowed = roots | {os.path.join(root, "lib-dynload") for root in roots}
    sys.path[:] = [loaded, here] + sorted(allowed)
    try:
        storage = resolver._TemporaryStorage()
    except (ImportError, OSError, ValueError, RuntimeError):
        reply({"ready": False, "error": "temporary storage unavailable"})
        return 2
    os.environ.pop("YTDLP_NO_LAZY_EXTRACTORS", None)
    os.environ.pop("QJS_DUMP_FLAGS", None)
    os.environ["SAILTUNE_QUICKJS_CHILD"] = "1"
    started = time.monotonic()
    try:
        import yt_dlp
        from yt_dlp.version import __version__ as version
    except ImportError:
        reply({"ready": False, "error": "runtime dependency unavailable"})
        return 2
    import_ms = int((time.monotonic() - started) * 1000)
    stdout_proxy, stderr_proxy = _Redirect(), _Redirect()
    sys.stdout, sys.stderr = stdout_proxy, stderr_proxy
    reply({"ready": True, "importMs": import_ms, "version": version, "archive": _digest(loaded)})

    while True:
        line = inp.readline(MAX_REQUEST_BYTES + 1)
        if not line:
            break
        try:
            request = json.loads(line.decode("utf-8"))
            rid, arguments = int(request["id"]), request["args"]
        except (ValueError, KeyError, TypeError):
            reply({"id": -1, "code": 2, "out": "", "err": "ERROR: malformed worker request.\n", "ms": 0, "skew": False})
            continue
        if len(line) > MAX_REQUEST_BYTES or not _valid(arguments):
            reply({"id": rid, "code": 2, "out": "", "err": "", "ms": 0, "skew": False})
            continue
        captured_out = io.TextIOWrapper(io.BytesIO(), encoding="utf-8", write_through=True)
        captured_err = io.TextIOWrapper(io.BytesIO(), encoding="utf-8", write_through=True)
        stdout_proxy.target, stderr_proxy.target = captured_out, captured_err
        sys.argv[:] = ["harbour-sailtube-resolver"] + arguments
        # The one-shot policy budgets temporary files per resolve; give each request the same budget.
        storage.attempts = 0
        t0 = time.monotonic()
        code = 0
        try:
            yt_dlp.main(arguments)
        except SystemExit as result:
            code = (0 if result.code is None else result.code
                    if isinstance(result.code, int) and 0 <= result.code <= 125 else 1)
        except Exception:
            # No exception text: it can contain media URLs or headers.
            captured_err.write("ERROR: the playback resolver failed unexpectedly.\n")
            code = 1
        if storage.failed:
            captured_err.write("ERROR: packaged resolver temporary storage limit exceeded.\n")
            storage.failed = False
            code = 2
        ms = int((time.monotonic() - t0) * 1000)
        reply({"id": rid, "code": code, "ms": ms,
               "out": captured_out.buffer.getvalue().decode("utf-8", "replace"),
               "err": captured_err.buffer.getvalue().decode("utf-8", "replace"),
               "skew": _signature(installed_archive) != installed_signature})
    try:
        os.unlink(loaded)
    except OSError:
        pass
    return 0


if __name__ == "__main__":
    sys.exit(serve())
