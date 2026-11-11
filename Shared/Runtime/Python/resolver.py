# SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
"""Package-owned resolver called by PyOtherSide in a dedicated child process."""
import errno
import os
import sys
import sysconfig
import time


class _TemporaryStorageError(RuntimeError):
    pass


class _TemporaryStorage:
    """Bound the pinned provider's tempfile path, not arbitrary third-party IO."""
    def __init__(self):
        import resource
        import stat
        import tempfile

        self.failed = False
        self.attempts = 0
        self.verified = False
        self.directory = os.environ.pop("SAILTUNE_RESOLVER_TMPDIR", "")
        directory = self.directory
        if (not directory or not os.path.isabs(directory)
                or os.path.realpath(directory) != directory):
            raise _TemporaryStorageError()
        fd = os.open(directory, os.O_RDONLY | os.O_DIRECTORY | os.O_NOFOLLOW | os.O_CLOEXEC)
        try:
            info = os.fstat(fd)
        finally:
            os.close(fd)
        if (not stat.S_ISDIR(info.st_mode) or info.st_uid != os.geteuid()
                or stat.S_IMODE(info.st_mode) != 0o700):
            raise _TemporaryStorageError()
        self.identity = (info.st_dev, info.st_ino)
        self.directory_mode = info.st_mode
        # No default-directory probing or fallback into shared /tmp. The
        # parent owns this fresh directory and cleans it only after group exit.
        tempfile.tempdir = directory
        os.environ["TMPDIR"] = directory
        os.environ.pop("TMP", None)
        os.environ.pop("TEMP", None)
        # This runs inside the dedicated child, never in the GUI process.
        # Preserve a tighter inherited limit; never raise either limit.
        maximum = 32 * 1024 * 1024
        soft, hard = resource.getrlimit(resource.RLIMIT_FSIZE)
        hard = maximum if hard == resource.RLIM_INFINITY else min(hard, maximum)
        soft = maximum if soft == resource.RLIM_INFINITY else min(soft, maximum)
        resource.setrlimit(resource.RLIMIT_FSIZE, (min(soft, hard), hard))
        sys.addaudithook(self._audit)
        # Existing platform hooks may reject installation without propagating
        # an exception. Verify activation without creating a probe file.
        sys.audit("sailtune.resolver_temp_policy")
        if not self.verified:
            raise _TemporaryStorageError()

    def _audit(self, event, arguments):
        if event == "sailtune.resolver_temp_policy":
            self.verified = True
            return
        if event not in ("tempfile.mkstemp", "tempfile.mkdtemp"):
            return
        # CPython 3.11 audits before each creation attempt, including name
        # collisions. Four attempts is a policy budget, not a measured count.
        allowed = event == "tempfile.mkstemp" and self.attempts < 4
        if allowed:
            try:
                path = os.fsdecode(arguments[0])
                current = os.lstat(self.directory)
                allowed = (os.path.isabs(path) and os.path.dirname(path) == self.directory
                           and (current.st_dev, current.st_ino) == self.identity
                           and current.st_mode == self.directory_mode
                           and current.st_uid == os.geteuid())
            except (OSError, ValueError, TypeError, IndexError):
                allowed = False
        if not allowed:
            self.failed = True
            raise _TemporaryStorageError("packaged resolver temporary storage limit exceeded")
        self.attempts += 1


class _RuntimeTimings:
    """Best-effort bounded child measurements, with no filesystem socket."""
    def __init__(self, enabled):
        self.channel = None
        self.thread = None
        os.environ.pop("SAILTUNE_TIMING_SOCKET", None)
        if not enabled:
            return
        try:
            import socket
            import threading
            self.counts = {stage: 0 for stage in (
                b"quickjs_help", b"quickjs_script", b"quickjs_library", b"quickjs_script_cpu")}
            self.stopped = threading.Event()
            self.channel = socket.socket(socket.AF_UNIX, socket.SOCK_DGRAM)
            self.channel.settimeout(0.05)
            token = os.urandom(16).hex()
            self.channel.bind(b"\0sailtune-phase-" + token.encode("ascii"))
            self.thread = threading.Thread(target=self._receive, daemon=True)
            self.thread.start()
            os.environ["SAILTUNE_TIMING_SOCKET"] = token
        except (ImportError, OSError, RuntimeError):
            self.close()

    def _forward(self, record):
        fields = record[:-1].split(b" ") if record.endswith(b"\n") else []
        if (len(fields) != 3 or fields[0] != b"SAILTUNE_PHASE_V1"
                or fields[1] not in self.counts or not 1 <= len(fields[2]) <= 6
                or not fields[2].isdigit() or int(fields[2]) > 120000
                or self.counts[fields[1]] >= 32):
            return
        self.counts[fields[1]] += 1
        os.write(2, record)

    def _receive(self):
        channel = self.channel
        if channel is None:
            return
        received = 0
        while not self.stopped.is_set() and received < 128:
            try:
                record = channel.recv(128)
                received += 1
                self._forward(record)
            except TimeoutError:
                continue
            except OSError:
                return

    def close(self):
        os.environ.pop("SAILTUNE_TIMING_SOCKET", None)
        if self.thread is not None and self.thread.ident is not None:
            self.stopped.set()
            self.thread.join(0.2)
        if self.channel is not None:
            try:
                if self.thread is not None and not self.thread.is_alive():
                    self.channel.setblocking(False)
                    for _ in range(64):
                        self._forward(self.channel.recv(128))
            except OSError:
                pass
            self.channel.close()
            self.channel = None


