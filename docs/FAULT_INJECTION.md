# Fault injection and download safety

This page covers two suites:

- `tests/fault_injection` for **NFR-ROB-01** (degrade, never break). Every discovery and resolve path has a fallback, and a failure in one item affects only that item.
- `tests/download_safety` for **NFR-SEC-05**. File names follow DL-05, and only the documented hosts are fetched. The suite also checks paths, sizes and types.

Both suites run the real code, the same as the app does. The faults come in only through the seams that already exist: transports, resolver hooks and extractor executables. Nothing under `Core/`, `Shared/` or `Clients/` was changed for these suites.

## Running

```sh
sh tests/fault_injection/run.sh   # Qt5Core + Qt5Network (moc); last line "fault_injection: PASS"
sh tests/download_safety/run.sh   # + Qt5Sql and python3; last line "download_safety: PASS"
```

Run these in the Linux build container, from a snapshot without `.git`. Each suite builds in a `mktemp -d` directory, and a trap removes it on exit. Where the Qt development files are missing, the suite is skipped.

Each case prints one line:

```
CASE <name>: PASS | FAIL | KNOWN-BUG (<file:line>) | GAP (<what>) | N/A
```

- **FAIL** fails the suite.
- **KNOWN-BUG** marks a real defect that this document records. It does not fail the suite. Once the defect is fixed, the case prints **XPASS**, and this page should be updated.
- **GAP** marks a protection that the code does not claim to have. A GAP is reported but never fails the suite.

The fixtures are synthetic and live in `tests/fault_injection/fixtures/`. They hold made-up ids such as `fiSearch001`, `UCfaultChannel0000000001` and `fiResolve01`, plus test hosts such as `rr1---sn-fault.googlevideo.com` and `*.fault.invalid`. They contain no recorded data.

## Seams used

| Seam | Where | Used for |
|---|---|---|
| `catalog::HttpTransport` | `Core/Catalog/Infrastructure/HttpTransport.h` | `fault::FaultTransport` (`tests/fault_injection/FaultSupport.h`) answers any status, a malformed body, a network error (status 0) or a timeout (`timedOut`) for each request. `fault::LoopbackTransport` sends the real `NetworkTransport` to the misbehaving local servers in `fault::BadServer`. |
| `SearchFallback` | `SearchService.h` | Scripted second search tier (the yt-dlp flat search in the app). |
| `FeedExtractorHook` | `FeedService.h` | Scripted third feed tier. |
| `SharedStreamSource<Api>` with `Api::Resolver` | `Core/Player/Infrastructure/SharedStreamSource.h` | A fake fast path of the component's shape. The proxy and the domain types are SailTune's pinned ones. |
| `YtDlpRunner` `executablePath` (`pythonChild = false`) | `Core/Resolver/Infrastructure/YtDlpRunner.h` | A fake `yt-dlp` shell script, written by the test, that serves fixtures, errors, crashes and hangs. The real `YtDlpStreamResolver` → `YtDlpVideoService` → `YtDlpRunner` chain runs on top of it. |
| `parsePlayerResponse` | `Shared/SailTune/.../PlayerResponseParser.h` (read-only) | The fast path's parser, fed a synthetic player answer. |
| `DownloadResolver`, `EngineOptions` | `Core/Downloads/Infrastructure/` | A scripted resolver, the `locate` callback and the host policies (the defaults, or 127.0.0.1 for the local host). |
| `CHECK`, `waitFor` | `tests/catalog_common/TestSupport.h` (read-only) | Reused through `FaultSupport.h`. |

## NFR-ROB-01 cases (`tests/fault_injection`)

The first run (2026-11-08, `sfos-m1-sailtube`) gave 45 PASS, 1 KNOWN-BUG, 0 FAIL; the one known bug (B1, below) has since been fixed and its case asserts the fixed behaviour.

### Discovery (`CatalogFaultTest.cpp`)

