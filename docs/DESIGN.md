<!-- SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0 -->
# SailTube: visual and interaction design

Status: v4, 2026-11-10 (for beta 3: bottom navigation, the rebuilt player, cards, chips, shelves, playlist and channel heads). Owner: the design component; v1 was drafted by the ux component,
which keeps settings, translations and accessibility. This is the design reference that all
QML work follows for QML work in `Clients/Sailfish/qml`. Each section says what the app does today,
read from the QML, and then the rules for new work. Where the code does not meet a rule yet, the
section ends with a **Gaps** list; the design component turns those items into polish tasks. The structure follows SailTune's design note (direction, surfaces,
resources, visual verification), expanded into one section per surface. Nothing here counts as finished
until it has been checked on R1 and R2 (§6 of the spec). A mock-up screenshot is not device
evidence.

Sources: `SAILTUBE_REQUIREMENTS.md` (requirement ids such as PLY-14), `docs/CHECKLIST.md`, the
Sailfish OS UI guidance (see References) and the code of this repository.
Clean room: see `docs/CLEAN_ROOM.md`. No GPL client code and no brand assets were used.

---

## 1. Direction and principles

SailTube puts the video first: large thumbnails and picture, readable titles and channel lines,
and one tap from any list to a playing video. The organisation follows what users know from the
official YouTube apps (Home, Subscriptions, a library with history, playlists and downloads,
search with filters, a watch page with related videos and comments, a mini-player), and every
surface is rendered with Sailfish's own interaction: pages and the back swipe, pulley menus,
long-press context menus, remorse, the cover, ambiences. Identity, icon and assets are original.
The user experience is measured from the tap on the launcher icon (§8), not from the first
playing frame.

| Principle | Rule | Where it shows today |
|---|---|---|
| Native Silica first | Use Silica components and `Theme` values. Build a custom component only when Silica has nothing for the job, and list it in this document. | Page, SilicaListView/SilicaFlickable, PullDownMenu, ListItem + ContextMenu, ViewPlaceholder, RemorsePopup / `remorseAction`, DockedPanel, ComboBox, TextSwitch, Slider, PageHeader, SectionHeader, DialogHeader, BusyIndicator, `image://theme/icon-*`. The custom components are `SectionBar`, `PlayerControls`, `FullscreenGlyph`, `MiniPlayer` (built on DockedPanel), the thumbnail badges, `SkeletonList` (loading rows, §5.1) and `Avatar` (round pictures, §4.2). |
| Content first | Thumbnails and the picture are the largest elements. Chrome hides while a video plays. Cached content paints before the network answers. No promotional banners. | The video row thumbnail takes 42 % of the row width. The overlay hides after 3.5 s. The feed paints from its cache (DSC-06). |
| One-hand reach | Put frequent actions where the thumb already is: the pulley item nearest the finger, the long-press menu, the bottom-docked mini-player. Keep the top-left corner free for the back gesture (PLT-06). | The fullscreen control sits bottom right. The mini-player is docked at the bottom. Nothing interactive sits top left on the watch page. |
| No brand assets | No service logos, brand marks, brand colours or brand fonts. The service may be named only in descriptive text (the About disclaimer, "Search YouTube", "Open in browser"). The app icon is original. | `tools/make_icons.py`; the About page disclaimer. |
| Platform limits | Qt 5.6 with ES5 JavaScript only (PLT-05). Decisions live in pure `qml/logic/*.js` (NFR-TST-04). Async callbacks check that their page still exists (PLT-06). | `logic/WatchLayout.js`, `Shell.js`, `SettingsInventory.js`, `Lifetime.js`. |

**Gaps**

- Icon: the distance from the video service's mark is open. The v2 icon (§7.2) was tried and the user chose v1 on the device.

---

## 2. Navigation

### 2.1 Page model

`harbour-sailtube.qml` sets `initialPage: MainPage` and `cover: CoverPage`, with
`allowedOrientations: defaultAllowedOrientations` and `_defaultPageOrientations: Orientation.All`.
Everything else is pushed onto `pageStack`, and Back is the system edge swipe. The window's
`bottomMargin` equals the mini-player's height while the mini-player is open, so pages end above
it.

### 2.2 Main page and the bottom navigation bar

Decision v3 (user request, 2026-11-08): the navigation sits at the **bottom**, as in the official
mobile apps, with six entries in the user's order: **Home, Shorts, Subscriptions, Library, Search,
Downloads**. All six stay at the bottom; none moves into a header.

| Entry | Icon | Section (`sections/*.qml`) | Content |
|---|---|---|---|
| Home | `icon-m-home` | `HomeSection` → `RecommendedView` | Continue watching, then Recommended when signed in, Popular otherwise (DSC-04); the landing for a user without subscriptions |
| Shorts | `icon-m-video` | `ShortsSection` → `ShortsView` | Vertical swipe-up pager of Shorts (§3.5) |
| Subscriptions | `icon-m-media-playlists` | `SubscriptionsSection` → `FeedView` | The RSS-first feed (DSC-05/06/07); default landing for a user who has subscriptions |
| Library | `icon-m-folder` | `LibrarySection` | Playlists, Watch later, History, Channels, Downloads, Import and export |
| Search | `icon-m-search` | `SearchSection` → `SearchView` | Search field at the top, filters, suggestions, results |
| Downloads | `icon-m-cloud-download` | `DownloadsSection` → `DownloadsView` | Downloads (also `DownloadsPage` from Library and the offline shortcut) |

How it renders (`components/NavigationBar.qml`, decisions in `logic/Shell.js`):

- The bar lives in the **window**, below the page stack, on Silica's docked-panel background
  (`Theme.rgba(highlightDimmerColor, opacityOverlay)`) with a hairline on top. Pages end above it
  (`ApplicationWindow.bottomMargin`), and the **mini-player docks above the bar**.
- Six equal entries, each a full-height `BackgroundItem` with a theme icon. **Labels**
  (`fontSizeTiny`) show under every icon when the widest label fits its entry; otherwise only
  under the current entry (`Shell.navigationLabels`), so six entries fit 540 px phones,
  landscape and the largest text size. The bar's height grows with the text size.
- The current entry: highlighted icon and label, and a line along the bar's top edge.
- A dot on Downloads while downloads run (DL-04).
- **Visibility** (`Shell.navigationBarVisible`): on every page except the watch page (immersive
  content, as in the official apps), fullscreen, Dialogs, pages that set
  `navigationBarOptOut` or `miniPlayerOptOut` (settings), and while the keyboard is open.
- **Taps**: on the main page an entry switches the section; on any other page it returns to the
  main page and shows that section (`Shell.barTarget`); a second tap on the current section
  scrolls it to the top (`scrollToTop`).
- **Right-to-left**: entries are placed explicitly and mirrored (Home on the right).
- **Landing** (`Shell.landingSection`, decided once at start): the Settings choice
  (Subscriptions, Recommended, Search). "Recommended" opens Home with or without an account. With
  the default, a user who has no subscriptions yet lands on Home, where Popular gives them
  something to watch at once; an empty feed is never the first screen. After the first
  subscription the next start opens the feed.
- Sections load lazily: only the landing section before the first frame; the others on their
  first visit, then kept (NFR-PERF-01). Each section shows a `PageHeader` (Search shows its
  field instead).
- Gestures: the back swipe and the home/close edge swipes are untouched; the bar is plain
  content above the bottom edge. There are no PushUpMenus in the app (they would sit next to the
  bar); page actions stay in pulleys and headers.

### 2.3 Pulley menus

Rules (PLT-06, DSC-04; the Sailfish guidance asks for fewer than five options so menus also fit
landscape):

- At most **5** declared items; the lint counts them, and a `Repeater` needs a
  `// pulley-dynamic: <max>` note. Aim for 4 or fewer; 3 is comfortable.
- Items are declared top to bottom, so the **last declared item is nearest the finger**. Put
  rarely used items on top (About SailTube, then Settings) and the most used item last.
- Pulleys hold page-level actions only. A destructive item is never the item nearest the finger
  and always goes through remorse.
- While a refresh runs, set `busy: true` on the PullDownMenu instead of adding a spinner to the
  page.
- Do not put setting toggles in a pulley (§6). Link to the setting instead.

Current pulleys, top to bottom (`*` means standalone page only, `()` means conditional):

| Page | Items |
|---|---|
| Home / RecommendedView | About SailTube · Settings · Subscriptions\* · Search\* · Refresh |
| Subscriptions / FeedView | About SailTube · Settings · (Recommended)\* · Search\* · Refresh |
| Library section | About SailTube · Settings |
| Search / SearchView | About SailTube · Settings · Library\* · Search filters · (Now playing) |
| Watch page | (Retry) · Play audio only / Play video · (Quality) · Add to playlist · (Download) |
| Channel | Hide/Show members-only videos · Hide/Show Shorts · Refresh |
| Playlist | (Refresh from YouTube) · (Download all) · (Shuffle) · (Play all) |
| Playlist preview | (Open channel) · Play all |
| Playlists | New playlist |
| History | Pause/Resume history · (Clear history) |
| Subscriptions page | Sort by latest upload / Sort A–Z |
| Video details | Open in browser · Copy link · Share · Play |
| Comments page | (Retry) · (Sort by top) · (Sort by newest) |
| Search filters (dialog) | Reset filters |

### 2.4 Push-up menus

None exist today.

Rules: use a PushUpMenu only at the end of a **finite** list (playlist, history, downloads,
chapters) for actions that belong at the end of the list. Lists that page forever (feed, search,
channel tabs, related, comments) have no push-up menu, because their end moves. They load the
next page when `atYEnd` is reached, show a `BusyIndicatorSize.Medium` footer while loading, and
show an inline tap-to-retry row after an error (§5). Use one paging mechanism per list, never
automatic loading plus a "More" button.

### 2.5 Attached pages

None are used today. Use `pageStack.pushAttached` only for a secondary view that is bound to the
current page and reached by swiping forward, for example the queue panel of PLY-25 (MAY). Never
use it for top-level navigation, for anything the user must find without noticing the page
indicator, or on the watch page while it is immersive.

