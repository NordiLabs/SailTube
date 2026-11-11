<!-- SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0 -->
# Client definitions and endpoint parameters (NFR-MNT-04)

Goal: when YouTube changes something, the fix is an edit to one file plus a re-recorded fixture. A
static check, `tools/check_client_definitions.py`, enforces this. Its suite is
`tests/client_definitions/run.sh`, which is offline, uses only the Python 3.9 standard library and
does not need `.git`.

## What counts as a definition

The check reads every string literal in the shipped code: `Core/`, `Clients/` and `Shared/`, minus
`Shared/SailTune/tests/`. It covers C/C++, QML/JS and Python. Comments are skipped. Each literal is
classified as one of these kinds:

| Kind | Examples |
|---|---|
| `client-name` | `"WEB"`, `"VISIONOS"`, `"ANDROID_VR"`, `"clientName":"WEB"` inside a JSON literal |
| `client-version` | `"2.20261105.05.01"`; a short version such as `"1.02"` on a client or OS version assignment |
| `identity-key` | `INNERTUBE_CLIENT_VERSION` (the page key the WEB version is scraped from) |
| `client-header` | `X-YouTube-Client-Name`, `X-YouTube-Client-Version` |
| `endpoint` | `youtubei/v1/<name>`, and the endpoint name passed to `post<...>()` / `sendJson()` (`"browse"`, `"next"`, ...) |
| `query-param` | `prettyPrint=false`, `client=firefox` (suggestions) |
| `params` | opaque protobuf tokens such as `"EgZ2aWRlb3PyBgQKAjoA"` (channel tabs, search filters) |
| `browse-id` | `FEwhat_to_watch`, `FEchannels`, `VLWL` |
| `visitor-key` | `visitorData`, `X-Goog-Visitor-Id` |
| `extractor-arg` | yt-dlp `--extractor-args` values such as `youtube:skip=dash` |

The check does not track computed values, such as the search-filter protobuf that
`Core/Catalog/Domain/SearchFilter.cpp` builds or the tab params a channel answer returns. It also
skips request-body field names, cookie names (`VISITOR_INFO1_LIVE` is part of the account cookie
allow-list) and the numeric client ids (`1`, `101`), because those are integers, not strings. The
`X-YouTube-Client-Name` site carries the numeric id with it.

## Rules

1. **Each component has one definitions file.** Components are listed in
   `tests/client_definitions/components.json`. A component is a directory (`root`), and only its
   `definitions` file may hold the literals above. A literal in any other file of the component is a
   **stray** and FAILs. A literal outside every component is **unowned** and FAILs too: add the
   component, or move the literal.
2. **Every definitions value has a fixture.** Each value in a definitions file must appear in at
   least one fixture under `tests/**` or `Shared/SailTune/tests/**`. A fixture is a recorded
   request/response (`.gz` files are decompressed) or a test source that asserts the request shape.
   Short values and client names only match when they stand alone: `"WEB"` and `=WEB&` count, but
   `"1.02M views"` does not count for `1.02`. `tests/client_definitions/` itself never counts as a
   fixture.
3. **Waivers can only shrink.** `tests/client_definitions/pending.json` lists the strays and
   unfixtured values that existed when the check was introduced, each with an `owner`. An entry
   covers `sites` occurrences (default 1) of one value in one file. One more occurrence FAILs as a new
   stray. An entry that no longer matches, or covers more occurrences than remain, FAILs as stale, so
   the owner deletes the entry (or lowers `sites`) in the same change that fixes the code. Never add
   entries: fix the code or add the fixture instead. Reviewers reject a diff that grows
   `pending.json`.

Run it: `python3 -I tools/check_client_definitions.py` (add `--list` for every definitions value and
the fixture that backs it, or `--json`), or `sh tests/client_definitions/run.sh` for the check plus
the seeded self-tests.

## Where the definitions live (2026-11-08)

| Component | Definitions file | Owner | Holds |
|---|---|---|---|
| `shared-playback` (pinned) | `Shared/SailTune/Core/SharedPlayback/Domain/InnerTubeClient.h` | SailTune | `VISIONOS` 1.02 client (id 101, OS version), the WEB visitor-id body (`2.20260101.01.00`), client and visitor headers |
| `catalog` | `Core/Catalog/Infrastructure/InnerTubeClient.cpp` | catalog | `WEB`, the `youtubei/v1/` base, `prettyPrint=false`, client headers |
| `account` | `Core/Account/Infrastructure/AccountService.cpp` | account | `WEB`, fallback version `2.20241105.01.00`, `browse`, `account/account_menu`, `account/accounts_list`, client headers |
| `library` | `Core/Library/Infrastructure/AccountImport.cpp` | library | browse ids `FEchannels`, `FEplaylist_aggregation` |
| `resolver` | `Core/Resolver/Infrastructure/YtDlpVideoService.cpp` | core | `youtube:skip=dash` |
| `sailfish-client` | `Clients/Sailfish/Composition/LibraryCatalogGlue.cpp` | library | the `player` endpoint of the library glue |