| Case | Fault injected | Asserted | Status |
|---|---|---|---|
| `search_one_bad_item_keeps_healthy_rows` | A row without an id, a renderer that is not an object, an unknown renderer, a channel row without an id | The 3 healthy rows survive in order. The unknown renderer is counted. A field that does not parse keeps its raw text. No drift is counted and the fallback is not used. | PASS |
| `search_malformed_json_falls_back` | Truncated JSON | The first tier reports `ShapeDrift`. The fallback rows are returned. | PASS |
| `search_missing_renderer_falls_back` | Valid JSON without `sectionListRenderer` | `ShapeDrift`, then the fallback answers. | PASS |
| `search_http429_falls_back` / `_http5xx_` / `_network_error_` / `_timeout_` | 429, 502, status 0, timeout | The first tier reports `RateLimited`, `Http5xx`, `Network` and `Network`. The fallback answers. Drift stays at 0. | PASS |
| `search_without_fallback_classified` | 503, 429, 403, network, timeout, malformed (no second tier) | The fixed class and HTTP status are reported, with no rows and nothing left pending. | PASS |
| `search_both_tiers_fail_fixed_class` | 500, then the fallback reports `Network` | The first tier's fixed class (`Http5xx`) is reported, with no rows. | PASS |
| `transport_timeout_real` | Real `NetworkTransport` to a server that never answers | `timedOut`, status 0 and `Network` after about 1 s. Search falls back. | PASS |
| `transport_connection_refused_real` | Closed port | Status 0, with error text set. `Network`, then the fallback. | PASS |
| `transport_garbage_response_real` | Bytes that are not HTTP | Status 0, `Network`, then the fallback. | PASS |
| `transport_connection_reset_real` | Connection closed without a byte | Status 0, `Network`, then the fallback. | PASS |
| `transport_truncated_body_degrades` | `200 OK`, Content-Length 4096, 20 bytes, then close | The fallback still answers. | PASS |
| `transport_truncated_body_classified_network` | The same fault | The first tier reports `Network` (the cut-off body is retried once), and the drift counter stays at 0. B1 fixed. | PASS |
| `channel_tab_one_bad_item_keeps_healthy_rows` | Videos tab with a row without an id, a row that is not an object and an unknown renderer | 2 healthy rows. The bad items are counted as unparsed or unknown. The continuation is kept. | PASS |
| `channel_tab_faults_classified` | Malformed, no tab list, ERROR alert, 404, 429, 503, network, timeout | `ShapeDrift`, `ShapeDrift`, `Unavailable`, `Unavailable`, `RateLimited`, `Http5xx`, `Network`, `Network`. Only the shape faults count as drift. | PASS |
| `channel_one_dead_among_several` | 3 channels browsed in parallel, the middle one answers 503 | A and C are `Ok` with their rows. B is `Http5xx`. | PASS |
| `feed_one_dead_channel_among_several` | 6 channels, see the feed matrix below | Each healthy channel keeps its rows from the expected tier. Only C fails, and the round is not "all failed". In the next round A fails on every tier and keeps its last good rows. | PASS |
| `comments_one_broken_thread_among_several` | 5 threads: one view model without an entity, one entity without an id, an unknown item, an older inline renderer, a junk mutation | Threads A, C and E survive in order. `missingBodies` is 2, and the total and continuation are kept. | PASS |
| `comments_page_faults_classified` | Malformed, no container, 429, 503, network, timeout | Fixed classes. Only the shape faults count as drift. | PASS |
| `comments_replies_failure_isolated_to_thread` | `CommentsModel`: thread A's replies answer 503, thread C's replies answer 200 | A shows an inline `replyErrorText`. C gets its reply and E is untouched. The list has no error, and A's failed request is not retried automatically. | PASS |
| `comments_entry_rate_limited_inline` | Watch-next entry answers 429 | An inline list error, no rows, `RateLimited` counted, no drift. | PASS |
| `related_one_bad_row_keeps_healthy_rows` | Related list with a row without an id, a row that is not an object, an unknown view model and the seed video | 2 healthy rows. The seed is excluded and the unknown renderer is counted. Info and continuation are kept. | PASS |
| `related_page_failure_keeps_rows` | Page 2 answers 503, then malformed, then times out, then works | The rows of page 1 stay. The page outcome carries the class, and the token is kept for a retry. The retry appends the healthy row and drops the bad one. | PASS |
| `watch_player_failure_info_survives` | WEB `/player` answers 500 | The info and related rows from `/next` stay, and the player outcome is `Http5xx`. | PASS |
| `watch_next_failure_not_cached` | `/next` answers 503, malformed or a network error | The failed page is not cached. The next request works. | PASS |