### 2.6 Watch page reuse and the mini-player

`window.openVideo(videoId, hints)` is the only way to show a video:

1. It clears `miniPlayerDismissed`.
2. It calls `playback.open` only when `videoId` differs from the loaded one; otherwise it
   re-attaches (PLY-10/11).
3. It looks for a page with `isWatchPage === true` already in the stack, pops back to it if
   found, and pushes `pages/VideoPage.qml` otherwise.

The window's `links` connections (LNK-03/04) use the same path. A related video on the watch
page replaces the current video in place, so Back returns to the list (PLY-25).

`components/MiniPlayer.qml` (PLY-11) is a `DockedPanel` with `dock: Dock.Bottom`,
`modal: false` and height `Theme.itemSizeLarge`:

- On the left is a 16:9 picture, `height − 2·paddingMedium` high, on
  `Theme.overlayBackgroundColor`. It shows a live `VideoOutput` only while playing and while no
  watch page holds the surface; otherwise it shows the thumbnail. Beside it are the title
  (`fontSizeSmall`) and the channel (`fontSizeExtraSmall`, `secondaryColor`).
- On the right are `IconButton`s: play/pause, next (only when `Shell.hasNext` is true) and close
  (`icon-m-clear`, which stops playback).
- A progress line runs along the top edge, `max(2, paddingSmall/2)` high, in `highlightColor`.
  It stays at 0 for live streams.
- `Shell.miniPlayerVisible` decides visibility: a session exists, the user has not closed the
  bar, the watch page is not on top, the page has not opted out and the keyboard is closed.

Rules: any page with its own bottom controls, or a long form, declares
`readonly property bool miniPlayerOptOut: true` (as `SettingsPage` does). Every entry point that
plays a video calls `window.openVideo`. Do not push `VideoPage.qml` directly.

**Gaps**

- Leftovers of the shell before the bottom bar: `pages/FeedPage.qml`, `RecommendedPage.qml`,
  `SearchPage.qml` and `LibraryPage.qml` are reachable only from each other's pulleys, never from
  the bottom bar. They ship unused; removing them (and the `standalone` mode of the three views)
  is left for after beta 3.
- The Search pulley has 5 items with one standalone-only item, so it is at the limit. Of the
  first 4, "Search filters" repeats the Filters row under the field.

---

## 3. Watch page

`pages/VideoPage.qml` has a `videoBox` (the picture plus the overlay) on top and an `info`
SilicaFlickable below it. The flickable is hidden while immersive. All layout and chrome
decisions live in `logic/WatchLayout.js` (pure ES5, `tests/decisions/tst_watchlayout.js`). The
player engine belongs to the playback component; the controls drawn on the picture
(`components/PlayerOverlay.qml`, `PlayerSettings.qml`, `SeekBar.qml`, `PlayerEndScreen.qml`) and
the layout belong to the design component.

### 3.1 Picture box

| State | Box | Orientation policy | Info column |
|---|---|---|---|
| Portrait, landscape or unknown video | Full width × `round(width·9/16)`, for example 1080×608 (PLY-14) | `basePolicy` = All | shown |
| Portrait, vertical video (w < h) | Natural height, capped at 60 % of the screen height and never shorter than 16:9 (`boxHeight`) | All | shown |
| Portrait fullscreen (vertical video, setting "Upright") | Fills the page below the top inset, `PreserveAspectFit` | Portrait | hidden |
| Landscape (rotated, or the fullscreen button) | Fills the page; left and right insets equal the cutout depth only when the fitted picture would reach under the cutout | Both landscape directions; works with rotation lock on (PLY-13) | hidden |
| Leaving landscape | Windowed | Portrait is pinned until the device is upright, then `basePolicy` returns (`restoreAfterExit`) | shown |
| Video goes away (audio only, failure) | Fullscreen ends | `basePolicy` | shown |

- Edge to edge (PLY-15): `page.cutoutMode = CutoutMode.FullScreen` is set when available
  (feature-detected). In windowed portrait the box starts below `Screen.topCutout` over a black
  strip. In fullscreen the picture is fitted to the safe area.
- The box height animates with a `NumberAnimation` of 150 ms. The animation is off while
  immersive.
- Until the stream is Ready, the thumbnail lies under the picture, `PreserveAspectFit`, at
  opacity 0.6 (1.0 for audio only). `WatchLayout.loadingChrome` decides what sits on it: a Large
  BusyIndicator while loading, buffering or recovering; the session's `statusText` under it
  (`fontSizeExtraSmall`, `lightPrimaryColor`); and after 12 s in Loading
  (`SLOW_LOADING_MS`) a **Retry** button, so a spinner never sits alone forever (POCO run 2).
- The "Downloading %1 %" note reads at most 99 % until the file is ready
  (`WatchLayout.downloadPercent`): the remux runs after 100 % of the bytes.
- On failure: `errorText` centred in `Theme.errorColor` at `fontSizeSmall`, a **Retry** button
  (PLY-18), and an **Open setting** button when `playback.errorSettingKey` is set (SET-02, §5).
- `ResumeOffer` (LIB-05 "Ask") is centred on a scrim with two Buttons and resumes by itself
  after 5 s.

### 3.2 Player controls (`components/PlayerOverlay.qml`, `PlayerSettings.qml`)

v3 (2026-11-10, the owner's request for YouTube Android parity in Silica's language). The
controls cover the whole picture, as on the official app, instead of a bar along the bottom.

| Area | Content | Silica rendering |
|---|---|---|
| Scrim | Over the whole picture while the controls show | `Qt.rgba(0,0,0,0.45)` (theme-exempt), 150 ms `FadeAnimator` |
| Centre | Previous (with a queue) · **play/pause disc** · next (with a queue). The disc becomes a Large BusyIndicator while loading or buffering and **Replay** at the end (`PlayerUi.centreState`) | Disc `itemSizeMedium × 1.5`, round, `Qt.rgba(0,0,0,0.35)` (darker while pressed) behind `icon-l-play`/`icon-l-pause`/`icon-m-repeat`; previous/next are `IconButton`s (`icon-m-previous`/`icon-m-next`) |
| Top row | **Minimise** (to the mini-player), the **title and channel in fullscreen**, the **autoplay** switch, **CC**, the **settings gear** | `IconButton` `icon-m-down`, inset by `horizontalPageMargin` from the corner where the back swipe starts; Silica `Switch`; a "CC" badge (filled while captions are on); `icon-m-setting` |
| Bottom row | **Elapsed / duration** (a tap shows the time remaining, "-m:ss", `PlayerUi.timeLabels`), the **seek bar**, **fullscreen** | Labels at `fontSizeExtraSmall`; `components/SeekBar.qml` (§3.2.1; seeks on release); `FullscreenGlyph`. This row and the centre row are never mirrored (§7.8) |

All of it uses the theme's light colours, because it always sits on the picture.

Autoplay in the top row writes the same `playback/autoplayRelated` key as its Settings row (one
setting, two places to reach it, as in the official app). This is the one documented exception to
"one control per key" (SET-02); the value has a single source of truth.

**Settings sheet** (`PlayerSettings.qml`, parity gap 7): a modal Silica `DockedPanel` docked at
the bottom of the watch page, at most 75 % of its height, opened by the gear. Each row names a
setting and shows its current value; a tap opens its choices in the same sheet, under a back row.
Rows follow what the session offers (`PlayerUi.sheetRows`, in this order): **Quality** (Automatic or
the current rung), **Playback speed** (0.25–2× in 0.25 steps, "Normal" at 1×), **Captions** (opens
the captions page; current track or Off), **Audio track**, **Loop**, **Sleep timer**, **Stable
volume** (as the playback component adds them), and **Audio only** (a switch). The controls stay up
while the sheet is open (`OverlayController.menuOpen`). Quality and audio only have left the
watch-page pulley for the sheet.

The sheet on the device (found by the owner with the beta 3 candidate, 2026-11-10; the headless
tests could not show either fault):

- **Surface.** Silica's own panel background (`PanelBackground`) is a light tint, 15 to 30 % of
  the highlight background, made to sit on the wallpaper. A panel that slides over content (the
  video, the info column) needs a surface of its own, or the content shows through and the rows
  cannot be read. The sheet draws an opaque base in `Theme.overlayBackgroundColor` (dark in a
  dark ambience, light in a light one, so the theme's text colours read on it in both) with
  Silica's tint on top. It also has a `z` above the info column, which is declared after it.
- **Modality.** A modal `DockedPanel` dims the whole application through a switch on the
  application window and holds it only while it exists. If the panel is destroyed while open
  (its page is popped or replaced) the app stays dimmed. So: while the sheet is open
  `backNavigation` is off, and any press outside the sheet closes it (a Back swipe starts with
  one), never the page; the page hides the sheet as soon as it stops being the active page (Back,
  a link, the mini-player, the next video replacing it); and the sheet turns the window's dimmer
  off itself if it is ever destroyed while open. `tst_sheet.qml` holds all of this.

Rule for any future panel: a `DockedPanel` over content gets its own opaque surface; a modal one
is hidden before its page leaves.

Timing: a tap on the picture toggles the controls; they hide after 3.5 s while playing (not while
the seek bar is held or the sheet is open) and always show while paused or ended.

#### 3.2.1 Seek bar (`components/SeekBar.qml`)

A thin track in `Theme.rgba(lightPrimaryColor, 0.3)` with, from the bottom up: the **buffered
range** (`0.55`, from `playback.bufferedPosition` when the session has it), **SponsorBlock
segments** in the community's category colours (sponsor green, self-promotion yellow, intro cyan…;
theme-exempt, `PlayerUi.segmentRects`), the **played part** in the highlight as it reads on a dark picture (§7.4), and small **gaps
at chapter starts**. The round thumb, in the same colour, grows from `0.8` to `1.5 × paddingLarge`
while held. While dragging, a **bubble** above the finger shows the time, the **chapter name**
(`PlayerUi.chapterAt`) and a **storyboard frame** when the session has one. The
seek happens once, on release (NFR-PERF-03); the controls stay up while the bar is held. The
storyboard is a sprite sheet: `playback.scrubPreview(ms)` returns the sheet and the cell
`{url, x, y, width, height}`, and the bubble shows that cell in a frame of the cell's own shape
(`PlayerUi.previewFrame`: flat for a film, tall and capped for a vertical video); while the
session has no storyboard (the normal case today) the bubble shows the time and chapter only. The bar's
touch area is taller than the track and keeps the drag (`preventStealing`), so scrubbing never
becomes Silica's back swipe.

