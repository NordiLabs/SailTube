# Sharing code with SailTune (ADR)

Status: accepted for phase 1, 2026-11-08. Owner of both projects: the same author; both are PolyForm
Noncommercial 1.0.0, so code moves between them without licence friction.

## Context

SailTune already has, hardened and tested on device: the resolver process boundary (PyOtherSide child running
the pinned yt-dlp zipimport release, private QuickJS-NG child, temp-storage audit hook, process groups,
output caps), the runtime preparation scripts and manifests, an output validator, catalog adapters for the
anonymous InnerTube WEB_REMIX protocol, a session-import parser and SAPISIDHASH signer, a fail-closed Secrets
vault, pure recovery/prefetch policies with host tests, and the native M1 build tooling. The requirements draft
(§1.6, G6) asks SailTube to reuse these rather than re-derive them, with changes (warm resolver, video formats).

SailTube is a separate repository. SailTune's tree is being changed by several parallel efforts right now.

## Options

| Option | For | Against |
|---|---|---|
| A. Copy-paste into SailTube | Fast; no coordination. | Silent divergence; fixes (security bounds, provider changes) must be ported by hand and get forgotten. |
| B. SailTune as a git submodule, compile selected files | One source of truth. | Pulls a whole app (and its churn) into SailTube; SailTune file paths/namespaces become SailTube API; hard-coded `harbour-sailtune` paths. |
| C. Extract a shared library repository (`nordtile-yt-runtime`) used as a submodule by both | Clean API, one source, versioned. | Requires restructuring SailTune while that work is active; premature before SailTube shows what it needs (warm worker, video formats). |
| D. **Pinned snapshot now, extraction (C) next** | Works today without touching SailTune; drift is detected mechanically; the snapshot doubles as the extraction's file list. | Two copies until C lands; one small patch to carry. |

## Decision

**D.** `Shared/` holds exact copies of SailTune files (runtime at `e190ac2`, re-pinned `5a98381`; shared playback at `fd49627`,
re-pinned `5611210` on 2026-11-10), listed in
`Shared/SHARED_MANIFEST.json` with upstream path, mode and SHA-256 of both the upstream and the local file:

| File | Mode |
|---|---|
| `Runtime/Python/resolver.py`, `Runtime/Python/Resolver.qml` | verbatim |
| `Runtime/QuickJs/CMakeLists.txt` (recipe of the staged `libsailtune-quickjs.so.1`) | verbatim |
| `Runtime/Child/PythonResolverChild.h`, `QuickJsChild.h` | verbatim |
| `Runtime/Child/PythonResolverChild.cpp`, `QuickJsChild.cpp` | patched: install root from `NORDTILE_RUNTIME_ROOT`, defined by the application in `NordTileRuntimeLayout.h` |
| `Runtime/Process/ResolverProcess.h` | extracted unchanged from an anonymous namespace in SailTune's `YtDlpStreamResolver.cpp` |
| `Playback/StreamSource.h` | local: SailTube-authored player-side interface (no upstream copy yet) |
| `SailTune/Core/SharedPlayback/**`, `SailTune/tests/sharedplayback/**` | verbatim from SailTune `main` `5611210` (before that `fd49627`): fast `/player` resolver with `peekCached`, loopback media proxy with the start pre-buffer, tail cache, growing chunks and pooled upstream connections, their tests |
| `SailTune/Core/Beta/ServerDate.h` | verbatim from `5611210`: included by the pinned `HttpsConnection.cpp`; an empty inline function unless `SAILTUNE_BETA` is defined, which SailTube never does |
| `SailTune/tests/privacy/check_network_inventory.py`, `test_check_network_inventory.py` | verbatim from `fd49627`: the static network-inventory checker (NFR-SEC-01) |
| `SailTune/Core/Accounts/**`, `SailTune/Core/Ytm/{Sources,Transport/Domain}/**`, `SailTune/tests/ytm_{session,sources}/` (part) | verbatim from `f875501`: session import parser, SAPISIDHASH signer, Sailfish Browser cookie import, Set-Cookie, credential vault (ACC-03/04/06/08/10, library component); built by `Shared/SharedAccounts.pri`, vault only with `CONFIG+=sailtube_secrets`; tests in `tests/shared_accounts` |

`Shared/SailTune/` mirrors SailTune's own relative layout, so the pinned files' `#include "../../Core/SharedPlayback/..."`
lines and test scripts work unchanged. SailTube glue lives beside the pins, never inside them:
`Shared/SharedPlayback.pri` (qmake), `tests/sharedplayback/run.sh` and `tests/privacy/{run.sh,inventory.py,selftest.py}`
(test entry points that call the pinned scripts). Entries may carry their own `upstream_commit`;
`tools/shared_sync.py --check --upstream <checkout> --pinned` verifies each entry against its own commit
through git objects, and `--ref <commit>` against any commit, independent of the checkout's working tree.