Feed matrix for `feed_one_dead_channel_among_several`. RSS is tier 1, the Videos tab tier 2 and the extractor tier 3.

| Channel | RSS | Videos tab | Extractor | Expected result |
|---|---|---|---|---|
| A | ok, 3 entries | (503 in round 2) | (Network in round 2) | 3 rows from RSS, kept in round 2 |
| B | malformed XML | ok, 2 healthy and 2 bad rows | — | 2 rows from InnerTube |
| C | 404 HTML | 503 | Network | 0 rows, `channelFailed` |
| D | network error | 429 | ok, 1 row | 1 row from the extractor |
| E | ok, with 1 bad and 1 empty entry | — | — | 2 rows from RSS |
| F | timeout | ok | — | 2 rows from InnerTube |

### Resolve (`ResolveFaultTest.cpp`)

| Case | Fault injected | Asserted | Status |
|---|---|---|---|
| `fast_network_falls_back_to_extractor` / `_bot_wall_` / `_bad_response_` / `_breaker_open_` | The fast path fails with `fallback = true` | The extractor's healthy set is returned: video `137`, audio `140` and `251`, every URL on https `*.googlevideo.com`. Counters: `fallbacks` 1, `extractorAnswers` 1, one extractor run. | PASS |
| `fast_unpaired_answer_falls_back_to_extractor` | Audio-only fast answer for a video request | `fastUnpaired` is 1 and the extractor answers. | PASS |
| `fast_final_never_falls_back` | `Final` whose label names the case (`removed`) | `Removed`, final, no extractor run, even with a healthy extractor. An unnamed `Final` is named by exactly one token-free extractor run and stays final whatever that run says (five cases: removed, country, bare answer, network failure, upcoming with its start). | PASS |
| `fast_one_bad_format_keeps_set` | A fast answer that also holds an audio and a video rung of unknown codecs and an HLS rung among the video ones | The fast path answers with video `137,136` and audio `251` and `140`. The bad rungs are dropped and the extractor does not run. | PASS |
| `fast_player_answer_one_bad_format_keeps_set` | A player answer with 9 bad formats (cipher, DRM, http, foreign host, port 8443, `text/plain`, OTF, no URL, not an object) | 2 formats kept (`251`, `137`) and 9 dropped, with no failure. | PASS |
| `fast_player_answer_faults_classified` | Not JSON, no playability, bot wall, removed, age gate, another video, live, no formats | `BadResponse`, `BadResponse`, `BotWall`, `Final`, `Unplayable`, `BadResponse`, `Unplayable`, `NoUsableFormat`. The adapter's class is fixed, and only `Final` is final. | PASS |
| `extractor_bot_check_fixed_class` | The fast path fails, then yt-dlp's stderr reports a bot check | `BotCheck` with label `bot_check`. The provider text never reaches the label. 2 runs (fast profile, then the full profile). | PASS |
| `extractor_malformed_json_invalid_response` | Truncated JSON on stdout | `InvalidResponse`, 2 runs. | PASS |
| `extractor_unavailable_final_single_run` | "Video unavailable … removed" | `Removed`, final, 1 run (no second pass). A bare "Video unavailable" is not final: the QuickJS pass runs and can answer. Country, terminated and upcoming are final in one run; HTTP 429 is try-later in one run; the temp-file limit and formats without URLs are retryable extraction failures. | PASS |
| `extractor_network_error_classified` | `urlopen error` | `Network`, 2 runs. | PASS |
| `extractor_other_video_rejected` | The answer names another video id | `InvalidResponse`. Another video's streams are never returned. | PASS |
| `extractor_crash_classified` | The process kills itself with SIGSEGV | `Failed`. | PASS |
| `extractor_metadata_without_streams_named` | Metadata with no formats, plus a bot warning on stderr | `BotCheck`. | PASS |
| `extractor_runtime_missing_classified` | The executable is missing | `RuntimeUnavailable`, no run. | PASS |
| `extractor_timeout_classified` | A runner run with a 500 ms timeout against `exec sleep 5` | `timedOut` within 4 s. The `Timeout` class is not final. | PASS |
| `extractor_one_bad_format_keeps_set` | yt-dlp formats: no URL, http, foreign host, userinfo, `"not an object"`, `42`, an unknown codec with string sizes, a storyboard | The parser keeps `140,weirdcodec,137,251`. After the ladder the set is video `137` and audio `140` and `251`. | PASS |

