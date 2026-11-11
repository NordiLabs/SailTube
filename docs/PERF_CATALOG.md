# Catalog component performance (NFR-PERF-08/09, E12)

What the catalog component does so that home, the subscriptions feed and search feel instant and
never slow down playback, how each stage is logged, and the latest numbers. The device is not
connected at the time of writing (2026-11-08), so the numbers below come from the build
container against live YouTube (`tools/perf/catalog_live_perf.sh`), plus the one device reading
from R2. They are a baseline for comparison, not device results.

## Rules

* **Content first, then refresh in place.**
  * **Feed:** paints its disk cache before any request (DSC-06). It refreshes the due channels
    in the background and replaces rows only when the id sequence changes.
  * **Search:** paints the last page 1 from disk at start (query, filters and rows;
    `last_search.json`, 0600, at most 7 days old). When the search page opens, it fetches page 1
    again and swaps the rows in one reset, never through an empty list.
  * **Home:** follows the landing-page setting (DSC-04). Recommended is a valid-empty answer
    without an account, so Subscriptions (the feed) is the default.
* **Never compete with the first video bytes.**
  * Discovery has its own transports and lanes (RES-02). The playback component's are never used.
  * On the watch page, the watch-next answer waits until `playback.playing` turns true for the
    new video, or 2.5 s at most: info, related videos and the comments token
    (`WatchInfo.waitForPlayback`). Comments load from that token, so they wait too.
  * A tap that opens a video drops the search's next-page prefetch.
* **Prefetch only what the next gesture needs.**
  * The search list fetches the next page into its cache 250 ms after a page is shown. A scroll
    then shows it at once, or attaches to the request still in flight.
  * A prefetch never starts the yt-dlp tier, fails silently, and is dropped by a new search.
  * Thumbnails load only for delegates that exist: the rows on screen plus the list view's
    cache buffer. A prefetched page loads no images until it is scrolled into view.
* **Skip the TCP + TLS handshake.**
  * `HttpTransport::warm(host)` opens the connection ahead of time, at most once a minute per
    host.
  * When: 1.5 s after start (after playback's own first requests), whenever the app returns to
    the foreground, and on the first character typed in search.
* **Likely next tap (E12):** `tapIntent` gives playback the rows on screen (`likelyVisible`) and
  the finger-down on a row (`touchDown`, usually 100–300 ms before the tap), so it can resolve
  speculatively. Playback decides; the catalog makes no request for it.

## Timing lines (`sailtube-timing …`, journal / stderr)

| Line | Where | Meaning |
|---|---|---|
| `innertube <endpoint> net=<ms> parse=<ms> ready=<ms> bytes=<n> status=<s>` | `InnerTubeClient::post`, every InnerTube request (search, browse, next, player, resolve_url) | `net` runs from request to last byte. `parse` is the JSON decode plus the renderer parse on the lane. `ready` runs from the last byte to delivery on the owner's thread (parse plus lane queueing). |
| `search page1 <ms>` | `DiscoverySearchModel` | From the search call to page 1's rows in the model (NFR-PERF-09). |
| `suggest <ms>` | `SuggestionModel` | Network answer shown (not logged for cache hits). |
| `comments <ms> threads=<n> pages=<n>` | `CommentsModel` | First comments page shown. |
| `watchnext released <id> by playing\|grace\|off` | `WatchInfo` | The watch-next requests start, and why. |
| `prewarm <transport> <host>` | `NetworkTransport::warm` | A connection was opened ahead of time. |

A device run should collect these with `journalctl --user -f | grep sailtube-timing`; that is
device step T1.

## Measurements

### Device (R2, POCO X3 Pro, 2026-11-08; before this batch)

| Step | Result |
|---|---|
| Suggestions | 332 ms |
| Search page 1 | 714 ms (target < 700 ms) |

### Container against live YouTube (2026-11-08, `--runs 5`, milliseconds)

| Scenario | Min | Median | Max | Bytes | Notes |
|---|---|---|---|---|---|
| search, cold connection | 624 | 852 | 1060 | – | new transport per run |
| search, warm connection | 516 | 597 | 707 | 479 238 | network alone: median 551 |
| search page 2 | 543 | 655 | 684 | 366 084 | network alone: median 618 |
| suggest | 52 | 73 | 120 | 200 | |
| channel Videos tab | 188 | 248 | 304 | 455 153 | |
| watch-next (`next` + `player` in parallel) | 554 | 781 | 1262 | 724 023 | JSON 3, parse 16 (medians) |
| comments page 1 | 304 | 348 | 461 | 289 773 | |
| RSS (one channel) | 15 | 29 | 42 | 27 340 | |
| playlist page 1 | 110 | 116 | 133 | 77 587 | |

With `--prewarm`, the medians were 674 ms cold against 603 ms warm.

**What the numbers mean:**
* **Parsing is not the bottleneck.** JSON decode plus renderer parse takes 0–16 ms in the
  container for answers of 0.3–0.7 MB. On the phone, expect a few times that, still well under
  100 ms.
* **Search page 1 time is mostly the server's reply.** That is about 550 ms of network from the
  container, of which the handshake is about 70–250 ms. On the phone's link, the handshake costs
  more, so warming it is what brings 714 ms under 700 ms. The cached first paint makes the page
  show content at 0 ms either way.
* **Page 2 adds about the same as page 1, so prefetching it hides about 0.6 s per scroll.**
* **Watch-next, at about 0.8 s and 0.7 MB, is the largest discovery answer.** It now waits for
  the first frame, so it never shares the link with the first video bytes.

To reproduce (opt-in, live network, never part of the gate):

```bash
tools/perf/catalog_live_perf.sh --runs 5
```

```bash
tools/perf/catalog_live_perf.sh --runs 5 --prewarm
```

Hosts contacted: www.youtube.com and suggestqueries.google.com.

## Offline checks (part of the gate)

| Check | Test |
|---|---|
| Stage line per request (`innertube search net= parse= ready= status=200`) | `catalog_search:hostTiming` |
| Search parse + fill of page 1 under 500 ms on the build host | `catalog_search:hostTiming` |
| Next page fetched once, shown without a request; a press in flight waits; no fallback; dropped by a new search | `catalog_search:nextPagePrefetch` |
| Last search shown at start (< 100 ms, no request), swapped in one reset, a failure keeps the rows, history pause and age respected | `catalog_search:lastSearchSnapshot` |
| Watch-next waits for the player, the grace release, cached answers and an already-playing video load at once | `catalog_watch:holdUntilPlaying` |
| `tapIntent` settle, cap, change-only, interval, press, cancel, open, background | `catalog_search:tapIntentSignals` |
| Feed cache painted before the network | `catalog_feed:cacheLaunch` |

## Not done yet

* **Next-page prefetch on the channel page:** its continuation is fetched on scroll only.
* **Device numbers:** steps T1, F1 and F5 need the phone; ask the maintainer.
