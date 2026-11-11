# Building

Target: Sailfish OS 5.1 SDK, aarch64 (installs on 5.1 and 5.2). Qt 5.6, C++11.

## Native loop (Apple Silicon)

```bash
tools/native_build.sh --jobs 8 --tests
```

Runs the shared SailTune tool (`tools/native_build.sh` of the SailTune checkout, override with
`SAILTUNE_NATIVE_BUILD=/path`) on a temporary copy rewritten for `sailtube.pro`. It snapshots what is on disk
into the `sfos-m1-native` container (the 5.1.0.11 aarch64 target with native gcc), runs qmake, make, lrelease
for the 10 catalogs and, with `--tests`, every `tests/*/run.sh`. Logs:
`~/Library/Caches/sailtune-native/<key>/`. The shared tool's TagLib/ICU staging is ignored by SailTube.
Not authoritative for deliveries.

## Tests

| Suite | Needs | Covers |
|---|---|---|
| `tests/domain/run.sh` | any C++11 compiler (runs on the Mac too) | stream selection, codec parsing, ids, MP4 index reader, MPD builder |
| `tests/resolver_qt/run.sh` | Qt5Core (container) | yt-dlp JSON parsers, URL allow-list, error classes, arguments, probe mapping |
| `tests/qml/run.sh` | Qt5Qml + Silica (container) for the load check; shell only for the gates | every QML file loads; ES5-only JS (PLT-05); ≤5 pulley items (PLT-06) |
| `tests/sharedplayback/run.sh` | C++11 compiler; Qt5Core + Qt5Network (container) for the TLS-fake parts | SailTune's own tests of the pinned shared playback component, run unchanged from `Shared/SailTune/tests/sharedplayback/` |
| `tests/privacy/run.sh` | python3 only (runs on the Mac too) | NFR-SEC-01: SailTune's checker self-test, the strict network inventory against `tests/privacy/network_allowlist.json`, and a scratch tree with an unlisted host that must fail |

## Packaging check

```bash
tools/rpm_build.sh
```

`rpmbuild --build-in-place` in the container with the arguments of SailTune's parity tool. It copies the
prepared Python-archive runtime stage (`vendor/python-runtime`, the same stage SailTune prepares with its
`scripts/build_python_runtime_sfos.py`; path in the container from `SAILTUBE_PYTHON_RUNTIME`) and writes the
RPM to `RPMS/` (ignored by git). `--without-runtime` builds a package that cannot resolve videos (spec checks
only). `mb2 build` with the same stage in `vendor/python-runtime` stays the authoritative delivery build.

## Shared snapshot

```bash
tools/shared_sync.py --check --upstream /path/to/sailtune
```

See [SHARING.md](SHARING.md).

## Icons

`tools/make_icons.py` (Pillow) regenerates `icons/*/harbour-sailtube.png` deterministically.

## Development resolver

`SAILTUBE_DEVELOPMENT_YTDLP=/path/to/yt-dlp` makes the app use an independently installed yt-dlp executable
instead of the packaged runtime (no JavaScript runtime). Development only.

## Release hygiene (NFR-MNT-03)

**Single source.** The release version is the `Version:` line of `rpm/harbour-sailtube.spec`: rpmbuild stamps
it on the package that ships, so the RPM cannot disagree with it. `VERSION` in `sailtube.pro` (compiled in as
`SAILTUBE_VERSION`) is a mirror and must be equal; bump both in the same commit. `Release:` counts rebuilds of
one version and is not part of the tag.

**Tags.** `v<version>` is a final release, `v<version>-<suffix>` (any tag with a hyphen, e.g. `v0.2.0-rc1`) a
pre-release. The part before the first `-` must equal the spec version at the tagged commit, and only commits
on `main` are tagged.

```bash
python3 -I tools/release_check.py version                       # prints 0.1.0; exit 1 if sailtube.pro disagrees
python3 -I tools/release_check.py tag v0.2.0-rc1 [--repo DIR] [--main-ref origin/main]
                                                                 # "release: prerelease" | "release: final"
python3 -I tools/release_check.py payload RPMS/a.rpm RPMS/b.rpm # exit 1 on any differing or missing file
```

