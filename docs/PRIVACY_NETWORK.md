# Network and privacy inventory (NFR-SEC-01)

Every host SailTube may contact, what triggers it and what it sends. The machine-readable list is
`tests/privacy/network_allowlist.json`; `tests/privacy/run.sh` (SailTune's checker, pinned under
`Shared/SailTune/tests/privacy/`) fails the build when code names a host or uses a network API that
the list does not name for that file, when a package permission, Qt module, rpm requirement or QML
import leaves the reviewed baseline, or when a tracking SDK or web engine appears. It also checks
that this page mentions every listed host. This is a static aid, not a traffic capture: addresses
returned at run time (stream and thumbnail addresses) and the bundled yt-dlp child are listed under
"dynamic sources" below.

There is no telemetry, no analytics, no advertising and no crash upload, and none may be added.

## Hosts in use

| Host | Why | Trigger | Sends |
|---|---|---|---|
| `www.youtube.com` | Stream lookup: yt-dlp watch page (child process); shared fast path visitor id and player request. Catalog component: anonymous InnerTube WEB discovery (`youtubei/v1` search, browse, next, `navigation/resolve_url`, player, and for Shorts `reel/reel_item_watch` / `reel/reel_watch_sequence`), the caption track list of a playing video (`player` with the shared fast path's client description and a visitor id, `visitor_id` when no answer has named one), the homepage client-version scrape (cookie `SOCS=CAI` only), channel RSS feeds (`feeds/videos.xml`) and caption tracks (`api/timedtext`); a connection opened ahead of a search (TCP + TLS only, no request) | user opens or plays a video; searches; opens a list, channel, playlist or watch page; the feed refreshes subscribed channels; captions are shown; imported handles are resolved | video id, query, channel/playlist id, continuation tokens, the request language and region, fixed public client description, provider visitor id kept in memory only; no cookies (besides the consent value), key, account or device id |
| `youtube.com` | host suffix accepted for live HLS manifests (RES-22); recognised in imported or pasted links (library); no request of its own | live playback, import | — |
| `googlevideo.com` / `.googlevideo.com` | media bytes of the chosen streams (probe, Range chunks, DASH fragments) and of downloads (Range resume); suffix check of resolved addresses | playback, user-started download | signed media address, fixed client headers |
| `127.0.0.1` | shared loopback media proxy with a session token; never leaves the device. [FULL] the optional PO-token helper's verified loopback base URL (RES-14), used by the yt-dlp child | playback through the shared component; [FULL] an extractor resolve while the user-enabled helper is ready and the request asks for it | nothing external; to the helper: the extractor's token request, no cookies |
| `i.ytimg.com`, `.ytimg.com` | thumbnails (addresses come from resolver/catalog answers; `i9.ytimg.com` and similar), saved with a download; storyboard sheets for the seek bar's scrub preview (only https addresses on this suffix under `/sb/`, from the resolver's answer); one shared bounded disk cache | lists, the video page, a download; dragging the seek bar on a video whose answer carries a storyboard | the thumbnail or storyboard sheet address |
| `yt3.ggpht.com`, `.googleusercontent.com` | channel avatars, banners and playlist artwork (DSC-12, DSC-19) | a channel, playlist or video page is shown | the image address |
| `suggestqueries.google.com` | search suggestions (DSC-11) | the user types in search | the typed prefix and the request language |
| `sponsor.ajay.app` | SponsorBlock segments by hash prefix (SB-01) | a video is played with SponsorBlock on | the first 4 hex characters of the SHA-256 of the video id; never the id |
| `returnyoutubedislikeapi.com` | Return YouTube Dislike counts (RYD-01) | a video page is shown with the option on (off by default) | the video id |
| `github.com` | [FULL] extractor (yt-dlp) updates, NFR-MNT-01 / NFR-SEC-04 (`Core/Extractor`, see `docs/EXTRACTOR_UPDATE.md`): HEAD of the channel's `releases/latest/download/yt-dlp` (the redirect's tag is the version), GET `releases/download/<tag>/SHA2-256SUMS` and `.sig`, and after the user's confirmation `releases/download/<tag>/yt-dlp`; channels `yt-dlp/yt-dlp` (stable) and `yt-dlp/yt-dlp-nightly-builds` (nightly) | the user checks for an extractor update, or confirms the offered one | fixed User-Agent; no cookies, account or device id |
| `release-assets.githubusercontent.com`, `objects.githubusercontent.com` | [FULL] GitHub's release asset hosts: the only hosts a github.com release download may redirect to (every hop re-checked: https, port 443, no user info, no further redirect) | as for `github.com` | the signed asset address github.com redirected to |

Link hosts that are parsed, never fetched, are listed under `ignored_hosts` with that reason, not as hosts:
`youtube-nocookie.com` (library import) and, in the catalog component's link and description parsing (LNK-01/02),
`m.youtube.com`, `music.youtube.com` (recognised and reported as unsupported), `youtu.be` and
`www.youtube-nocookie.com`. The XML namespace names `www.w3.org` (Atom) and `search.yahoo.com` (Media RSS)
are compared while parsing channel feeds and are never fetched either.

## Links built for sharing

`Clients/Sailfish/qml/logic/ShareLinks.js` writes the public address of a channel
(`https://www.youtube.com/channel/<id>` or `/@handle`) or of a playlist (`/playlist?list=<id>`) from
an id the app already holds, for the Share and Copy link actions of the channel and playlist pages.
The address goes to the system share sheet or the clipboard, only when the user taps the action; the
app makes no request with it. The allowlist lists the file under `www.youtube.com` as a reference file.

## Hosts approved by the requirements but not used yet

