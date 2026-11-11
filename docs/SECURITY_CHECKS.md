# Security checks (NFR-SEC-03, NFR-SEC-04)

What the automated checks for secrets handling (NFR-SEC-03, with ACC-10) and
runtime downloads (NFR-SEC-04) prove, where they stop, and what can only be
checked on a device.

| Suite | Needs | Runs on | Covers |
|---|---|---|---|
| `tests/secrets/run.sh` | python3, optional Qt5Core/Network/Sql, a C compiler on Linux | Mac: static part only. Linux container: everything | NFR-SEC-03 |
| `tests/runtime_downloads/run.sh` | python3 | Mac and container | NFR-SEC-04 (STORE part, and the "no download path today" part of FULL) |

Both print `<suite>: PASS` as their last line on success. They work from a
snapshot without `.git`, and their temp files go under `${TMPDIR:-/tmp}` and
are removed by a trap.

```sh
sh tests/secrets/run.sh
sh tests/runtime_downloads/run.sh
python3 -I tests/secrets/check_secret_logging.py [--root DIR] [--verbose]
python3 -I tools/check_runtime_downloads.py [--root DIR] [--variant full|store] [--verbose]
```

`--verbose` on the runtime-download check lists every launch, load and
search-path site it followed and the origin it found for each. Read that list
when you review a change to the worker, the runner or `main.cpp`.

## NFR-SEC-03: secrets

### 1. Static logging check (`tests/secrets/check_secret_logging.py`)

**Scope.** The check reads `Core/Account/**`,
`Clients/Sailfish/Presentation/AccountController.*`,
`Core/Resolver/Infrastructure/YtDlpRunner.*`, `Core/Worker/**` and the pinned
`Shared/SailTune/Core/Accounts/**`. If one of these patterns matches no file,
the check fails, so a moved file cannot make the scan pass by being skipped.

**What counts as a logging call.**
- C++: `qDebug/qInfo/qWarning/qCritical/qFatal` and the `qC*` forms (printf
  style and stream style); `printf/fprintf/fputs/puts/perror/syslog`;
  `std::cout/cerr/clog <<`; `QTextStream(stdout|stderr) <<`.
- Python: `print`, `sys.std{out,err}.write`, `logging`/logger methods,
  `os.write(1|2, …)` and `warnings.warn`.
- QML/JS: `console.*` and `print`.

**What it proves.** No logging call in that code formats an identifier whose
words (split at camelCase and snake_case) include a secret word: `cookie`,
`jar`, `token`, `sapisid`, `apisid`, `authorization`, `secret`, `password`,
`credential`, `sid`, `sidcc`, `psid`, `hsid`, `ssid`, `bearer` or `blob`. No
logging call formats an argument list (`arguments`, `args`, `argv`), which
carries `--cookies <jar>`, and none contains the literal `--cookies`. Comments
and the contents of string literals are ignored, so a message like "cannot
write the extractor cookie jar" is fine.

**Seeded failures.** `run.sh` adds 8 leaks to a scratch copy and checks that
each one fails with the expected file and identifier:
- a jar path in `AccountStore`;
- a cookie value in `AccountService`;
- the argument list and `--cookies` in `YtDlpRunner`;
- a token in `AccountController`;
- a Python `print` in `worker.py`;
- QML `console.log` in `Worker.qml`;
- a signing secret in the pinned `SessionSigner`.

**Limits.** The check matches names; it does not follow data. A secret copied
into a variable with a neutral name (`x`) and then logged passes the static
check. The runtime test below catches that case for the code paths it drives.
Logging in files outside the scope is not checked.

### 2. Runtime test (`tests/secrets/SecretsTest.cpp`)

The account runs the way the app wires it (see `main.cpp`):
- `AccountStore`, `AccountService` and `AccountController`, with the pinned
  paste parser;
- `YtDlpRunner` with the per-call cookie hooks.

Only the network and the extractor are replaced:
- a fake `AuthTransport` returns the documented shapes, plus `Set-Cookie`
  rotations;
- a shell script stands in for yt-dlp.