def _timing(stage, start, enabled):
    if enabled:
        elapsed = max(0, min(120000, int((time.monotonic() - start) * 1000)))
        try:
            # One small atomic pipe write; never include arguments or output.
            os.write(2, ("SAILTUNE_PHASE_V1 %s %d\n" % (stage, elapsed)).encode("ascii"))
        except OSError:
            pass


def run(arguments):
    if sys.version_info < (3, 11):
        sys.stderr.write("ERROR: unsupported Python runtime; Python 3.11 or newer is required.\n")
        return 2
    if not sys.flags.no_user_site or not sys.flags.dont_write_bytecode:
        sys.stderr.write("ERROR: packaged resolver runtime startup policy is unavailable.\n")
        return 2
    if (not isinstance(arguments, list) or not 1 <= len(arguments) <= 63
            or any(not isinstance(arg, str) or len(arg.encode("utf-8")) > 8192 for arg in arguments)
            or sum(len(arg.encode("utf-8")) for arg in arguments) > 65536):
        return 2
    # The native host removes Python environment overrides and disables user
    # site imports before plugin initialization. Now exclude site packages and
    # the working directory as resolution inputs for yt-dlp. System startup
    # hooks have already run under PyOtherSide: this is not Python's -I -S mode.
    archive = os.path.join(os.path.dirname(os.path.abspath(__file__)), "yt-dlp.pyz")
    roots = {os.path.realpath(sysconfig.get_path(key)) for key in ("stdlib", "platstdlib")}
    allowed = roots | {os.path.join(root, "lib-dynload") for root in roots}
    sys.path[:] = [archive] + sorted(allowed)
    try:
        storage = _TemporaryStorage()
    except (ImportError, OSError, ValueError, RuntimeError):
        sys.stderr.write("ERROR: packaged resolver temporary storage is unavailable.\n")
        sys.stderr.flush()
        return 2
    os.environ.pop("YTDLP_NO_LAZY_EXTRACTORS", None)
    os.environ.pop("QJS_DUMP_FLAGS", None)
    os.environ["SAILTUNE_QUICKJS_CHILD"] = "1"
    sys.argv[:] = ["harbour-sailtune-resolver"] + arguments
    timing_enabled = os.environ.get("SAILTUNE_STREAM_TIMING") == "1"
    timings = _RuntimeTimings(timing_enabled)
    import_started = time.monotonic()
    resolution_started = None
    exit_code = 0
    try:
        from yt_dlp import main as resolve
        _timing("python_import", import_started, timing_enabled)
        resolution_started = time.monotonic()
        resolve(arguments)
    except SystemExit as result:
        exit_code = (0 if result.code is None else result.code
                     if isinstance(result.code, int) and 0 <= result.code <= 125 else 1)
    except ImportError:
        sys.stderr.write("ERROR: packaged resolver runtime dependency is unavailable.\n")
        exit_code = 2
    except OSError as error:
        # Inspect only the numeric error, never its message or filename. These
        # errors can arise while creating, writing or closing a script file.
        if error.errno in (errno.ENOSPC, errno.EDQUOT, errno.EFBIG):
            storage.failed = True
            exit_code = 2
        else:
            sys.stderr.write("ERROR: the playback resolver failed unexpectedly.\n")
            exit_code = 1
    except Exception:
        # Preserve no exception text: it can contain media URLs or headers.
        sys.stderr.write("ERROR: the playback resolver failed unexpectedly.\n")
        exit_code = 1
    finally:
        if resolution_started is not None:
            _timing("resolver_run", resolution_started, timing_enabled)
        timings.close()
    if storage.failed:
        sys.stderr.write("ERROR: packaged resolver temporary storage limit exceeded.\n")
        exit_code = 2
    sys.stdout.flush()
    sys.stderr.flush()
    return exit_code