## NFR-SEC-05 cases (`tests/download_safety`)

The status is from the run on 2026-11-08: 16 PASS, 4 GAP, 1 N/A, 0 FAIL.

| Case | What is checked | Status |
|---|---|---|
| `dl05_name_tests` | The library component's DL-05 name tests (`tests/downloads_domain` `naming()`), run as they are: traversal, separators, NUL and control characters, bidi overrides, FAT-reserved characters, leading dot or dash, reserved device names, the 255-byte limit with the longest suffix, UTF-8 cut points, template text kept literal, the id and extension allowlists | PASS |
| `dl05_hostile_titles_on_disk` | 11 hostile titles (`../../outside/escape`, separators, control characters, `.hidden`, `-rf`, `CON`, 300 bytes of CJK, template text, an RTL override, FAT-reserved characters, `..`) downloaded through the engine. Each becomes the expected DL-05 name, flat in the chosen folder, with the right bytes and no `.part` left over. The sibling folder stays empty. Hostile ids are refused at enqueue. | PASS |
| `confinement_symlinked_final_replaced` | A symlink planted at the final name, pointing outside the folder. It is replaced by a regular file and its target is untouched. | PASS |
| `confinement_symlinked_part_probe` → `confinement_symlinked_part_refused` | A symlink planted at `<name>.part`, pointing outside. The engine writes 30000 bytes through it, then renames the link into place. | PASS (G1 closed by library: the planted link is replaced) |
| `size_content_length_mismatch_refused` | A `200` whose Content-Length is 5 bytes more than announced | Failed with `size_mismatch`, no file. PASS |
| `size_content_range_total_mismatch_refused` | A `206` whose Content-Range total differs from the announced size | Failed with `size_mismatch`, no file. PASS |
| `size_unknown_length_refused` | No announced size and no Content-Length | Failed with `http`, no file. PASS |
| `size_body_longer_than_range_clipped` | Each `206` carries 3000 bytes more than its range | Done, and the file is exactly the announced bytes. PASS |
| `size_free_space_precheck` | A static probe of `Core/Downloads` for a free-space check | PRESENT (G4: a free-space check exists; behavioural cases in tests/offline_engine `case503AndLowSpace`) |
| `type_non_media_body_probe` → `type_non_media_body_refused` | `text/html` served at exactly the announced size. It is saved as `.m4a`. | PASS (G2 closed by library) |
| `type_thumbnail_jpeg_and_size_checked` | A JPEG is saved. HTML, PNG and a JPEG over the cap are not saved. | PASS |
| `hosts_default_policy_tables` | `EngineOptions::defaultMediaHost` and `defaultThumbnailHost` on 17 URLs: https `*.googlevideo.com` media and `https://i.ytimg.com` thumbnails only. Refused: http, the bare domain, look-alike suffixes and prefixes, a userinfo trick naming another host, other YouTube hosts, ftp, file, IP addresses, empty. | PASS |
| `hosts_default_policy_looseness_probe` → `hosts_media_check_port_userinfo_path` | Userinfo, port 8443 and any path on `*.googlevideo.com` are accepted | PASS (G3 closed by library) |
| `hosts_default_policy_refuses_before_connecting` | With the engine's own policy, plain http and a foreign https host are refused (`host_refused`) with zero connections. Only empty `.part` files remain. | PASS |
| `hosts_redirects_rechecked_and_capped` | A 302 to a foreign host is refused and not followed. A loop on an allowed host stops after 3 hops (4 requests). | PASS |
| `hosts_no_cookies_or_credentials_sent` | The resolver's Cookie and Authorization headers are never sent, and every media request is a Range request. | PASS |
| `nfr_sec_01_inventory` | `tests/privacy/run.sh`, run as it is (strict FULL, STORE, unlisted-host rejection) | PASS |
| `res14_bind_check` | RES-14 (FULL) PO-token provider bind check | **N/A**: no PO-token provider exists yet (RES-14 is "missing" in `docs/CHECKLIST.md`). `StreamRequest::poProvider` is only a cache-key field. |

## Bugs and gaps found

These are not fixed here. Each one belongs to the component that owns the code.