Every cookie value is synthetic (`synthetic-<NAME>-<uuid>`) and unique to the
run. The test treats each of these as a secret:
- every imported value;
- the values in a refused paste;
- the pasted SAPISIDHASH digest;
- every rotated value;
- every SAPISIDHASH digest the service computes (read from the fake transport).

**Steps:**
1. A refused paste with no signing cookie.
2. Sign-in from a picked file: copied request headers with an Authorization
   line, FULL build, no vault, so the session goes to the 0600 file fallback.
3. Verification, with a rotation from the `account_menu` answer.
4. The disclosure is acknowledged.
5. Switch to the brand channel.
6. A signed `browse`, with a rotation.
7. Extractor calls through the runner:
   - a success, where the fake extractor rewrites `SIDCC` in the jar and the
     value folds back;
   - a failure (exit 1, with the jar named on stderr);
   - a timeout (the process group is killed);
   - a cancel;
   - three calls at once;
   - the runner destroyed during a call.
8. Direct `begin`/`end` calls with a damaged jar and with a vanished jar.
9. A backup export, from an INI that holds credential-looking keys and values.
10. A diagnostics report: the session's values go in `secrets`, and the logs,
    the extractor results, a full `Cookie:` header and an `Authorization:`
    line go in the free-text fields.
11. Sign-out.
12. With a fake vault: browser import from a generated Sailfish Browser
    database, one extractor call, then sign-out.

**After every step**, a snapshot of the whole test root (listing, modes and the
content of every file) must show:
- Secret-bearing files only inside the private dir (`data/account`).
- Each such file at mode 0600.
- Every directory in the private dir at 0700.
- Secret-bearing files only as `session.json` (while signed in on the file
  fallback) or as the jar of a call that is still open. A jar left behind is
  reported.
- Nothing in `TMPDIR` but the runner's per-call directories, which must be
  gone at the end.
- After sign-out, no secret in any file.

Files the user owns, such as the picked file and the browser's database, are
not scanned.

**Inside the extractor**, the fake prints the jar's mode and whether the jar is
under the private dir. The test requires `600` and `yes`. It also requires
`--cookies [cookies]` in the run's diagnostics: the runner scrubs the jar path.

**Logs and outputs.** The test captures all Qt messages
(`qInstallMessageHandler`, with debug enabled), the controller messages, every
extractor output and diagnostic, the report and the backup. None may contain
any secret. The captured log may contain neither `--cookies` nor the jar
directory.

**File-system trace (Linux).** `run.sh` builds `tests/secrets/fs_trace.so` and
runs the test with `LD_PRELOAD`. The shim records the test process's file
calls under the test root:
- creates, with their mode after the umask;
- the first write to a file whose mode grants group or other access;
- every chmod, with the file's size at that moment;
- renames.

The test then requires:
- no byte written to a file under the private dir or `TMPDIR` while that file
  was readable by group or other;
- no chmod of a non-empty file there to such a mode;
- no file created outside the private dir, apart from test-owned inputs and the
  backup target;
- no rename across the private dir boundary.

This catches files that exist only for a moment, which a snapshot cannot see.
A file created with a wider mode and narrowed before its first byte is printed
as a `note:` and does not fail the test. That is how QSaveFile writes
(0666 & ~umask, then `setPermissions(0600)` before any data), and no secret is
ever readable that way. The test sets `umask(022)`, the device session's value,
so that the code cannot pass by relying on a stricter umask.

**Sensitivity.** Run in a throwaway copy, each of these mutations makes the
test fail:
- the jar is not deleted;
- the jar content is logged through a neutrally named variable, which the
  static check cannot see;
- the jars go under `TMPDIR`;
- the runner does not scrub the jar path;
- `redact()` ignores `secrets`;
- the backup's settings filter is disabled.

**Limits.**
- The vault is a fake. The real `SailfishCredentialVault` is never exercised.
- The warm-worker path is not driven. With `RunnerOptions::warm` set, the
  arguments, including `--cookies`, go through `WarmResolver` to `worker.py`;
  the static check covers that code, this test does not.
- Logs are Qt messages from this process only. What the real yt-dlp prints is
  not seen.