Waived today (`pending.json`: 4 stray entries covering 5 sites, and 2 unfixtured values; all of them pinned or account code):

- **sailtune** (4 stray entries covering 5 sites, and 1 unfixtured value, all pinned).
  `FastStreamResolver.cpp` builds `/youtubei/v1/visitor_id` and `/youtubei/v1/player` with
  `prettyPrint=false` and reads `visitorData`. The visitor body's WEB version `2.20260101.01.00` has
  no fixture. These entries can only leave through a SailTune change and a re-pin.
- **account**: the fallback version `2.20241105.01.00` has no fixture. Note that it is a year older
  than catalog's built-in WEB version; account normally takes the version from its provider.

## Update procedure (a YouTube change)

1. **Find the value.** Run `python3 -I tools/check_client_definitions.py --list`. It prints every
   definitions value with its file, line and the fixture that backs it.
2. **Edit the one definitions file** of the component. If the value is still a waived stray, move it
   into the definitions file first and delete its `pending.json` entry.
3. **Re-record or update the fixture** so the new value appears in it:
   - Catalog discovery answers use `tools/record_fixtures.py`. This is the only catalog tool that
     touches the network. It is manual and opt-in, and it never runs from a test. It scrapes the
     current WEB version from the homepage (`INNERTUBE_CLIENT_VERSION` with `SOCS=CAI`), records
     every request into `tests/fixtures/catalog/*.json.gz`, scrubs each answer (IPs, `sn-` node
     names, the visitor's country, tracking blobs) and refuses to save an answer whose scrub check
     fails. It writes `MANIFEST.json`, which holds the request (`endpoint`, `body` with `params`) and
     the `clientVersion` used. `--only NAME[,NAME]` saves only the named fixtures, but it still sends
     the whole request plan. `--dry-run` writes nothing. `--locales es,pt,...` records the
     per-locale set. Catalog tests assert shapes, never exact values, so you can always re-record.
     Review the diff before committing.
   - Account, library, resolver and client glue: update the request-shape assertion in the
     component's test (`tests/account_service`, `tests/library_account_import`,
     `tests/library_catalog_glue`, ...) to the new value.
4. **Run** `sh tests/client_definitions/run.sh` and the component's own suites
   (`tools/catalog_test.sh <suite>` in the container, or `tools/native_build.sh --tests`).

The self-test proves the procedure on a scratch copy of the real tree. It changes one client version
in its definitions file and confirms the check FAILs (unfixtured). It then rewrites the fixtures that
carried the old value, which is the "re-record" step, and confirms the check PASSes. The SailTube-owned
definitions files have no client version that a fixture backs today: catalog's version is still a
waived stray in `ClientIdentity.cpp`, and account's fallback has no fixture. The scratch run
therefore uses the pinned VISIONOS `1.02`, backed by `Shared/SailTune/tests/sharedplayback/*.cpp`.
Once catalog moves its version into its definitions file, the self-test picks that version instead.

## The pinned SailTune caveat

`Shared/SailTune/**` is a verbatim pin of SailTune (`Shared/SHARED_MANIFEST.json`, `docs/SHARING.md`).
`tools/shared_sync.py --check` fails on any local edit. The shared playback component's client
definitions in `Shared/SailTune/Core/SharedPlayback/Domain/InnerTubeClient.h` therefore change only
like this:

1. SailTune edits its `InnerTubeClient.h` and its fixtures/tests (`tests/sharedplayback/*`, generated
   by its `make_fixtures.py`). The request goes through the maintainer: SailTube never edits SailTune.
2. SailTube re-pins with `tools/shared_sync.py --refresh-verbatim <SailTune checkout>`. The pinned
   tests come along, so rule 2 still holds after the re-pin. If the re-pin removes a waived stray,
   delete its `pending.json` entry in the same change.

Relation to NFR-MNT-01 (extractor updates without an app release): the header calls its VISIONOS
definition a cold-start default, and a host should refresh it from the bundled yt-dlp client table.
That way a client rotation arrives with a `.pyz` update instead of a re-pin and a release. Catalog
has the same hook, `ClientIdentity::setExtractorVersion`, which takes the WEB version from the
extractor table. Neither hook is wired in SailTube today. In the meantime:

- catalog scrapes the current version at run time (cached for 12 h) and only falls back to the
  built-in value;
- the fast path takes the pinned VISIONOS default as it is;
- the extractor (yt-dlp) brings its own client table with each `.pyz` update.

So a YouTube client rotation is covered at run time by the scrape and by the yt-dlp update. Only the
fast path waits for a re-pin. The weekly live check (`docs/LIVE_CHECK.md`) is what notices that a
definition went stale.