- **B1 (catalog component, bug, fixed): a truncated body was classified as drift.** Fixed in the catalog
  component: `NetworkTransport::finish` marks a 2xx reply cut short (`reply->error()` set, or fewer bytes
  than an uncompressed `Content-Length`) as `truncated` with status 0, and `InnerTubeClient` sends it
  once more before it reports `Network`. Tests: `catalog_core:truncatedBody` and the case below,
  which now asserts the classification. The original finding:
  - Where: `Core/Catalog/Infrastructure/HttpTransport.cpp:113-117` (`NetworkTransport::finish`) and `Core/Catalog/Infrastructure/InnerTubeClient.cpp:45-50` (`toReply`).
  - What happens: when the connection drops after a `200` status line, `finish` keeps status 200 and the partial body, because `errorText` is set only when the status is 0. `toReply` then fails to parse the JSON and reports `ShapeDrift`, which `Outcome.h` reserves for "the parser needs an update". So a flaky network inflates the DIA-01 drift counter and can raise a false "parser outdated" hint.
  - Effect on users: the fallback still runs, so they are not affected.
  - Suggested fix: treat `reply->error() != NoError` as a transport failure whatever the status, or classify it as `Network` in `toReply` when `errorText` is set.
  - Case: `transport_truncated_body_classified_network`.
- **G1 (library component, closed): a planted `.part` symlink was written through; the engine now replaces it.**
  - Where: `Core/Downloads/Infrastructure/DownloadEngine.cpp:607-608` (`beginTrack` opens `<name>.part` with `WriteOnly | Append` and follows links) and `:829` (`finishTrack` renames the link into place).
  - What happens: a symlink planted in the download folder makes the engine write outside the folder, and the final name then becomes a link to that outside file.
  - Who can do it: anything that can write to the folder, such as another app with Videos or Downloads access. A FAT SD card cannot hold symlinks.
  - Suggested fix: refuse or replace a symlink (`QFileInfo::isSymLink`, or `O_NOFOLLOW`) before opening the part and before the rename.
- **G2 (library component, closed): media bodies were not type-checked.**
  - Where: `DownloadEngine.cpp:690` (`checkResponse`).
  - What happens: an HTML or JSON error body of exactly the announced size is saved as media. The size checks make this unlikely, because a 403 or 404 page rarely matches the clen.
  - Suggested fix: refuse `text/*` and `application/json` Content-Types, or sniff the first bytes (`ftyp` or EBML) at offset 0. The thumbnail path already checks the JPEG magic and a size cap.
- **G3 (library component, closed): the default media host check was looser than the resolvers'.**
  - Where: `DownloadEngine.cpp:132-134` (`EngineOptions::defaultMediaHost`).
  - What happens: the check tests only the scheme and the `.googlevideo.com` suffix. Core's `acceptedMediaUrl` (`YtDlpJson.cpp`) and the component's `mediaHost` (`PlayerResponseParser.cpp`) also refuse a userinfo part, a port other than 443 and a path other than `/videoplayback`.
  - Why it matters: resolved URLs are already filtered, but a redirect `Location` is checked by this function alone. The host stays on googlevideo, so this is defence in depth.
  - Suggested fix: reuse `acceptedMediaUrl`.
- **G4 (library component, present): a free-space check before a transfer now exists (`no_space`); disk-full mid-transfer is exercised with an LD_PRELOAD ENOSPC shim in tests/offline_engine.**
  - What happens: nothing in `Core/Downloads` checks free space (no `QStorageInfo`). A full disk ends as a `write` failure in `readBody`, with the part kept, which is safe but not a precheck.
- **Observation (catalog component, not tested): redirect hosts are missing from the host inventory.** `NetworkTransport` follows redirects (`FollowRedirectsAttribute`), and `contactedHosts()` records only the first host of each request. Qt refuses https→http downgrades.

## Not covered

- Discovery paths outside the NFR-ROB-01 list are not covered. These are suggestions, home, playlists, captions, SponsorBlock and dislikes, and their own suites cover their failure classes.
- The proxy, refresher and probe paths of `SharedStreamSource` are not covered here. They are in `tests/shared_stream_source`.
- The extractor's real 60 s and 90 s profile timeouts are not exercised. The timeout is checked on the runner with a short limit, together with the class mapping.