- The trace sees only libc file calls made by this process (not its children,
  not direct syscalls). It needs glibc and `LD_PRELOAD`. On the Mac the Qt
  part is skipped, because no Qt is installed there.

### Current findings (component code, not fixed here)

The runtime test fails on these. Both are in the account component:

1. **`Core/Account/Infrastructure/BrowserCookieImport.cpp:47`**:
   `QDir().mkpath(scratchRoot_)` creates the scratch directory
   (`<private>/scratch`) with the process umask (0755) and never narrows it.
   - On a first sign-in by browser import with the vault available, nothing has
     created the private dir yet, so the same call also creates
     `data/account` at 0755.
   - `AccountStore` narrows the private dir to 0700 later, the first time it
     writes a jar.
   - The `scratch` directory stays 0755.

   The test reports this as "private dir … 0755" and "directory
   data/account/scratch … 0755".
2. **`Core/Account/Infrastructure/BrowserCookieImport.cpp:21-22`**:
   `copyPrivate()` uses `QFile::copy()`, and Qt 5.6 gives the copy the
   *source's* permissions (the Browser's database is typically 0644).
   - Only after that does `setPermissions(0600)` run, so the full cookie
     database sits at 0644 for a moment.
   - The containing `browser-*` directory is already 0700 at that point.
   - Separately, if `QFile::copy()` cannot create its temp file in the target
     directory, it falls back to `QDir::tempPath()`, which would put the
     database in a shared location.

   The trace reports this as "cookies.sqlite (8192 bytes) was given mode
   0644".

With both fixed in a throwaway copy, the whole suite passes. The fix used
there:
- create the directories with `umask(077)`, or narrow each one that is
  created;
- write the copy through a file opened `O_CREAT|O_EXCL` at 0600.

So the test has no other false positives.

### NFR-SEC-03 on the device

- **Real vault.** Sailfish Secrets stores and erases the session; nothing is
  written to disk. Checked with `find ~/.local/share/harbour-sailtube` after
  sign-in and after sign-out.
- **Real logs.** Scan `journalctl --user` / `devel-su journalctl` during
  sign-in, playback with the cookie rung, rotation and sign-out, for the
  imported values. This also covers QML `console` output, PyOtherSide and the
  resolver child's stderr.
- **Real yt-dlp jar writes.**
  - Does yt-dlp save the jar in place, or through a temp file and a rename?
    A temp file would be created inside the 0700 jar directory with the umask
    mode.
  - Run `ls -la` on the jar directory (`<private>/jars/<id>/`) during a call.
  - Confirm nothing stays behind after the call.
- **Warm worker with cookies.** The `--cookies` argument goes through the
  worker protocol pipe and nowhere else. `/proc/<worker>/cmdline` does not
  carry the jar path.
- **System backup.** Does the Sailfish system backup (or a user backup tool)
  include `~/.local/share/harbour-sailtube/account/session.json` on the FULL
  file fallback? If it does, that is a copy of the session outside the app's
  control. The app's own SailTube backup excludes it; this test checks that.
- **Crash dumps.** Core dumps of the app (systemd-coredump) may contain the
  session in memory. That is outside the app, but worth documenting.

## NFR-SEC-04: runtime downloads

### Static check (`tools/check_runtime_downloads.py`)

The only runtime code update is NFR-MNT-01 (FULL): the verified extractor archive, a data file that is
imported, never executed. Everything else stays strict: any code path
in shipped code (Core, Clients and Shared, minus test directories, plus the
`rpm/*.spec` scriptlets) that could fetch, stage, unpack, mark executable, load
or run code from a writable location is a finding.

**Rules:**