#### 3.2.2 Gestures on the picture

| Gesture | Effect | Sailfish check |
|---|---|---|
| Tap | Toggle the controls (after the 250 ms double-tap window) | — |
| Double tap left / right | Seek back / forward by the step setting (5/10/15 s); taps accumulate; a ripple on that side with the 10 s icon and the running total ("+20 s") | Inside the picture, away from the edges |
| Long press (while playing) | 2× while held, a "2× ▸▸" pill at the top; release restores the speed (`playback.holdSpeed`, or rate 2 and back) | — |
| Swipe up | Enter fullscreen (windowed only) | Vertical drags inside the page are free in Silica |
| Swipe down | Leave fullscreen; windowed, minimise to the mini-player | The lipstick top-edge swipe starts at the screen edge, outside the picture's area in windowed mode; in fullscreen a swipe that starts on the edge goes to the system, one that starts inside the picture is ours |
| Pinch (fullscreen) | Fill the screen (crop) or fit the picture | — |
| **Dropped:** horizontal drag on the picture to scrub | — | Silica pages go back with a horizontal swipe anywhere on the page; scrubbing on the picture would break Back. The seek bar scrubs instead (it keeps its own drag) |

The decisions are pure functions (`PlayerUi.swipeAction`, `positionAt`, `chapterGaps`) with host
tests; `tst_gestures.qml` drives the page.

#### 3.2.3 Captions, audio track, loop, sleep timer, stable volume

All of it is the session's API (`PlaybackSession`, "Watch overlay"); the page reads and calls it
directly.

- **Captions over the picture**: white `lightPrimaryColor` text on a `Qt.rgba(0,0,0,0.6)` box,
  centred, at most 90 % of the width; `fontSizeSmall`, `fontSizeMedium` in fullscreen. The box
  sits above the bottom row while the controls show and near the bottom of the picture otherwise
  (`PlayerUi.captionBottomMargin`). The text is `playback.captionText`, which runs on the
  session's own cue clock, so captions never ask for the fine position cadence (PLY-24: only the
  visible controls do).
- **CC button** (`PlayerUi.captionsToggle`), shown when `playback.captionTracks` has entries: on →
  off, remembering the track; off → the last track, or the first time a real track in the
  interface language, then an automatic one, then the first track (`PlayerUi.preferredCaption`);
  all through `playback.setCaptionTrack(id)` ("" is off). The settings sheet's Captions row names
  the chosen track and opens the track list (translations included).
- **Audio track** (SEL-04/05): a choice list of `playback.audioTracks` (labels such as "English
  (original)", "Español", descriptive), the selected one in the highlight colour; shown when there
  is more than one.
- **Loop video**: a switch on `playback.loop` (`setLoop`); not on a live stream.
- **Stable volume**: a switch on `playback.stableVolume` (`setStableVolume`), shown only when
  the session's `stableVolumeAvailable` says the switch can change what plays (the playing
  language has both a levelled and a plain track).
- **Sleep timer** (P-01): Off, 15, 30, 45, 60 minutes, End of video (`PlayerUi.SLEEP_CHOICES`,
  `playback.setSleepTimer`); the row shows the time left, or "End of video"
  (`PlayerUi.sleepState` over `sleepTimerSecondsLeft` and `sleepTimerAtEnd`).

A choice tapped in the sheet is applied by a function of the sheet itself (`pickQuality`,
`pickAudioTrack`, `pickSleep`, `pickSpeed`), never by the tapped row: closing the sheet or
applying the choice rebuilds the list, and a row destroyed in the middle of its handler cannot
finish it (found with the audio tracks: the sheet stayed open).

Caption tracks come from the catalog component for streamed videos and from the saved files for
downloads; nothing is fetched until a track is chosen. Two failures, two answers:

- The **track list** could not be loaded: the CC button is absent, as on a video without
  captions. The Captions page says "Captions could not be loaded" and offers Retry.