None at present. A component that starts using an approved host lists its file in the allowlist entry in
the same commit and adds it to the table above; any other host needs a requirements change first.
`github.com` and `objects.githubusercontent.com` (NFR-MNT-01) moved to the table above with the
extractor updater; `release-assets.githubusercontent.com` was added with it, because GitHub now
redirects release downloads there. All three are FULL only: `tests/privacy/run.sh` also runs the STORE
variant (`inventory.py --variant store`), which rejects any STORE code naming them.

## Dynamic sources

- **Resolved media and thumbnail addresses.** Stream addresses are accepted only over HTTPS on port
  443, under `googlevideo.com` with path `/videoplayback` (HLS also under `youtube.com`), without
  user info (`acceptedMediaUrl`, tested in `tests/resolver_qt`); the shared parser keeps plain
  addresses on `*.googlevideo.com` only, and the probe and proxy follow at most three redirects, only to
  https `*.googlevideo.com` (`MediaAddress.h`; tested in `tests/sharedplayback`).
- **[FULL] PO-token helper (RES-14).** Off by default; the user enables it, aarch64 only. It is a
  separate Deno process running the pinned public bgutil HTTP provider, managed by
  `Core/Resolver/Infrastructure/PoTokenProvider.cpp` (command line in
  `Core/Resolver/Domain/PoTokenPolicy.cpp`). Its own network use is the provider's and is not
  restricted (`--allow-net`; a host allow-list broke token minting): it contacts the provider's
  attestation endpoints while minting, and its frozen install (`deno install --frozen`) fetches the
  locked npm/JSR packages. It may read only its own directory (`<AppData>/potoken`, not the account
  file) and may not write, run programs or load native code; its environment is a fixed minimal one
  (no app variables, no proxy, no PATH). SailTube's side talks to it on `127.0.0.1` only: after each
  start it reads `/proc/net/tcp` and `tcp6` (and the helper's own `/proc/<pid>/fd`) and stops and
  refuses the helper if any of its listening sockets is outside `127.0.0.1`; the check repeats while
  it runs. The yt-dlp child gets the helper's base URL and plugin directory only while it is ready and
  only for requests that ask for it, never with cookies. STORE has no helper at all.
- **yt-dlp / QuickJS resolver child.** The bundled resolver contacts the provider on its own while
  a user-started search or resolution runs. It gets no cookies or account data.

## Optional signed-in session (ACC-01..11)

Signed out is the default and everything works without a session. Only after the user imports one
(from the Sailfish Browser on FULL, or by pasting cookies), the credential-carrying paths
`Core/Account/Infrastructure/AccountService.cpp` and `Core/Account/Infrastructure/AuthTransport.cpp`
send InnerTube WEB POSTs to `www.youtube.com` (`account/account_menu` to verify the session at start,
after an import or switch, on demand and every 15 min; `account/accounts_list` to list logins and
brand channels; `browse` for the subscription and playlist import). Each carries the imported
cookies, a fresh `SAPISIDHASH` authorization, `Origin`, `Referer`, `X-Goog-AuthUser` and, for a brand
channel, `X-Goog-PageId`. The transport keeps no cookie jar, follows no redirect and accepts no other
host. Rotated cookies from the answers are written back to the session store. The session is kept in
the Sailfish Secrets vault when available; on FULL otherwise in a 0600 file in the private data
folder (disclosed once); on STORE it is not stored at all without the vault. Sign-out deletes it;
the browser is not signed out. Cookie values never appear in logs or diagnostics.

## Package baseline (PLT-04)

Sailjail permissions `Internet;Audio` only. Qt modules `quick qml multimedia network sql` (sql: the local library store). QML imports
and rpm requirements are listed in `build_permissions` of the allowlist; web engines, WebSockets,
Bluetooth, NFC, positioning, sensors, contacts and purchasing imports are rejected.

Playback component build requirements (2026-11-08): `pkgconfig(gstreamer-1.0)`, `gstreamer-app-1.0`,
`gstreamer-video-1.0`, `nemo-gstreamer-interfaces-1.0`, `egl`, `glesv2` and `audioresource-qt`. They
are compile-time headers and link libraries for the app's own GStreamer player (decode, rendering,
the audio resource, PLY-01/20/23); they add no network endpoint, permission or QML import. Media
traffic still goes only to the hosts above (through the loopback MediaProxy on 127.0.0.1). Local-only additions:
Nemo.Notifications (download finished), Sailfish.Pickers (choose an import file), zlib (import archives). One
external open: `components/DownloadsView.qml` (downloads page and section) hands a downloaded local file to the system player until PLY-26.
Catalog component: `VideoPage.qml` and `VideoDetailsPage.qml` open a video or description link in the browser
when the user asks (DSC-14). Local-only addition: Qt D-Bus (`BuildRequires: pkgconfig(Qt5DBus)`) for the
session-bus link service `org.nordilabs.sailtube.SailTube` (LNK-03, PLT-13); it opens nothing on the network.
The catalog's network code goes through `Core/Catalog/Infrastructure/HttpTransport` (two
`QNetworkAccessManager`s, interactive and background, no cookie jar) and the QML image loader
`ImageCacheFactory` (image hosts only).

## Match-only hosts of the account pieces

The pinned SailTune account code (`Shared/SailTune/Core/Accounts`, `Core/Ytm/Sources`, `Core/Ytm/Transport`) names
`.youtube.com`, `music.youtube.com`, `.google.com` and the `accounts.`/`consent.` hosts only to match cookie
domains during an import or rotation and to recognise a sign-in or consent redirect. It makes no request; the
allowlist lists them under `ignored_hosts` with that reason, per file. Signed-in requests are made by
`Core/Account/Infrastructure/AuthTransport.cpp`, to `www.youtube.com` only, already listed under that host.