`tag` reads the spec and `sailtube.pro` from the tagged commit with `git show` and checks ancestry with
`git merge-base --is-ancestor`; when `main` does not resolve it tries `origin/main` (a CI clone). `payload`
compares every payload path, its mode, symlink target and SHA-256 of the content (timestamps, inodes and owners
are ignored); gzip, bzip2 and xz payloads are read in Python, zstd ones need `rpm2cpio` or `zstd` on PATH.
Exit codes: 0 ok, 1 check failed, 2 usage or environment error.

**Where it runs.** The `tag` and `payload` checks belong to the release job: it needs the git history (a clone
with the tag and `main` fetched, not the `.git`-less build snapshot) and two independent aarch64 builds of the
tagged commit (armv7hl as well once that target is enabled) whose RPMs must have identical payloads. The
release job publishes the build as a pre-release when `tag` prints `release: prerelease`. `tests/release/run.sh`
(python3 only; the git cases are skipped without `git`) runs in every build and on the Mac: the real tree's
`version` check, a scratch copy with a mismatching `sailtube.pro`, tag cases in a temporary git repository
(matching tag on main, mismatching tag, side-branch commit, `-rc1` pre-release, `origin/main` fallback) and the
payload comparison on synthetic newc archives and minimal synthetic RPMs.

## Coverage ratchet (NFR-TST-06)

```bash
python3 -I tools/coverage_ratchet.py check                  # every build (tests/coverage/run.sh)
python3 -I tools/coverage_ratchet.py update                 # after adding tests: drop entries that are tested now
python3 -I tools/coverage_ratchet.py growth --base origin/main   # host / release job (needs git history)
python3 -I tools/coverage_ratchet.py list                   # every engine function, tested or not
```

An *engine function* is a free function or a public method (inline, static, virtual and `Q_INVOKABLE`
included; constructors, destructors, operators, signals, private/protected members excluded) declared in a
header under `Core/**` or `Shared/Playback/**`; `Shared/SailTune/**` and `Shared/Runtime/**` are not scanned.
Entries read `<header>::<Class>::<name>` or `<header>::<name>`; overloads share one entry. A function is
*tested* when its bare name occurs as an identifier in a file under `tests/**` (comments of C++/QML/JS files
removed, string contents kept; `tests/coverage/**` and `tests/fixtures/*.json(l)` not searched).
`tests/coverage/baseline.txt` lists the engine functions that are untested today and may only shrink.

`check` fails on an untested function missing from the baseline (add a test) and on a baseline entry that is
tested now or no longer exists (remove it: `update`). `growth` fails when the working-tree baseline has an entry
the baseline at `--base` lacks, so listing a new function instead of testing it is caught too; it needs a clone
with history, not the `.git`-less build snapshot. `tests/coverage/run.sh` (python3 only; the git case is skipped
without `git`) runs `check` on the tree and the failing and passing cases in a scratch copy and a temporary git
repository. The full definitions are in the tool's docstring.

## Offline engine suite (NFR-TST-02)

`tests/offline_engine/run.sh` runs the real fetchers against fake media hosts on loopback with networking
disabled, and exits non-zero on any failure (last line `offline_engine: PASS` on success). It is skipped (still
PASS) on non-Linux hosts and without `cc`, `c++`, `pkg-config`, Qt5Core, Qt5Network, Qt5Sql or `moc`.