Rules:

- Never edit `Shared/` in SailTube. Change SailTune (or later the library) and re-pin.
- `tools/shared_sync.py --check` fails on local edits; `--check --upstream <SailTune checkout>` reports upstream
  drift; `--refresh-verbatim <checkout>` copies changed verbatim files and re-pins. Patched/extracted files are
  only reported, so a person re-applies the patch.
- Re-pinning one group to a named commit (the 2026-11-10 procedure; only committed content, never the working tree):
  `tools/shared_sync.py --refresh-verbatim <checkout> --ref <commit> --only <manifest path prefix> --restamp`.
  `--restamp` also records the commit for the group's files that did not change there, so the whole group names
  one commit. A file the new code needs and the manifest does not have yet is added through the tool as well:
  `--refresh-verbatim <checkout> --ref <commit> --add <upstream path> --note "<why>"` writes
  `Shared/SailTune/<upstream path>` and its entry; then add it to the group's `.pri`. Nothing under `Shared/SailTune/`
  is created or edited by hand.
- SailTune's history was rewritten when its beta was published: the commits `5a98381` (runtime), `fd49627`
  (privacy checker) and `f875501` (accounts) no longer exist in its repository, so `--pinned` reports those entries
  as `GONE`. Their content is unchanged: `--check --upstream <checkout> --ref 5611210` is clean for all of them
  except the privacy checker, which drifted upstream (SailTune's own link-association checks). They keep their
  recorded commits until core re-stamps them (`--restamp` on each group; patched and extracted entries and the
  top-level `upstream_commit` are edited with the patch review, as before).
- The prepared runtime artefacts (`yt-dlp.pyz`, `libsailtune-quickjs.so.1`, notices, source companion) are not
  rebuilt by SailTube: the same stage SailTune prepares with `scripts/build_python_runtime_sfos.py` is copied
  to `vendor/python-runtime/` for packaging. Shared protocol names (`SAILTUNE_*` environment variables, the
  library file name) are kept so the bytes stay identical.
- The native build loop reuses the shared tool through `tools/native_build.sh`, which rewrites a temporary copy
  for `sailtube.pro` instead of editing it.

## Extraction plan (option C), in order

1. Upstream the `NORDTILE_RUNTIME_ROOT` patch into SailTune (`#define` to `/usr/share/harbour-sailtune`), so both
   child files become verbatim. Move `ResolverProcess` into its own header in SailTune.
2. When SailTube adds the warm resolver worker (RES-01), add it to SailTune's runtime too (SailTune needs the
   same fix for its ~30–40 s cold start), so the warm protocol is designed once.
3. Create `nordtile-yt-runtime` with: `Runtime/` (children, Python bridge, warm protocol), output validator,
   process runner, runtime manifests and preparation scripts, the network-inventory privacy test, the
   InnerTube transport (visitor id, client table from yt-dlp, pooled TLS) and the session-import parser/signer
   once accounts arrive. Namespace `nordtile::runtime`. Its own host tests.
4. Both apps consume it as a git submodule at a pinned commit and drop their copies; `shared_sync.py` is retired.

Trigger: start step 1 when SailTune's streaming component is idle; coordinate with the maintainer of SailTune's shared component.

## Consequences

- SailTube builds today without touching SailTune; a SailTune security fix in a shared file shows up as
  `DRIFT` in the check and must be pulled deliberately.
- The namespace `nordtile::music::sailfish::infrastructure` of the child files appears in SailTube until step 3.
- What SailTube must not take from SailTune: YouTube-Music-specific catalog parsing, audio-only quality policy,
  the one-shot resolver wiring (`YtDlpStreamResolver`), which SailTube replaces with its own runner and
  later the warm worker.

## Requests to SailTune (relayed by the maintainer; SailTube never edits the pins)

| # | Pinned file | Request | Why (SailTube evidence) |
|---|---|---|---|
| S1 | `Runtime/Child/*.cpp` | Take the install root from a define (`NORDTILE_RUNTIME_ROOT`) | Our only patched pins; upstreaming makes them verbatim |
| S2 | `Runtime/Process/ResolverProcess.h` (extracted) | Arm `PR_SET_PDEATHSIG` (SIGKILL) in `setupChildProcess`, with a parent re-check, as `Core/Process/Infrastructure/HelperProcess` does | PLT-14: after a SIGKILL of the app the resolver child (and the QuickJS grandchild it starts) can outlive it; `tests/process` shows the pattern works on the 5.1 target |
| S3 | `Core/SharedPlayback/Infrastructure/MediaProxy.cpp` | Answer a request with a missing or unknown session token with **403**, not 404 | RES-17 accept text; `tests/ssrf` records 404 today (the request is refused and nothing is served, so it is not a leak) |