- A **chosen track's text** could not be loaded: the choice stays (it is the user's), nothing can
  be drawn, and the caption box says "This track could not be loaded" for 4 s
  (`PlayerUi.captionLoadFailed`, the Captions page's own words), again each time the track is
  turned on. A CC badge that reads "on" over a silent picture would look broken.

Automatic translations are not offered in beta 3 (the provider refuses them to anonymous
requests), so nothing here is designed around them.

#### 3.2.4 End of video and errors

- During the autoplay countdown an **Up next** card (the next video's picture and title, "Up next
  in N s", Cancel) holds the centre of the picture; the overlay's disc steps aside.
- After the end without autoplay, the centre disc offers **Replay** and up to **three suggested
  videos** (the first related ones) sit along the bottom; a tap plays one on this watch page.
- **Errors**: the message, Retry, "Open setting" when the error names a setting, "Play video"
  for a listen-only failure, and **Open in browser**, in windowed and fullscreen alike.
- **A live event that has not started** uses the same box, but it is news, not a failure: the
  session's text ("… has not started yet. Scheduled to start <date and time>.") is drawn in plain
  light text instead of the error colour, with Retry and Open in browser under it.
- **The thumbnail behind it all** is dimmed so the text reads on a bright picture
  (`WatchLayout.thumbnailOpacity`): 0.3 under the error box, 0.6 under the spinner and status
  line and before the first frame, whole when a video plays without a picture (listen only).
  Before this the error text and its buttons sat on the thumbnail at full brightness.

#### 3.2.5 The same language elsewhere

- The **mini-player** keeps play/pause, next (with a queue) and close with the overlay's icons,
  and a tap re-attaches the watch page with the full controls.
- **Shorts** (stage 1) play on the watch page, so they get the same overlay; the inline pager of
  stage 2 will reuse `SeekBar` (no chapters) and the long-press 2× gesture.

#### 3.2.6 Touch lock

**Release switch.** The lock is built and tested but has never been on a device, and a lock that
misbehaves can leave the user stuck in fullscreen. It is therefore behind one switch,
`TOUCH_LOCK_RELEASED` in `logic/PlayerUi.js`, which is `false` in the tree: the row never shows.
It goes to `true` only when the device check passes (`docs/DEVICE_DESIGN.md`, step D2). The rest
of this section describes the lock as built.

For watching in fullscreen without stray touches (a pocket, rain, a child). As on the official
app, the settings sheet's last row is **Lock screen**; it shows in fullscreen only
(`PlayerUi.lockOffered`: not on an error, whose buttons must stay reachable, and not at the end,
where Replay and the suggestions are).

While locked:

- The controls stay away, and the picture reads no tap, double tap, hold, swipe or pinch.
- Back is off (`backNavigation`), and a fullscreen entered by rotation alone holds landscape.
- Any touch shows **Unlock**, a pill at the bottom centre with a lock icon, for 3 s; it also
  shows when the lock starts, so the way out is seen once. A tap on it unlocks and brings the
  controls back.

The lock ends by itself when fullscreen ends (the video goes away), on an error and at the end of
the video. Sailfish's own edge gestures (to Home, to the events view) are the system's and stay.
Known limit: when the queue moves to the next video the watch page is replaced, and the new one
starts unlocked.

### 3.3 Edge glow (`WatchLayout.showEdgeGlow`, PLY-16)

| State | Navigation indicator |
|---|---|
| Setting `playback/edgeGlow` = hidden | hidden |
| Immersive, or the page is landscape | hidden |
| Playing with the controls hidden | hidden |
| Windowed portrait, paused or controls visible | shown |

The back swipe works in every state. Only the indicator is hidden.

### 3.4 Info column order

v3 (2026-11-10), in the order of the official app, with Silica parts.

| # | Block | Component and style |
|---|---|---|
| 1 | Title | Label, `fontSizeMedium`, `highlightColor`, at most 3 lines |
| 2 | Meta | "Live now · views · age", `fontSizeExtraSmall`, `secondaryColor`, directly under the title; then the stream or status line at `fontSizeTiny` (DSC-14) |
| 3 | Owner row | BackgroundItem that opens ChannelPage: round avatar `iconSizeMedium` (DSC-19), or the channel's initial on a tint while there is none, as on the cards; the name (`fontSizeSmall`, `primaryColor`) with the subscriber count under it (`fontSizeExtraSmall`, `secondaryColor`); the app's one Subscribe button at the end (`components/SubscribeButton.qml`, compact, highlighted while not subscribed; LIB-01). Anchored throughout, so it mirrors |
| 4 | Notes | "Playing downloaded file" with "Stream instead" (PLY-26); non-fatal playback messages; "Downloading N %" (at most 99 % until the file is ready) or "Downloaded" (DL-04) |
| 5 | Action chips | One row that scrolls sideways (`components/ActionChip.qml` in `components/ChipStrip.qml`, the strip every chip row uses; `StopAtBounds`; starts at the right end in right-to-left layouts): the **like count** (a passive chip), the **dislike estimate** when the user turned it on in Settings (a passive chip with the same thumb pointing down and "≈" before the number; RYD-01), **Share**, **Download**, **Add to playlist**, **Copy link**, **Open in browser**. A chip is a pill `itemSizeExtraSmall` high on `Theme.rgba(highlightBackgroundColor, 0.15)` (0.4 while pressed) with a small theme icon and a `fontSizeExtraSmall` label. Below it the TextSwitch "Link at the current time" and a feedback line for 3 s |
| 6 | Description | A card: a rounded box on `Theme.rgba(highlightBackgroundColor, 0.1)` (0.3 while pressed) with the StyledText description collapsed to 5 lines and "Show more / Show less" in the highlight colour; a tap anywhere on the card toggles it; links, timestamps and hashtags stay active (the text takes only taps on links) |
| 7 | Chapters | SectionHeader, then a row "%n chapter(s) · now: …" that opens ChaptersPage (DSC-15) |
| 8 | Comments teaser | A card like the description's: "Comments 7.7K" with a forward arrow and, once the comments are loaded, the first comment (avatar and two lines). A tap opens `CommentsPage`. It never starts a load itself (DSC-16). With related videos as cards the comment list sits far down; the teaser is the way to it |
| 9 | Related | SectionHeader, then the first 5 as **cards** in portrait (§4.1; compact rows in landscape) with badges and the shared menu, then "More related videos" (DSC-13) |
| 10 | Comments | `CommentsSection` (DSC-16), the lazy inline list |

Rules: new watch-page content goes into this order. A block is hidden while it is empty. Nothing
here may delay the first frame (DSC-14). The pulley keeps Retry, Add to playlist and Download;
Quality and Audio only live in the player's settings sheet.

**Gaps**

- The chip row scrolls horizontally inside a page that goes back with a horizontal swipe: check on
  the device that Back still works from the row's start.

### 3.5 Shorts

The Shorts entry of the bottom bar opens a vertical, swipe-up feed: one Short per screen
(`SilicaListView`, `SnapOneItem`, `StrictlyEnforceRange`), no pulley (it would fight the swipe).

Data: catalog's `shortsModel` (the subscribed channels' Shorts tabs, then popular Shorts for the
locale, then the account's sequence when signed in; vertical thumbnails), with `setCurrent()` on
every settled page and `loadMore()` near the end. Every build has the model (the pager once had a
fallback over the subscriptions feed for a build without it; it could no longer run and went).

**Reading a row that changes later.** A Short's details (counts, channel, avatar, related video)
arrive after its row exists, and `get()` hands out a snapshot that tells nobody when the row
changes. A binding on a bare `get()` is evaluated once per page: the rail and the bottom block
stayed empty until the page was recreated (found by catalog, 2026-11-11; the first test passed
because its model never changed). Everything read from a row therefore depends on
`ShortsView.dataStamp`, which moves with the model's `dataChanged` and its read-only `revision`;
`tst_shorts.qml` changes a row's details after load with `setProperty()`. The same rule holds for
any list that reads rows through `get()`. The pager also tells the model when the Short on screen
plays (`playbackRunning`), so its details requests wait for the first video bytes, not 2.5 s.

**Stage 2a, a Short plays in place** (`components/ShortsView.qml`, decisions in
`logic/ShortsUi.js`; requested for beta 3, 2026-11-11). Entering the tab plays the
current Short at once, full screen, upright and looping, in the shared player; no watch page and
no overlay controls.

- **One live picture.** Every page shows its poster (the vertical frame, `PreserveAspectCrop` on
  black). The page the pager has *settled* on (not the middle of a drag) adds a
  `VideoSurfaceItem` over it. A vertical Short fills the screen; a wider one is shown whole on
  black (`ShortsUi.cropsToFill`). The live picture shows only when the session holds this Short, is
  `Ready` and the surface has a frame (`ShortsUi.surfaceShown`): a session that is switching may
  still hold the previous Short's last frame, and the poster (with a spinner) covers the gap, so a
  swipe never shows black or the wrong picture.
- **A swipe** moves to the next or previous Short; when the pager settles, `playback.openShort(id,
  hints)` opens it (the session loops it, stores no resume point and never parks it), then
  `playback.shortsAhead([next, afterNext])` asks for the next two (the first resolved and held for
  ten minutes, the second only resolved), then catalog's `setCurrent()` prefetches the details. The
  Short left behind stops by being replaced. Host numbers for the first frame after a swipe: about
  60 ms for a prepared Short, 150 ms for one only resolved, 460 ms for an unprepared one. Swipes
  are the list's vertical flicks, so they never meet Silica's back gesture (horizontal, from the
  edge) or the bottom bar (outside the pager).
- **A tap** is the user's own pause (`playback.togglePlayback()`), with a pause mark; it survives
  a hold and a release, so coming back to a Short the user paused leaves it paused. Nothing else
  on the picture takes a tap. A long press opens the shared row menu.
- **A 2 px progress line** along the bottom edge, filling from the left in both directions. The
  session sends one position a second while the controls are hidden, and the line grows linearly
  over the second between two updates; a looping Short's position jumps back to 0 at each loop and
  the line jumps with it, never animating backwards (`ShortsUi.progressMove`). Until the session
  knows the duration the row's own stands in (`ShortsUi.progressFraction`).
- **What plays when** (`ShortsUi.sessionAction`): the Shorts section is the current one with the
  main page on top. When it is covered (the tab left, or a pushed page, dialog or another video
  on top) the pager holds the Short (`holdShort`), and releases it when the section is back; the
  first hold pauses, the last release resumes unless the user paused meanwhile. Coming back
  resumes the same Short without opening it again, or opens it again if the session meanwhile
  held another video. A locked screen and the app leaving the front hold a Short by themselves,
  and the cover shows the paused Short; the pager only lets the display go off
  (`window.videoVisible`).
- **Around it.** A Short is never parked: it has no mini-player, on the Shorts tab or any other
  (it would show the same video twice); the bottom bar stays. The display stays on while a Short
  plays. The list is fetched again only when it is empty or after ten minutes away
  (`ShortsUi.shouldRefresh`), so a quick return keeps the Short.
- **A Short that cannot play** shows the reason and Retry (when the session can retry) over its
  poster; the pager still moves on, and the next Short's open replaces the failed one at once.

**Stage 2b, the rail and the information** (`components/ShortsAction.qml`; data from catalog's
Shorts rows, read by name: `likeText`, `commentText`, `commentsDisabled`, `channelId`,
`channelAvatar`, `description`, `relatedVideoId`, `relatedVideoTitle`; unknown is "" or -1). Both
blocks are part of the page, so they move with the swipe; everything over the picture sits in a
`SilicaItem` with `palette.colorScheme: Theme.LightOnDark`, so Silica's own controls (Subscribe)
draw light-on-dark in every ambience.

- **Right rail**, bottom right, top to bottom, each an icon on a dark translucent disc (so it reads
  over any picture) with its count under it in an outlined label: **like** (`icon-m-like`, the
  thumb the watch page uses), **comments** (`icon-m-chat`), **save** (`icon-m-add`), **share**
  (`icon-m-share`). Mirrored to the left in right-to-left layouts. There is no dislike here: the
  official app dropped it from the rail in 2026; the estimate stays on the watch page.
  - *Like* shows the count when the creator shows one and the thumb alone otherwise. SailTube
    cannot like (no account feature writes one), so a tap says so in a notice ("Liking videos is
    not available yet"); the button stays, so the rail keeps its shape.
  - *Comments* open the existing comments page with the Short's id and an empty token (the
    model fetches the watch data itself); the Short pauses under the page like under any pushed
    page. Switched off, dimmed and inert, when the creator turned comments off.
  - *Save* opens the add-to-playlist dialog for this Short; *Share* the system share sheet with
    `youtube.com/shorts/<id>` (`ShareLinks.shortVideo`), or copies the link and says so when
    there is no share sheet.
  - The notice is a dark pill over the picture, 3 s (Silica has no notice component in this SDK).
- **Bottom left**, to the left of the rail, over the dark gradient: one line with the round
  avatar, the channel's name in bold (a tap on either opens the channel) and the app's one
  `SubscribeButton`; the **caption**, which is the Short's title (almost no Short has a description
  of its own), in two lines: a tap opens it to eight, with the description under the title when
  there is one, and a second tap closes it; the **related video** as a chip with its picture
  (the video icon without one) and its title (a tap opens it on the normal watch page; about
  half of the popular Shorts have one, a long video or the source Short of a remix).
- **No jumping.** A popular Short has no channel, counts or description until its details arrive
  (about 0.3 s for the Short on screen and the next one). The rail always has its four buttons;
  the channel line keeps its height (a tinted disc stands in for the avatar, no Subscribe
  without a channel id); the related chip's row keeps its room whether or not there is a chip.
  What arrives only fills in.
- Not in 2b, on purpose: double tap to like (the single tap pauses at once, so the double tap
  would blink the pause), press and hold for 2x (the long press is the row menu, which carries
  Download and Details; it moves with an overflow menu), a scrubbing progress line, next and
  previous buttons for a screen reader.

**Stage 2c** (comments as a sheet over the Short) follows, after a device check of 2a and 2b. It
is the one piece that repeats the settings sheet's risk (a modal panel over live video with a
list inside), so it takes that sheet's lessons (§3.2): an opaque surface, `backNavigation` off
while it is open, hidden before its page leaves, and the window's dimmer reset if it is ever
destroyed open. Until then comments are the page.

---

## 4. List items

### 4.1 Video rows (`components/VideoListItem.qml`)

**Cards (v3, 2026-11-10, parity with YouTube Android).** Home, Subscriptions and Search set
`cardLayout: true`. On a portrait-wide row (`Rows.useCard`: the row is no wider than the screen's
short side) the item is a card:

- a **full-width 16:9 thumbnail**, edge to edge, carrying the same badges (duration or LIVE,
  watched progress, download state);
- under it, at `horizontalPageMargin`: the channel's **round avatar** (`iconSizeMedium`; catalog's
  `channelAvatar` role), or a round placeholder with the channel's **initial** in the highlight
  colour while the model has none;
- beside the avatar: the **title** (`fontSizeMedium`, 2 lines) and one **meta line**
  "channel · age · views" (`fontSizeExtraSmall`, secondary colour, fades);
- `paddingLarge` below, so cards read as separate without dividers.

Landscape rows, lists that do not opt in (Library, history, playlists, related, channel pages)
and channel results keep the compact row below. Long press opens the same menu; a tap opens the
video. The loading skeleton takes the card's shape (`SkeletonList.cards`).