| Rule | Finds |
|---|---|
| RD-URL | release/asset download URLs (GitHub releases, `*.githubusercontent.com` asset hosts, codeload, PyPI file hosts, `/releases/download/`) in code (not in comments) |
| RD-FLAG | yt-dlp `--update`, `-U`, `--update-to`, `--remote-components`, `--plugin-dirs` literals |
| RD-GUARD | `YtDlpRunner::commonArguments()` lost `--ignore-config`, `--no-plugin-dirs` or `--no-remote-components` (yt-dlp would otherwise load plugins or fetch its JS challenge solver from GitHub) |
| RD-EXEC-MODE | an execute bit on a file: Exe* flags, `S_IX*`, `S_IRWX*`, an octal mode with x bits, in setPermissions/chmod/fchmod/open/creat, `os.chmod`/`os.open`, `chmod +x` |
| RD-CODE-WRITE | a write, copy, rename or extract whose target names a code/executable file (`.pyz .py .so[.N] .zip .whl .tar* .js .qml .sh .bin .rpm …`); any write into the runtime directory |
| RD-EXTRACT | archive extraction to disk (libarchive, QuaZip, KArchive, zipfile/tarfile extract, `shutil.unpack_archive`) |
| RD-PROC | a process launch whose program is not from the install |
| RD-DLOPEN | dlopen/QLibrary/QPluginLoader/ctypes on anything but a bare system soname or an install path |
| RD-LOADPATH | code search paths from a non-install origin: QML import and plugin paths, library paths, `LD_LIBRARY_PATH`/`LD_PRELOAD`/`PYTHON*`/`QML*_IMPORT_PATH`/`QT_PLUGIN_PATH`, `NORDTILE_WORKER_ARCHIVE`, `sys.path`, `site.addsitedir` |
| RD-PYLOAD | zipimport, `spec_from_file_location`, `runpy.run_path`, exec/compile of non-literal code |
| RD-QMLREMOTE | `Qt.include`/`createComponent` of a remote or non-literal source, `createQmlObject` of a non-literal, `eval`/`Function` in QML/JS |
| RD-SCRIPTLET | `curl`/`wget`/`git`/`pip install` in RPM scriptlets |

**Origins.** For launches, loads and search paths, the check follows the path
expression through:
- local definitions;
- function parameters (a parameter inside a pass-through launcher such as
  `startHelper` is fine, because its callers are checked as sites of their
  own);
- for `x.field`, every assignment to that field anywhere in the tree (this is
  how `options_.executablePath` is followed to `main.cpp`).

Only the *install* origin passes:
- `applicationFilePath`, `runtimeFile()`, `NORDTILE_RUNTIME_ROOT`, `QLibraryInfo`
  or `Qt.resolvedUrl`;
- `__file__` or `sysconfig` in Python;
- `/usr` literals.

Everything else fails:
- *writable*: QStandardPaths, temp or home paths, `~`, XDG;
- *env*: getenv/qgetenv/os.environ;
- *unknown*: anything the check cannot resolve.

Today the check verifies, for example:
- the worker's archive (`NORDTILE_WORKER_ARCHIVE`) comes from
  `runtimeFile("runtime/yt-dlp.pyz")`;
- the warm worker program and `HelperProcess` callers run the app binary;
- QuickJS is loaded from `NORDTILE_RUNTIME_ROOT`;
- the Python children's library and QML paths come from `QLibraryInfo`.

**Exceptions** (`tests/runtime_downloads/exceptions.json`, each with a reason):

| File | Rule | Why |
|---|---|---|
| `Core/Worker/Runtime/worker.py` (`os.open(loaded, …)`) | RD-CODE-WRITE | NFR-ROB-04: the warm worker copies the **bundled** `yt-dlp.pyz` (install origin, verified) into its private 0700 work dir at 0600 and deletes it on exit, so an update of the installed file mid-session cannot break zipimport. |
| `Core/Worker/Runtime/worker.py` (`sys.path[:] = [loaded, here] + …`) | RD-LOADPATH | The same private copy goes first on `sys.path`. It lives in the cache-location work dir, hence the writable/env origin; it is a byte copy of the install. |
| `Core/Resolver/Infrastructure/YtDlpRunner.cpp` (`process->start(options_.executablePath`) | RD-PROC | The program is the app binary, except for the development override `SAILTUBE_DEVELOPMENT_YTDLP`, which a developer sets in their own environment. Nothing downloads or writes that file. |
| `Core/Downloads/Infrastructure/GstRemuxer.cpp` (`QLibrary gst(gstLibrary, 0)`) | RD-DLOPEN | The library name passes through the constructor. The only shipped construction uses the default `"gstreamer-1.0"`, a bare soname. |