**Networking disabled without a namespace.** The acceptance text asks for an empty network namespace, but the
build container refuses `unshare -n` / `unshare -rn`. Instead, every fetcher test runs under
`tests/offline_engine/no_network.c`, built as `no_network.so` and loaded with `LD_PRELOAD`. It wraps
`connect()`, `sendto()` and `sendmsg()` and fails them with `ENETUNREACH` for any destination that is not
loopback (127.0.0.0/8, `::1`, `::ffff:127.x.y.z`) or a local socket (AF_UNIX, AF_NETLINK pass), and makes
`getaddrinfo()` fail (`EAI_SYSTEM`, errno `ENETUNREACH`) for any non-numeric name other than `localhost` /
`*.localhost`. Each refusal is printed to stderr and counted; with `NO_NETWORK_LOG=<file>` one line per refusal
is appended there. The suite proves the guard first: `probe.c` must be refused for TCP to `1.1.1.1:443` and
`[2606:4700:4700::1111]:443`, UDP to `8.8.8.8:53` and a lookup of `example.com`, while loopback TCP, `localhost`
and AF_UNIX still work. Inside the Qt test process a `QTcpSocket` to `1.1.1.1:443` must also be refused
(`guardCanary`), so the guard is known to sit under Qt's socket layer. After the fetcher runs the refusal log
must be empty: no test tried to leave the machine. Limits: the guard covers dynamically linked libc calls
only (a static binary or a raw `syscall()` would bypass it); a CI runner that can create namespaces may wrap
the whole suite in `unshare -rn` as well.

**What runs, all under the guard.** SailTune's pinned shared playback tests unchanged from the pin
(`Shared/SailTune/tests/sharedplayback/run.sh`, including `MediaProxyTest` against its local TLS media host);
the download engine suite (`tests/downloads_engine/run.sh`); and `OfflineEngineTest`, one named test per case
against the real `DownloadManager` / `DownloadEngine` and a fake googlevideo on 127.0.0.1:

| case | OfflineEngineTest | also covered by |
|---|---|---|
| Range | `caseRange` | `MediaProxyTest`: `testServing`, `testSeekMidStream`; `DownloadsEngineTest`: `audioEndToEnd`, `restartResume` |
| 403 at byte 0 | `case403AtByteZero` | `testEarlyRefusalIsRetriedInPlace`; `resumeFaults` |
| young-token window | `caseYoungTokenWindow` (inside the 10 s window: two in-place retries; retries spent or address 30 s old: re-resolve) | `testRefusalPersistsThenRefreshesAndGivesUp` |
| truncation and reopen | `caseTruncationAndReopen` (exactly five ranges, in order; with and without a Content-Length; a cut request may reach the host twice, see below) | `testDroppedConnections`; `resumeFaults` |
| `416 */total` | `case416StarTotal` (restart from byte 0, exact `clen`) | `testServing` (the proxy answers `416` with `bytes */total`) |
| 503 when space is low | `case503AndLowSpace` | `testLimitsAndClose` (the proxy's `503` when no stream slot is free) |

`caseTruncationAndReopen` compares the ranges the fake host saw, without exact repeats, with the five the engine
must ask for. A repeat is allowed only for a request whose answer the host cut: Qt 5.6's HTTP layer sometimes
re-sends such a request by itself, and the case prints a note
when it does. `OFFLINE_ENGINE_ONLY=<case>` runs one case and `OFFLINE_ENGINE_REPEAT=<n>` repeats the truncation
scenario n times in one process, for diagnosis.

The download engine checks free space before each track (NFR-SEC-05 G4); nothing answers 503 because of it.
`case503AndLowSpace` covers an upstream `503` (transient: same address, same offset), low space (Failed
`no_space` before any media request, and a retry finishes once there is room), and a symlink to `/dev/full`
planted at the part name, which is replaced rather than written through (G1), so the item finishes intact.

**Mutation check.** In scratch copies the suite removes the 416 handling and rebuilds: `MediaProxy.cpp`
without its `respondError(job, 416, ...)` branch must make `MediaProxyTest` fail on its 416 check, and
`DownloadPolicy.cpp` without `if (s == 416) return ResumeAction::RestartFromZero;` must make
`case416StarTotal` fail (the item ends Failed `http`). Pinned files are only copied, never edited; if either
line changes, the suite fails and asks for `run.sh` to be updated.