Pictures: the card uses catalog's `thumbnailLarge` role when the model has one and decodes it at
the card's width; with only the list thumbnail (320 px wide) it decodes at no more than 640 px and
the GPU scales the rest (`Rows.decodeWidth`), so a card never costs a CPU upscale.

Prefetch (E12): `VisibleRows` reports up to three rows of which at least half is on screen
(`Rows.rowOnScreen`); with compact rows that is what "fully visible" gave, and with cards it
includes the half-visible third card a user can already tap.

**Compact row.**

- The row is a `ListItem` whose `contentHeight` is `max(thumb, texts) + 2·paddingMedium`.
- The thumbnail sits at `horizontalPageMargin` (anchored, so it mirrors in RTL),
  `Rows.thumbWidth` wide: 42 % of the portrait width, and the same size in landscape (the row
  does not grow taller when it gets wider). It is 16:9, on a
  `Theme.rgba(highlightBackgroundColor, 0.1)` placeholder. The image uses
  `PreserveAspectCrop`, `sourceSize.width` and `asynchronous: true`; the lint enforces the last
  two.
- The text column starts `paddingMedium` after the thumbnail:
  - title: `fontSizeSmall`, at most 3 lines, elided, `primaryColor` (`highlightColor` when
    pressed);
  - "channel · published": `fontSizeExtraSmall`, `secondaryColor`, fades;
  - "N views": `fontSizeExtraSmall`, `secondaryColor`, with `Format.count` and the Qt locale.

Thumbnail badges (`enabled: false`, so taps pass through, LIB-06):

| Position | Badge | Style |
|---|---|---|
| Top left | "Watched" (`WatchOverlay`) | Scrim `rgba(0,0,0,0.75)`, radius `paddingSmall/2`, `fontSizeTiny`, `lightPrimaryColor` |
| Top right | "NN %" or "Saved" (`DownloadBadge`, DL-04) | Same |
| Bottom right | Duration; "LIVE" on `Theme.errorColor`; "UPCOMING"; playlist count | Same |
| Bottom edge | Watch progress above 0.5 % (`WatchOverlay`) | Track `rgba(1,1,1,0.35)`, fill `highlightColor`, `max(2, 5 %)` of the thumbnail height |

Members-only and restricted rows prefix the channel line ("Members only · %1"). Hidden rows are
counted in the list footer and never dropped silently (DSC-18).

Rules: every list of videos uses `VideoListItem` rather than a copy. It sets `videoId`, so the
overlays work, and `menu: Component { VideoMenuItems { video: … } }`. It opens the video with
`window.openVideo(id, row)`.

### 4.2 Channel rows

What exists today:

- Search results draw channels with `VideoListItem`: the avatar is cropped into the 16:9 slot
  and `subscriberText` is the second line.
- `SubscriptionsPage` rows are `itemSizeMedium`, with an `iconSizeMedium` avatar and the name.
- The `ChannelPage` header shows an `itemSizeLarge` avatar in a Rectangle with `radius: width/2`,
  the subscriber count in `highlightColor` and the video count at `fontSizeSmall` in
  `secondaryHighlightColor`. Below it is a row of tabs that share the width equally while their titles fit and scroll sideways when one does not (long titles such as "Listas de reproducción" were cut; ux's per-language layout test found it); the row rests on the current tab; the
  current tab is highlighted.

Rule: a channel row is a ListItem at least `itemSizeMedium` high, with a **round** avatar
(`components/Avatar.qml`; `iconSizeMedium`, or `iconSizeLarge` in search), the name in the
primary colour and the subscriber count in the secondary colour. It never uses a 16:9 frame.
`Avatar` masks the decoded picture with a small fragment shader that samples the Image as a
texture (no `layer`, so it is cheap in delegates); a non-square picture is cropped to its centre
square, and a round placeholder in the thumbnail tint shows until it loads.

### 4.3 Playlist rows

What exists today:

- `PlaylistsPage` rows are `itemSizeLarge`: a 16:9 thumbnail `contentHeight − 2·paddingSmall`
  high, the title, and "%n video(s) · channel". Watch later comes first and has no menu.
- Search results put the count in the duration badge.
- The ChannelPage playlist tab uses its own delegate with a "%n videos" badge.

Rule: a playlist row has a 16:9 thumbnail with the count badge at bottom right, the title, and
the owner or count in the secondary colour.

### 4.4 Other rows

| Row | Pattern |
|---|---|
| Downloads | Thumbnail, title, then "kind · state · NN %" (`errorColor` when failed), then a `ProgressBar` while running or paused |
| Library hub | `Icon`, title and detail line, `itemSizeMedium`; an unavailable row uses `opacityLow` and "Not available yet" |
| Chapters | Start time in `secondaryHighlightColor`, then the title; the current chapter is highlighted; tapping seeks and pops back |
| Comments | Avatar `iconSizeMedium` (replies `iconSizeSmall`, indented `2·paddingLarge`); meta, body and likes lines; "More replies" row |
| Suggestions | `itemSizeSmall` Label; history entries use `secondaryColor` and have a "Remove from history" menu |

### 4.5 Context menus (`components/VideoMenuItems.qml`)

The entries, in order: Add to playlist · Watch later / Remove from Watch later · Mark as watched /
Mark as not watched · Subscribe to %1 / Unsubscribe from %1 (only for `UC…` ids) · Download
audio · Download video.

Rules:

- A long-press opens the context menu everywhere. The details-only view (DSC-14) becomes the
  menu's first entry, "Details".
- Most used entries come first; Silica opens the menu just below the pressed row.
- A destructive entry comes last and goes through remorse.
- A page-specific menu extends `VideoMenuItems` (for example "Remove from playlist") and does
  not replace it.

### 4.6 Search

- The field stays outside the result list (the keyboard stays open). Under it, a **chip row**
  that scrolls sideways (`ActionChip`, `StopAtBounds`): **Filters** (`icon-m-levels`; opens the
  filter page; while filters are active it is marked and reads "Filters: sort · date · …"), then
  **All**, **Videos**, **Channels**, **Playlists** and **Live** as quick choices that apply at
  once (`searchModel.type`, `searchModel.live`) and show the current one.
- Suggestions carry a leading icon: a clock (`icon-m-history`) for an earlier search, a magnifier
  (`icon-m-search`) for a suggestion; an earlier search keeps its "Remove from history" menu.
- Results are cards in portrait (§4.1); a channel result keeps its row with the round avatar.
- A chip's touch target is `itemSizeSmall` high; the pill drawn inside is `itemSizeExtraSmall`.
- A search that starts elsewhere lands here: a **hashtag** tapped in a video's description runs
  its search, leaves the watch page for the Search section and shows the query in the field. The
  field follows the model's query only while the user is not typing in it.

### 4.7 Library tab (`sections/LibrarySection.qml`)

As the official app's "You" tab. Under the header, two **shelves** that scroll sideways
(`components/ShelfTile.qml`: a 16:9 picture 42 % of the portrait width, a two-line title):

- **History**: the most recent videos, each with its watched line; a tile opens the video.
- **Playlists**: Watch later and the playlists, each with its item count on the picture; a tile
  opens the playlist.

A shelf's header row (title and a forward arrow) opens the full page. Below the shelves, one row
per collection: Playlists, Watch later, History, Channels, Downloads, Import and export. Signed
in to a YouTube account, two more rows follow Watch later: "YouTube Watch later" and "Liked
videos", the account's own lists (ACC-11; they were on the old library page and had no way in
from the bottom bar). A row
whose shelf is showing is left out, so nothing is listed twice; a shelf without content is hidden
and its row is back. The shelves live in the list's header and scroll away with it.

Channel rows (search results) carry a **Subscribe** button at the row's end
(`components/SubscribeButton.qml`, loaded only for such rows): the watch page's texts, remorse
before unsubscribing.