How the exception list behaves:
- An exception that no longer matches a finding fails the check, so the list
  stays current.
- `--variant store` ignores exceptions marked `"variant": "full"`. When the
  FULL-only updater (NFR-MNT-01) arrives, it is declared as FULL-only, and the
  STORE run then proves that the Harbour build has no runtime-download path.

**Seeded failures.** `tests/runtime_downloads/run.sh` runs the check on the
tree (FULL and STORE). It then runs it on scratch copies with 16 seeded
violations, each of which must fail with the expected rule and file:
- a downloaded body written to `.pyz`;
- a `.so` staged through a variable;
- C++ and Python chmod +x;
- a QProcess and a `subprocess` run from cache/home;
- `sys.path` from `$HOME/.cache`;
- QLibrary from the data dir;
- `PYTHONPATH` set to the data dir;
- a GitHub release URL;
- `--update-to` and `--remote-components`;
- the runner losing `--no-remote-components`;
- zipfile `extractall`;
- a remote `Qt.createComponent`;
- JS `eval`;
- a FULL-only exception that is rejected under `--variant store`.

The run also checks that look-alikes pass (a URL in a comment, a 0700
directory, a `.db` data file), and that a stale exception fails.

**Limits.** This is a lexical and AST check, not a data-flow analysis:
- A download hidden behind an indirection the check does not follow fails as
  "unknown" (safe). But a launch through a wrapper whose name is not
  `startHelper`/`start`, or whose receiver name does not look like a process,
  is not seen.
- Writes are flagged by target name, so an executable written under a
  data-looking name and then run is caught only at the launch or load site.
- The check proves nothing about what yt-dlp does inside the bundled archive.
  `--no-remote-components`, `--no-plugin-dirs` and `--ignore-config` are
  enforced, and the worker strips user site paths and `PYTHONPATH`.
- yt-dlp writes YouTube's player JavaScript into the per-call 0700 temp
  directory and runs it in the packaged QuickJS child
  (`Shared/Runtime/Child/QuickJsChild.cpp`, `--script <file>`). That is
  untrusted *data* interpreted in a sandboxed runtime as part of extraction,
  not helper code. The interpreter itself comes from the install. This is
  accepted by design and not flagged.
- The worker's yt-dlp `--cache-dir` (cache location) holds yt-dlp's cached
  data, not code.

### FULL fake-release-host tests (NFR-MNT-01, built)

The extractor updater (`Core/Extractor`, FULL only, `docs/EXTRACTOR_UPDATE.md`) has its suite in
`tests/extractor_update`: against a fake HTTPS release host it refuses an http URL, a redirect to an unlisted
host, an invalid certificate, a wrong SHA-256, a missing signature, a bad signature (wrong key, tampered sums,
EdDSA), `../` and absolute archive entries, an oversize or grown archive, and an install without the offered
{version, size, source}; aborts at every step (cancel, lost connection, crash mid-download, verifier failure or
hang, crash mid-swap) keep the old copy; rollback and the fallback to the bundled archive work; the verifier
never imports or executes the archive (marker-file proof); the real 2026.10.26 sums and signature verify with the
pinned yt-dlp key (cross-checked with gpgv). The updater's files are `"variant": "full"` exceptions and are
compiled only in a `!sailtube_store` block, so `--variant store` stays clean. Device counterpart: §6 step I4.

### NFR-SEC-04 on the device

- **RPM.** The installed RPM's files under `/usr/share/harbour-sailtube` are
  root-owned and not writable by the user (`rpm -V`, `ls -l`). The install
  origin the check trusts relies on this.
- **Writable locations.** During playback, downloads and a worker restart, no
  executable file appears under `~/.local/share/harbour-sailtube` or
  `~/.cache/harbour-sailtube`:
  `find … -type f -perm /111` finds nothing, and the only `.pyz` is the
  worker's private copy, which matches the installed archive's SHA-256
  (reported as `archive` in the worker's ready line).
- **Network trace.** No request goes to GitHub or PyPI hosts
  (`docs/PRIVACY_NETWORK.md`, NFR-SEC-01).