Note for QML in list headers: inside a ListView the name `section` is the view's own property,
so a root item must not be called `section` when nested lists read it (the hub's id is `hub`).

### 4.8 Playlist and channel heads

**Playlist** (`components/PlaylistHeader.qml`, on `PlaylistPreviewPage` and `PlaylistPage`). As on
the official app, in Silica's language. Under the `PageHeader` (title; owner and video count):

1. The **cover**, centred, 62 % of the width (half the portrait width in landscape), 16:9: the
   playlist's picture, or the first video's for a local list. Hidden without a picture.
2. **Play all**, one Silica `Button`: the list becomes the queue and its first video opens.
3. A strip of chips (`components/ChipStrip.qml`, the watch page's chips): **Shuffle** (the list in
   random order as the queue), then **Share** and **Copy link** when the list has a public address.

Each part shows only when the list can do it (`ShareLinks.headParts`): no Play all on an empty
list, no Shuffle with one video or without a queue, no Share on a local list. A strip without
chips takes no room. Play all and Shuffle are not repeated in the pulley; what is left there is
what is used less (save to library, open the channel, download all, refresh). A pulley with
nothing to offer is hidden.

The queue is one call for every list: `window.playQueue(rows, index)` opens row `index` on the
watch page and hands the ids to `playback.setQueue` (`Shell.queuePlan`; rows without a video id
are left out).

**Channel** (`pages/ChannelPage.qml`). Under the avatar and counts, the channel's **description**
on a tinted card: two lines, and a tap opens it in place (the watch page's description card, the
same "Show more" and "Show less"). **Share** is the first pulley entry, the Silica place for the
official app's overflow menu.

**Links.** `logic/ShareLinks.js` writes the address from the id alone
(`/channel/<id>`, `/@handle`, `/playlist?list=<id>`); an id that is not a plain YouTube id gives
no link and no Share entry. The address goes to the system share sheet or the clipboard and is
never requested by the app (docs/PRIVACY_NETWORK.md).

### 4.9 Home: Continue watching (`components/ContinueShelf.qml`)

As on the official app's Home. Above the list (Popular signed out, the recommendations signed in),
one shelf that scrolls sideways with the videos left **unfinished**: started (5 % or more) and not
marked watched, newest first, at most ten, each tile (`ShelfTile`) with its watched line. A tile
opens the video, which resumes where it was left; the shelf's header row opens the history.

- The rows come from the newest 40 history rows (`Rows.continueWatching`). They are read when Home
  comes on screen, so a video watched meanwhile moves or leaves; the shelf is rebuilt only when
  its content changed (`Rows.continueKey`), and keeps its scroll position otherwise.
- Nothing to continue: the shelf takes no room and its file is not loaded (start-up rule, §8.1).
- It is local: the history on the device, no request, the same with or without an account.

---

## 5. States

### 5.1 Loading

| Situation | Indicator |
|---|---|
| Empty list, first load (nothing cached) | `SkeletonList` in the list header: placeholder rows with `VideoListItem` geometry, plus the PullDownMenu's `busy` line |
| Next page | Footer item `itemSizeLarge` high with a `Medium` BusyIndicator |
| Background refresh with content shown | PullDownMenu `busy: true`; the standalone feed also shows an `ExtraSmall` indicator in its PageHeader |
| Inline parts | Comments header `Small`, replies `ExtraSmall`, details thumbnail `Small` |
| Player | `Large`, centred on the picture |

Rules (NFR-PERF-01):

- A list is in exactly one of three states, decided by `Rows.emptyState(count, busy, pending)`:
  **rows** (cached or fresh), **skeleton** (a first load is running or about to start), or
  **placeholder** (loaded and empty, or failed). A list is never blank, and a list that is still
  loading never says "No videos".
- Cached rows always win: show them at once and refresh behind them (stale-while-revalidate,
  DSC-06). The skeleton appears only when there is nothing to show yet.
- The skeleton (`components/SkeletonList.qml`) has the row geometry of `VideoListItem`, so the
  real rows land where the shapes were; it uses the thumbnail placeholder tint and
  `Theme.rgba(primaryColor/secondaryColor, 0.1)` text bars, covers the view
  (`Rows.skeletonCount`, at most 12 rows) and sits in the list header, so it moves with the
  pulley. It takes no input. Its one motion is a slow opacity pulse of the whole column
  (2 × 700 ms, `OpacityAnimator`, render thread), running only while it is visible and the app is
  in the foreground.
- Lists with rows of another shape (comments, channels only) use the Silica BusyIndicator until
  a matching skeleton exists.
- Every video list follows this, the pushed pages included: the channel page, the playlist
  preview and the related videos show the skeleton, not a spinner in the middle of the page.
- A next page that failed shows `components/RetryRow.qml` (the error and "Tap to retry." in one
  touch target) in the footer of every list (§5.3).
- While watch-next loads, the watch page shows what the row hints already carry (title, channel,
  thumbnail), and the picture box shows the thumbnail under the busy indicator.

### 5.2 Empty

Every list has a `ViewPlaceholder` inside its flickable:

- `text` names what is missing, briefly, with no final period: "No subscriptions",
  "No videos", "No history", "No downloads", "Find videos", "No chapters".
- `hintText` says what to do, as a sentence with a period: "Subscribe to channels to see their
  new videos here.", "Pull down to refresh.", "Long press a video anywhere to add it.".

A page is never blank.

### 5.3 Errors

| Level | Pattern |
|---|---|
| Whole list | ViewPlaceholder: `text` is the error and `hintText` the recovery ("Pull down to refresh."), backed by a Refresh pulley item |
| End of a list, or inline | BackgroundItem "%1 Tap to retry." (ChannelPage, PlaylistPreview, comments, replies); RelatedPage uses a Retry Button. Scrolling never retries (DSC-16). |
| Player | Centred column: message in `Theme.errorColor`, **Retry**, **Open setting** |
| Field | Label in `errorColor` at `fontSizeExtraSmall` under the control (download folder) |

The "Open setting" link calls `window.openSetting(key)` → `Inventory.locate(key)` → it pushes
`SettingsPage { focusKey }`, which scrolls to the control and flashes it for 1500 ms (SET-02,
`tests/qml_ui/tst_errorlinks.qml`). Error text never names a page; it links instead.

Rule: show error messages in `Theme.errorColor` and the retry affordance in `highlightColor`.

### 5.4 Remorse for destructive actions

| Action | Mechanism | Text |
|---|---|---|
| Delete a playlist / remove a saved one | `ListItem.remorseAction` | "Deleting" |
| Remove a video from a playlist | `remorseAction` | "Removing" |
| Remove a history entry | `remorseAction` | "Removing" |
| Clear the whole history | `RemorsePopup.execute` | "Clearing history" |
| Cancel a download or delete a downloaded file | `remorseAction` | "Removing" |
| Unsubscribe (SubscriptionsPage) | `remorseAction` | "Unsubscribing" |

Rules: use remorse, never a confirmation dialog. Commit the change only when the remorse ends
(LIB-02). Use a row remorse for one item and a RemorsePopup for a page-wide action.

**Gaps**

- Error colours: every video list ends a failed page with `RetryRow` in `highlightColor` (it is
  a tap target, not a failure state); the player, downloads and settings use `errorColor` for
  the failure itself. Left: the comments list still shows its inline error in
  `secondaryHighlightColor`.
- No "Open setting" link exists outside the player. Candidates: "Everything here is hidden by
  filters", "All results are hidden", and a download held back by the unmetered-only setting.

---

## 6. Settings

The pattern is `pages/SettingsPage.qml`: one SilicaFlickable column with a PageHeader, then a
SectionHeader per `Inventory.SECTIONS` (content, history, playback, quality, audio, integrations,
downloads, providers, diagnostics). Each row is one of:

| Component | Base | Behaviour |
|---|---|---|
| `SettingSwitch` | TextSwitch | `automaticCheck: false`; a click writes `!stored` |
| `SettingCombo` | ComboBox | `labels` follow the inventory's value order; the first-bind write is suppressed (`Inventory.isUserChange`, PLT-06) |
| `SettingSlider` | Slider | Range and step come from the inventory; the value saves on release (`clampNumber`) |
| `SettingLink` | BackgroundItem, at least `itemSizeMedium` | Opens a sub-page; `icon-m-right` is mirrored in RTL; without a page it is disabled with "Not available yet" |

Each control carries `settingKey`, a `store` (`appSettings`, or an adapter that has
`revision`/`value`/`setValue`) and `flash`. FULL-only rows are hidden on STORE. A
`restartRequired` note (`highlightColor`) appears when a decoder setting changes. The page sets
`miniPlayerOptOut: true`.

Rules:

- One control per key (SET-02). A new setting is added to `Core/Settings/Domain/SettingsDefaults.cpp`
  (SET-03) and `logic/SettingsInventory.js`, then gets exactly one control with `settingKey`. The
  lint `check_setting_keys` and `tst_settings.qml` check this.
- Settings apply at once. If one cannot, show the restart note.
- Toggles for settings never appear in pulleys, dialogs or other pages; those places link to the
  setting with `openSetting`.
- The label is a short noun phrase in sentence case. The `description` is one sentence, ending
  in a period, that says the effect.
- A variant limitation is stated in the description next to the setting (PLY-12 on STORE).

**Gaps**

- More duplicate toggles: the HistoryPage pulley (Pause history), the ChannelPage pulley (Hide
  Shorts and members-only) and SearchFilterPage (Hide Shorts and members-only).
- "Also in search results" (`hideWatchedInSearch`) has no inventory key.
- Caption style is a SettingLink disabled with "Not available yet". The Equalizer row and the
  Resolver updates row are hidden in beta 3 (`SettingsInventory.HIDDEN_KEYS`: no engine reads them).
- "Keep display on" shows on STORE, where it has no effect, and has no limitation note
  (PLY-12).
- The "Library and downloads" row sits above the first SectionHeader. "Video capabilities" is a
  bare BackgroundItem without the SettingLink arrow. "Player client" is a combo with a single
  choice.

---

## 7. Visual system

### 7.1 Cover (`cover/CoverPage.qml`)

Today (playback's state with ux's layout): the session's `coverMode` picks one of
three states. **Session**: thumbnail (16:9, top), title, channel and a progress line (hidden for
live); play/pause plus next with a queue, otherwise play/pause plus stop. **Continue watching**:
the last resumable video with a heading and one play action (`playback.continueWatching()`).
**Identity**: app icon and name. `tst_cover.qml` covers the states (MPR-02).

Rules (MPR-02):

- Two cover actions at most; the right one is next with a queue, stop otherwise.
- No timers or animations run while the cover is not `Cover.Active`.
- Text never sits on moving or arbitrary pictures: title and channel are on the cover
  background in theme colours, so they stay legible in every ambience.

#### Live video on the cover (user-approved, the Sailfish rendering of picture-in-picture)

Sailfish gives apps no PiP surface, but the active cover is visible on the home screen and can
show the playing video. The user approved this as SailTube's PiP (2026-11-08). Engine: playback
(same frames as the watch page, about 15 fps, only while `Cover.Active`, frozen otherwise):
`CoverVideoItem` (`org.nordilabs.sailtube 1.0`, `source: playback.mediaObject`, active only while
`Cover.Active`, `PreserveAspectFit`, at most 15 fps, read-only `hasFrame` and `framesDrawn`),
`playback.coverVideoActive`, `playback.setCoverVisible(bool)` (decoding continues in the
background only while the cover is visible), diagnostics `coverFramesDrawn` and
`coverDecodeSeconds`, timing markers `cover-video on|off`. Setting `playback/coverVideo` ("Video
on the cover"): on in FULL, off in STORE. Layout and the setting row: ux. Design rules:

| Aspect | Rule |
|---|---|
| Picture area | Top of the cover, full width. Landscape and unknown video: a 16:9 area (`CoverVideoItem` fits, so a 16:9 video fills it and other landscape ratios get thin bars). Vertical video: the area grows to at most 55 % of the cover height, fitted on black (`theme-exempt`). |
| What fills it | Live frames (`CoverVideoItem.hasFrame`) while playing, the cover is `Cover.Active`, the video has a picture and the setting is on. Paused: the last frame, still. Otherwise the thumbnail (audio only, setting off, no frame yet, failure, live stream not started). |
| Transition | The thumbnail stays underneath; the first live frame replaces it with one 200 ms opacity fade (Animator). No other motion; the progress line moves only on position updates. |
| Text | Title (2 lines on a large cover, 1 on a small one) and channel (large cover only) below the picture on the cover background. Nothing drawn over the video except the progress line along its bottom edge (`highlightColor`, 2 px or `paddingSmall/2`). |
| Small covers | `Cover.Small` (many apps on the home screen): picture and one title line; no channel. |
| Ambiences | Text in theme colours on the cover background; the black behind vertical video and the picture are the same in light and dark ambiences. |
| Actions | Unchanged (play/pause + next or stop). |
| Power | Frames are produced only while `Cover.Active`; on the lock screen, in the app, or with the display off nothing is rendered or decoded for the cover (NFR-PERF-06). |

Device check (§6 M6 extended; design asks the maintainer): home screen with the cover visible
vs the app in front vs the display off — `framesDrawn` / `coverFramesDrawn` grow by about 15 per
second only while the cover is visible and stop otherwise, `coverDecodeSeconds` grows only
while it is visible, and the timing log shows `cover-video on|off` at each change; battery and CPU over 10 minutes at
the home screen with a playing video against audio only; light and dark ambiences; small and
large cover sizes; a vertical video (Short); setting off falls back to the thumbnail.

### 7.2 App icon

The icon is in `icons/{86,108,128,172}x…/harbour-sailtube.png`, generated deterministically by
`tools/make_icons.py`. `tests/icons/run.sh` checks the sizes, `SAILFISHAPP_ICONS` and the spec.
The About page shows the 172 px icon.

v2 (design component, 2026-11-08): a sailing boat at dusk on a round sea — a teal-to-indigo disc, a low
amber sun behind the boat, a mainsail with a curved leech and a jib in front of the mast, a cream
hull and two light wave lines. v1 put a white, play-shaped triangle on a red disc, which read too
close to the video service's own mark (red with a white play triangle); v2 keeps the boat
(SailTube, sibling of SailTune) and drops both the red and the play shape. Legible at 86 px (the
two sails and the sun stay distinct). Approved on 2026-11-08, then **reverted to v1 at the
user's request after seeing both on the device** (d2a4350, beta 1). v1 ships: the dusk disc
with a play-shaped sail. The brand-distance note above stays open for a future revision.

Rules: original artwork only, within Sailfish's family of round and square silhouettes; no red
disc and no lone right-pointing triangle (brand distance); change the icon through the script
only. Check it on the real launcher grid at all four sizes, in light and dark ambiences, before
approving it (device check).

### 7.3 Colour

Use `Theme` tokens only:

| Token | Use |
|---|---|
| `primaryColor` / `highlightColor` | Main text / pressed or current text, titles on the watch page, links, accents |
| `secondaryColor` / `secondaryHighlightColor` | Meta lines / pressed meta lines, owner line, chapter times |
| `highlightBackgroundColor` via `Theme.rgba(…, 0.1)` | Thumbnail and avatar placeholders |
| `errorColor` | Errors; the LIVE badge background |
| `lightPrimaryColor` | Anything drawn on the picture or on a scrim |
| `overlayBackgroundColor` | Mini-player picture background |
| `opacityLow` | Unavailable rows |

The documented exemptions (each marked `// theme-exempt:`; `check_colours` enforces this):

| Literal | Where |
|---|---|
| `"black"` | Letterbox, cutout strip and picture background (VideoPage), details thumbnail |
| `rgba(0,0,0,0.7)` | Controls gradient scrim, ResumeOffer |
| `rgba(0,0,0,0.75)` | Thumbnail badges |
| `rgba(1,1,1,0.35)` | Watch progress track |

### 7.4 Ambiences

Dark and light ambiences are both supported (`Theme.colorScheme`). Content on the page background
uses theme tokens, so it flips with the ambience. Content on the picture or a thumbnail uses
light tokens on dark scrims and stays the same in both. Rule: use `lightPrimaryColor` only over
a scrim or the picture. Never hard-code dark or light.

Light-ambience audit (design component, 2026-11-08, static): every `lightPrimaryColor` and every
`theme-exempt` literal sits over the picture, a thumbnail or a scrim (watch-page box, overlay
controls, badges, resume offer, cover picture); everything on the page background uses theme
tokens. One miss fixed: the slow-load spinner and Retry on the black picture box took the
ambience's (dark, in a light ambience) colours; they now use `lightPrimaryColor`. Rule: any
control inside the picture box sets `color: Theme.lightPrimaryColor` (Buttons, BusyIndicator,
Labels).

Second pass (2026-11-10), after the player rebuild:

- The watch page's picture box is a `SilicaItem` with `palette.colorScheme: Theme.LightOnDark`,
  so every Silica control inside it (the autoplay Switch, Buttons, the busy indicator) draws
  light-on-dark in every ambience without each one being told. The explicit light colours stay.
- A highlight drawn on the picture uses
  `Theme.highlightFromColor(Theme.highlightColor, Theme.LightOnDark)`, never the plain highlight
  colour, which is dark in a light ambience: the seek bar's played part and thumb, a pressed
  end-screen tile. Found here: the seek bar was nearly invisible in a light ambience.
- Left as it is: the watched line on thumbnails (`WatchOverlay`, `ShelfTile`) uses the plain
  highlight colour on a white 35 % track; it is on the device list for a light ambience.

**Gaps:** nothing checks a light ambience automatically; the device check (screenshots of the
main page, a list, the watch page loading/playing/failed, the mini-player, settings and the cover
in a light ambience) is queued with the maintainer.

### 7.5 Typography

| Role | Size | Colour |
|---|---|---|
| Watch page title | `fontSizeMedium` | `highlightColor` (static text) |
| Card title (Home, Subscriptions, Search, related), shelf header, Library or settings row title | `fontSizeMedium` | `primaryColor` |
| Compact row title (channel, playlist, playlists, history, downloads; every list in landscape), owner name, description, channel "about", comment body | `fontSizeSmall` | `primaryColor` |
| Shelf tile title, chip label | `fontSizeExtraSmall` | `primaryColor` |
| Meta lines, row details, subscriber count | `fontSizeExtraSmall` | `secondaryColor` |
| Badges on pictures, bottom bar labels, stream line, end-screen tile titles | `fontSizeTiny` | `lightPrimaryColor` / theme |

One scale for the whole first minute: a title is Medium where it is the widest thing on its row (a
card, a header) and Small beside a thumbnail; everything secondary is ExtraSmall. Nothing uses
Large or above. Interactive text is `primaryColor` and takes the highlight colours while pressed
(titles `highlightColor`, meta `secondaryHighlightColor`); static text on the page is
`highlightColor`.

Single-line labels use `TruncationMode.Fade`. Multi-line labels wrap with `maximumLineCount` and
`elide`.

### 7.6 Spacing

| Value | Use |
|---|---|
| `horizontalPageMargin` | Left and right edge of every row, thumbnail and label |
| `paddingSmall` | Badge insets, gaps between lines in a text column |
| `paddingMedium` | Thumbnail-to-text gap, row top and bottom padding, button spacing |
| `paddingLarge` | Column top padding, list end spacer, icon-to-text gap in hub rows |
| `itemSizeExtraSmall … itemSizeLarge` | Row heights: inline rows, then suggestions, quality and tabs, then bottom bar, hub, subscriptions and controls bar, then playlists, downloads and mini-player |

### 7.7 Touch targets

Rule: a tappable row is at least `Theme.itemSizeSmall` high. An icon control uses `IconButton`
with an icon of at least `iconSizeMedium`. Overlay controls follow PLY-17 (§3.2). No target may
sit in the top-left back-gesture corner.

### 7.8 Right-to-left

The root sets `LayoutMirroring.enabled` when `Qt.application.layoutDirection` is RightToLeft,
with `childrenInherit: true`. The direction comes from the catalog entry `QT_LAYOUT_DIRECTION`,
which is "RTL" in Arabic only; `tests/i18n/check.py` checks this. `SettingLink` mirrors its
arrow. `tst_rtl.qml` loads the Arabic catalog and checks the bottom bar and a video row.

Rule: lay out with anchors and positioners, because LayoutMirroring does not mirror an explicit
`x`. Mirror directional icons. A centred or symmetric `x: margin; width: parent.width − 2·x`
label is fine.

Media controls are the exception to mirroring, as everywhere: the player's transport (previous,
play, next) and its bottom row (time, seek bar, fullscreen) keep their left-to-right order
(`LayoutMirroring.enabled: false` on those two rows); the seek bar fills from the left, and so
does the mini-player's progress line (placed with `x`, on purpose). The top row of the overlay and
everything outside the picture mirror with the page, the mini-player's buttons included.

A strip that scrolls sideways starts at its leading end: a horizontal `ListView` does this by
itself; `ChipStrip` (a Flickable) sets `contentX` to its right end when mirrored, and its content
is at least as wide as the strip so a short row sits at the right.

Positions computed from another item's `x` break under mirroring just as an explicit `x` does
(found in the watch page's owner row: the name collapsed and Subscribe covered the avatar). The
lint cannot see arithmetic; `tst_rtlpath.qml` does: with the Arabic catalog it walks Home,
Subscriptions, Search, Library, the watch page with its settings sheet, a playlist, the
playlists, history, channels and settings pages, and fails on any visible text that collapsed
(width ≤ 1) or left the screen; it also checks the owner row's order, the chip strips' start and
the player's unmirrored rows.

**Gaps:** the Downloads section is not walked by `tst_rtlpath.qml` (the UI harness has no `downloads` mock);
the comments list and the Shorts pager are not walked either. Device check in Arabic pending.

### 7.9 Large fonts

Theme font sizes follow the system text size. Rule: rows that hold text take their height from
their content, as `VideoListItem.naturalHeight` does. A fixed height is allowed only where the
text fades on one line.

**Gaps:** the rows that had fixed heights now grow with their text (the Library hub, playlists,
downloads; the bottom bar fits its labels instead). What is left is the device look at the
largest text size (`docs/DEVICE_DESIGN.md`, K3): the chips, the settings sheet's rows and a
playlist's head.

### 7.10 Landscape

Every page allows all orientations except `VideoDetailsPage`, which allows Portrait only. The
watch page becomes immersive in landscape (PLY-13). Rule: every page works in landscape unless
this document records a reason. Keep pulleys at 4 or fewer visible items so they fit landscape.
See §4 Gaps for the row thumbnail width.

---

## 8. Motion and start-up

What moves today:

| Element | Motion |
|---|---|
| Page transitions | PageStack default; `PageStackAction.Immediate` for link pushes while `pageStack.busy` |
| Mini-player | DockedPanel's own show/hide |
| Overlay controls | `FadeAnimator` on opacity, default duration |
| Video box height | `NumberAnimation` 150 ms, off while immersive |
| Settings focus | Highlight for 1500 ms, not animated |
| Pulley, remorse, BusyIndicator, ViewPlaceholder | Silica built-ins |

Rules:

- Prefer Silica's built-in motion. Animate custom elements only on opacity or scale, with
  Animators (render thread), at 200 ms or less.
- No looping animation except BusyIndicator and the skeleton pulse (§5.1); both stop while
  hidden or in the background.
- Nothing animates while the app is in the background (NFR-PERF-06) or in list delegates during
  a fling (NFR-PERF-07: no frame over 50 ms; thumbnails decode with `sourceSize` and
  `asynchronous`; watched state updates incrementally, LIB-06).

### 8.1 Start-up experience

"The total user experience counts from the moment the app opens." The path from the tap on the
launcher icon to the first useful content is designed, measured and budgeted like playback.

| # | What the user sees | Budget on R2 (p50) | Timing point (`sailtube-timing startup <stage> <ms>`) |
|---|---|---|---|
| 1 | Launcher icon zooms into the app's window (lipstick); the cover appears | system | — |
| 2 | First Silica frame: bottom bar plus the landing list's cached rows, skeleton or placeholder | ≤ 1.5 s (NFR-PERF-01, p95 ≤ 2.5 s) | `app`, `translations`, `gst-probe`, `models`, `qml-loaded`, `shell`, `first-frame`; `skeleton` when the landing list has nothing cached |
| 3 | Cached feed rows with thumbnails | ≤ 2.0 s | `content` (first rows of the landing section) |
| 4 | Fresh rows replace or extend the cached ones without a jump in scroll position | network | catalog's refresh timing (docs/PERF_CATALOG.md) |
| 5 | First tap on a video: the watch page slides in at once with the row's title, channel and thumbnail | one frame | `sailtube-timing video details/playing/first-frame <ms>` after `window.openVideo` |

What happens before the first frame (and what may not):

- Allowed: the main page with only the landing section (`Shell.loadedSections`), the bottom bar,
  the landing view and its cached rows, the cover component.
- Deferred until `first-frame` (StartupTiming's `marked` signal; 1.5 s fallback): download
  notifications and the FULL keep-alive (`DownloadNotifier`, `DownloadKeepAlive`).
- Deferred until the first video: MPRIS (`MediaControls`, so Amber.Mpris and its D-Bus
  registration leave the start path), the display keep-on helper, and the player module's video
  surface (only the watch page and `components/MiniPlayerPicture.qml` import it; the mini-player
  loads its picture by `source`).
- Deferred until the first video: the mini-player (its file is not even compiled before;
  Qt 5.6 has no QML disk cache, so every file referenced on the first screen costs its compile
  on each cold start).
- Loaded by file name only when needed, so neither compiled nor created at start: the round
  avatar of a row (channel rows only), the skeleton rows (`SkeletonList` wraps `SkeletonRows`),
  the placeholder buttons (`PlaceholderActions` wraps `PlaceholderButtons`). Rule for new work:
  anything a first screen shows only in some states goes behind a `Loader` with `source`, not a
  `visible: false` instance (POCO run 5: the merged shell had doubled models → content).
- Other sections load asynchronously on their first visit and stay loaded.
- Never before the first frame: a network wait, a resolver or yt-dlp start, a GStreamer pipeline,
  a comment or related fetch.

Native start-up work (core's `docs/PERF_STARTUP.md`; asked through the maintainer):

- `main.cpp` builds every model, opens the library and the download database, runs the GStreamer
  probe and builds the catalog before `view->show()`. Requested: show the view first with the
  landing models, then open downloads, build the download runner and probe GStreamer after the
  first frame (the probe result can be cached from the previous run, keyed on the GStreamer
  version, because it only matters before the first player).
- Playback's pre-warm (RES-21) and speculative resolve (E12) start after `content`, so they do not
  compete with the first rows.

**Gaps**

- NFR-PERF-01 and NFR-PERF-07 have not been measured on R1 or R2 (§6 I2, T1). The maintainer
  runs the 10 cold starts when the phone is available.
- The native deferrals above are core's; until they land, `models` − `app` is the largest stage.

---

## 9. Definition of Done for UI work

A UI change is done when all of the following hold:

1. **Components.** It uses Silica and `Theme` (§1 to §7). Any new custom component is listed in
   this document.
2. **Lint.** `tests/qml_lint/run.sh` reports 0 problems. `lint.py` checks:
   - an SPDX header in every file;
   - ES5 only, and no `Qt.callLater` (PLT-05);
   - pulleys of 5 items or fewer, with a `pulley-dynamic` note on any Repeater (PLT-06);
   - network Images with `sourceSize` and `asynchronous`;
   - colour literals only with `theme-exempt`, and untranslated literals only with
     `i18n-exempt`;
   - a `//:` translator note on every `qsTr`;
   - no `Connections.enabled`;
   - every context-object handler names a signal in its C++ header;
   - no id that shadows a property;
   - pure logic libraries;
   - one control per setting key;
   - root LayoutMirroring;
   - every qml directory installed.
3. **Decisions.** Layout and state logic lives in `qml/logic/*.js`, with tests in
   `tests/decisions/` that run under Node and QJSEngine (NFR-TST-04).
4. **Headless UI tests.** `tests/qml_ui/run.sh` passes:
   - `tst_pages`: every page opens and closes in portrait and landscape, with touch targets;
   - `tst_shell`, `tst_watch`, `tst_settings`, `tst_errorlinks`, `tst_rtl`, `tst_timing`;
   - `tst_startup`: helpers deferred past the first frame and the first video; a list shows
     the placeholder, the skeleton or rows, never nothing;
   - `tst_contract`: the mocks match the C++ headers.

   A new page is added to `tst_pages`. A new context property is mocked in `Mocks.qml`.
5. **Translations.**
   - Every string goes through `qsTr`, with a `//:` note directly above it that says where the
     text appears and what each `%1` or `%n` means.
   - Plurals use `%n`. Sentences are not built by concatenation.
   - Run `tools/normalize_ts.sh`, then fill all **10 catalogs** completely: en, es, pt, fr, de,
     it, ru, ja, zh_CN and ar.
   - `tests/i18n/run.sh` must pass (`check.py`: catalogs current, nothing unfinished,
     placeholders kept, RTL entry). The packaged `.qm` check is in progress
     (`tests/i18n/packaged.sh`, I18N-03).
   The harness runs real Silica without a screen. It proves states and geometry; it cannot show
   what is drawn. These depend on real Silica rendering, modality or touch and are only known
   from a device: the surface of a panel and what shows through it (`PlayerSettings`,
   `MiniPlayer`); window dimming by a modal panel; stacking between siblings (`z`); shaders
   (`Avatar`) and render-thread animators (the controls' fade, the skeleton's pulse); a palette
   handed down by a `SilicaItem` (the picture box) and `Theme.highlightFromColor`; and every
   gesture that meets one of Silica's own (the back swipe against chip strips and shelves, the
   seek bar's drag, the pinch and swipes on the picture, a pulley above a flickable).
6. **Visual check.** Look at the change in dark and light ambiences, in portrait and landscape,
   at the largest text size, in Arabic (RTL), on a device with a cutout, with the mini-player
   both shown and hidden, and on both FULL and STORE.
7. **Device.** Run the §6 steps for the area on R1 and R2 (for example P8, P10, P11, P16, F4,
   I3, M6, T1). Record the evidence in the device notes, with screenshots free of personal data.

**Gaps**

- The lint accepts any `//:` within three lines, so a note written for another string passes.
  Examples: the SearchView hints "Search again to retry." and "Type a search above and press
  Enter.", the HistoryPage hint "History is paused.", and the "Top"/"Newest" reuse in the
  CommentsPage subtitle.
- `check_setting_keys` sees only `settingKey` bindings, so the duplicates listed in §6 pass.
- Right-to-left layout is checked along the first-minute path (`tst_rtlpath.qml`). Nothing
  checks a light ambience or the largest text size automatically; both are device steps.

---

## References

- Sailfish OS app UI overview (pulley menus, fewer than five options for landscape, attached
  pages, covers and cover actions, ambiences): https://docs.sailfishos.org/Develop/Apps/UI/
- Sailfish UI Definition of Done (Silica fonts, colours and layout values, no empty or unfinished
  views, error recovery, smooth transitions, translation hooks):
  https://docs.sailfishos.org/Develop/Apps/UI/Definition_of_Done/
- Silica QML reference (PullDownMenu, PushUpMenu, ListItem and RemorseItem, RemorsePopup,
  ViewPlaceholder, DockedPanel, CoverActionList, Theme):
  https://sailfishos.org/develop/docs/silica/
- Requirements: `SAILTUBE_REQUIREMENTS.md` (PLT-05/06, DSC-04/06/13–19, PLY-10–18/25, LIB-01–07,
  DL-04, MPR-02, SET-01–03, I18N-03, NFR-PERF-01/02/06/07). Status: `docs/CHECKLIST.md`.
