<!-- SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0 -->
# YouTube parity matrix

Status: v4, 2026-11-10 (for beta 3); rows for settings that are now read, upcoming videos and SponsorBlock corrected 2026-11-11. Owner: the design component. v4 re-checks
every partial and missing row against the code at 17bc00e with the design
batch on top (player API bound, cards, playlist and channel heads). Every status describes
that one tree. Input for the maintainer. Requirement ids are those of
`SAILTUBE_REQUIREMENTS.md` v1.0 and `docs/CHECKLIST.md`.

Clean room: the "Official apps" column describes the public, observable behaviour of the official
YouTube apps only. No official or GPL client source was read, and no brand assets are used.

## Legend

| Column | Values |
|---|---|
| Official apps | `A` Android, `i` iOS, `W` web (desktop or mobile site). `–` = absent. `(P)` = Premium only in most regions. |
| Checklist id(s) | Spec / checklist ids. `P-nn` = not in the spec; see [Proposed additions](#proposed-additions). |
| SailTube status | **done**: wired from the UI to the engine (device check may still be pending). **partial**: some layers exist, but the user cannot use the whole feature. **missing**: not built, or only a settings row or model with nothing using it. **out of scope**: a non-goal of the spec, or impossible on the platform. |
| Native rendering | The Silica pattern used, or proposed for missing rows. "Pulley" = PullDownMenu, "push-up" = PushUpMenu, "ContextMenu" = long-press menu on a ListItem. |
| Component | `core`, `playback`, `catalog`, `library`, `ux`, `design`. `account` is the checklist's account component, which has no session yet. |
| Tags | `ACCT` = needs a signed-in YouTube account. `FULL` = FULL (Chum/OpenRepos) only, because Harbour rules or the STORE sandbox forbid it (spec §1.5, E9). |

Wiring was checked in `Clients/Sailfish/qml` and `Clients/Sailfish/Presentation`. A model or
settings row that no page or engine uses counts as **missing** or **partial**, never as **done**.

## Home

| Feature | Official apps (A/i/W) | Checklist id(s) | SailTube status | Native Sailfish rendering | Component |
|---|---|---|---|---|---|
| Personalised Home feed | A i W | DSC-04, ACC-11 | **partial**: Home section and `recommendedModel` (`FEwhat_to_watch`) exist; signed out Home shows the public "Popular" list (DSC-04a, catalog) under a Continue watching shelf (design). The personal feed needs a signed-in account. `ACCT` | Bottom bar section, SilicaListView, pulley Refresh, ViewPlaceholder | catalog, account |
| Choice of landing page (Subscriptions, Recommended, Search) | – (always Home) | DSC-04, SET-01 | **done** (ux, design): Settings chooses Subscriptions, Recommended (Home) or Search; Home is honoured without an account, since it shows Popular; with the default, a user who has no subscriptions yet lands on Home instead of an empty feed | Settings ComboBox; MainPage loads only the landing section | catalog, ux |
| Useful Home without an account (continue watching, new from subscriptions, mixes from recent videos) | A i W (signed-out Home) | P-02 | **partial**: a Continue watching shelf from the local history tops Home (design, `components/ContinueShelf.qml`), above the Popular list (catalog); "new from subscriptions" lives in the Subscriptions section, and mixes are not built | Home section: a shelf in the list's header (DESIGN §4.9) | design, catalog |
| Topic chips (All, Music, Gaming…) | A i W | P-17 | **missing** | Horizontal ListView in the list header, or a ComboBox in the header | catalog |
| Infinite scroll, pull to refresh | A i W | DSC-06, DSC-09 | **done** | `onAtYEnd` paging, pulley Refresh, BusyIndicator in the footer | catalog |
| Row overflow menu (Watch later, add to playlist, download, subscribe, mark watched) | A i W | LIB-02, LIB-04, DL-04 | **done** (ux, design b695eec, b2ba1b0): `VideoMenuItems` on search, feed, Home, channel, related and playlist-preview rows; and the watch page's related rows | ListItem ContextMenu (`VideoMenuItems`) | ux |
| "Not interested" / "Don't recommend channel" | A i W | P-08 | **missing** (local channel block proposed) | ContextMenu item, then remorse | library |
| Watched progress bar and badges on thumbnails | A i W | LIB-06 | **done** | `WatchOverlay` + `DownloadBadge` inside `VideoListItem` | library |

| Bottom navigation bar (Home, Shorts, Subscriptions, Library, Search, Downloads) | A i (W: side menu) | DSC-04, user request 2026-11-08 | **done** (design b7229fc): six entries at the bottom of the window in the user's order, mini-player above, hidden on the watch page; labels when they fit | `components/NavigationBar.qml` on the docked-panel background; sections lazily loaded | design |

| Video cards: full-width thumbnail, avatar, title, "channel · views · age" | A i | DSC-04, DSC-05, DSC-08, DSC-19 | **done** (design 7e36840) on Home, Subscriptions and Search in portrait; the avatar shows the channel's initial until catalog's `channelAvatar` role lands, and the picture stays the 320 px list thumbnail until `thumbnailLarge` does | `VideoListItem.cardLayout` (DESIGN §4.1); compact rows in landscape | design, catalog |

## Search

| Feature | Official apps (A/i/W) | Checklist id(s) | SailTube status | Native Sailfish rendering | Component |
|---|---|---|---|---|---|
| Search with infinite scroll | A i W | DSC-08, DSC-09, DSC-10 | **done** | SearchField outside the list (the keyboard stays open), "More results" button and `onAtYEnd` | catalog |
| Filters: sort, upload date (incl. last hour), type, duration, live | A i W | DSC-08 | **done**: filter page, plus quick chips under the field for the result type and Live (design 1877906) | Chip row under the SearchField (`ActionChip`); Filter page with ComboBox rows | catalog |
| Feature filters (4K, HD, subtitles, Creative Commons, 360°, HDR) | A i W | P-16 | **missing** | Extra ComboBox / TextSwitch rows on the filter page | catalog |
| Mixed results (channels, playlists) | A i W | DSC-08 | **done** | Same list; channel rows open ChannelPage, playlist rows open PlaylistPreviewPage | catalog |
| Suggestions while typing | A i W | DSC-11 | **done** | Suggestion SilicaListView with a clock or magnifier icon per row; replaces the results while the field has focus | catalog |
| Search history: recall, remove entry, clear, pause | A i W | DSC-11 | **done** | History entries in the suggestion list, ContextMenu "Remove from history"; switches on the filter page | catalog |
| Hashtag search from a description | A i W (hashtag pages) | DSC-14 | **done**: a tap runs a search for `#tag` and opens the Search section with the query in the field (`tst_search.qml`); the official hashtag page (its own feed) is not rebuilt | Description link, then the Search section | catalog, ux |
| Search within a channel | A i W | DSC-12 (MAY) | **missing** | SearchField in the ChannelPage header, or a pulley item | catalog |
| Voice search | A i W | – | **out of scope** (no system speech-to-text API on SFOS) | – | – |

## Shorts

| Feature | Official apps (A/i/W) | Checklist id(s) | SailTube status | Native Sailfish rendering | Component |
|---|---|---|---|---|---|
| Shorts detection and badge (one rule everywhere) | A i W | DSC-17 | **done**: "Short" badge on channel and search rows (design 685ba04); counted when hidden | "Short" badge text in channel rows; counted when hidden | catalog |
| Hide Shorts | – | DSC-17, SET-01 | **done** (catalog, ux): the Settings switch (default on) hides Shorts in search, channel lists, the Subscriptions feed, Home and related videos (`ContentFilters`); the per-list switches still work. Device check pending | Settings TextSwitch; channel pulley; filter page switch | catalog, ux |
| Shorts open upright and fullscreen | A i W | PLY-13, PLY-14 | **done** (device check P10) | Watch page; `WatchLayout` picks portrait fullscreen for a vertical video | ux |
| Vertical swipe viewer | A i W | DSC-17 (MAY, parity) | **partial** (beta 3): the Shorts entry of the bottom bar opens a vertical pager over catalog's `shortsModel`. The Short on screen plays by itself in place, looping, with the next one preloaded; a tap pauses; leaving the tab, a pushed page or a locked screen pause it. A right rail has like (count only; the app cannot like), comments (the page), save and share, and the bottom left has the channel, Subscribe, the description and a related video. Missing: comments as a sheet over the Short (2c), double tap to like, hold for 2x, an overflow menu, a scrubbing progress line. Device check pending | Bottom-bar Shorts section: full-screen vertical `SilicaListView` (`SnapOneItem`), title and channel on a bottom scrim, row menu on long press | ux, playback |
| Shorts tab on the channel page | A i W | DSC-12 | **done** | ChannelPage tab row | catalog |
| Create or remix Shorts | A i | §1.2 non-goal | **out of scope** | – | – |

## Channel pages and tabs

| Feature | Official apps (A/i/W) | Checklist id(s) | SailTube status | Native Sailfish rendering | Component |
|---|---|---|---|---|---|
| Header: avatar, subscribers, video count | A i W (plus banner) | DSC-12, DSC-19 | **done** (no banner): round avatar (design b695eec) | Column in the list header | catalog |
| Tabs: Videos, Shorts, Live, Playlists; Releases when there is no Videos tab | A i W | DSC-12 | **done** | Row of BackgroundItem tabs in the header; pulley Refresh / Hide Shorts / Hide members-only | catalog |
| `@handle`, `/c/`, `/user/` resolution | A i W | DSC-12 | **done** | – (LinkRouter, then ChannelPage) | catalog |
| Subscribe button on the channel page | A i W `ACCT` | LIB-01 | **done** (design ab843a8): the watch page's Subscribe / Subscribed button in the channel header, local, remorse before unsubscribing | Button in the header, as on the watch page | library, ux |
| Sort videos: Latest / Popular / Oldest | A i W | P-07 | **missing** | ComboBox in the header, or a pulley item | catalog |
| About tab (description, links, joined date) | A i W | DSC-12 (MAY) | **partial**: the description shows on a card in the channel head, two lines until tapped (design); links and the joined date need data from catalog | Expandable card in the header (DESIGN §4.8) | catalog, design |
| Posts (community) tab | A i W | P-14 | **missing** | Extra tab; read-only text and image rows | catalog |
| Members-only / restricted rows badged, or hidden with a count | A i W (badge) | DSC-18 | **done** | Badge text; "%n hidden by filters" footer | catalog |
| Notification bell | A i W `ACCT` | – | **out of scope** (account plus push); local variant P-05 | – | – |
| Channel memberships (Join) | A i W `ACCT` | – | **out of scope** (payments) | – | – |

## Playlists

| Feature | Official apps (A/i/W) | Checklist id(s) | SailTube status | Native Sailfish rendering | Component |
|---|---|---|---|---|---|
| Open a YouTube playlist (up to 5000 items, continuations) | A i W | LIB-03 | **done** | PlaylistPreviewPage; pulley "Open channel", "Play all" | catalog |
| Save a YouTube playlist to the library | A i W `ACCT` | LIB-03 | **done**: PlaylistPreviewPage pulley "Save to library" / "Open in library" (65325e8) | Pulley "Save to library" on PlaylistPreviewPage | library |
| Local playlists: create, rename, delete, reorder | A i W `ACCT` | LIB-02 | **done** (no account needed) | PlaylistsPage pulley "New playlist"; ContextMenu Rename / Move up / Delete with remorse; Dialog for the name | library |
| Watch later | A i W `ACCT` | LIB-02 | **done** (local) | Row ContextMenu toggle; Library row; option in the add-to-playlist dialog | library |
| Play all / shuffle | A i W | LIB-03, PLY-25 | **done** (design): a Play all button and a Shuffle chip in the playlist head; `window.playQueue` hands the whole list to `playback.setQueue`. A YouTube playlist shuffles the rows loaded so far. Device check pending | `components/PlaylistHeader.qml` under the PageHeader (DESIGN §4.8) | design, playback |
| Loop a playlist | A i W | P-11 | **missing** | Queue page switch | playback |
| Download all | A i (P) | DL-08 | **done** | PlaylistPage pulley | library |
| Liked videos | A i W `ACCT` | ACC-11 | **done** `ACCT` (catalog, design): signed in, the Library tab has a "Liked videos" row that opens the account's list (sent signed). Device check with an account pending | Library row backed by an account mirror | account |
| Share a playlist or channel link | A i W | P-10 | **done** (design): Share and Copy link chips in the playlist head (YouTube playlists and saved copies), Share in the channel pulley; `logic/ShareLinks.js` builds the address from the id | Chips in the playlist head; pulley "Share" on the channel, through `Sailfish.Share` as on the watch page | design |
| Collaborative playlists, privacy settings | A i W `ACCT` | ACC-11 (MAY writes) | **out of scope** (account writes) | – | – |

## Watch page

| Feature | Official apps (A/i/W) | Checklist id(s) | SailTube status | Native Sailfish rendering | Component |
|---|---|---|---|---|---|
| Title, views, likes, upload date, subscribers | A i W | DSC-14 | **done** | Labels under the video, filled when watch-next answers (never delays the first frame) | catalog |
| Description: 5 lines, links, seeking timestamps | A i W | DSC-14 | **done** | StyledText Label, "Show more" BackgroundItem | catalog |
| Chapters: list with jump, current chapter | A i W | DSC-15 | **done** | Inline row "n chapters · now: …", then pushed ChaptersPage | catalog |
| Chapter ticks and chapter name on the seek bar | A i W | PLY-17, DSC-15 | **done** (design): gaps in the track at chapter starts and the chapter name in the drag bubble, from `playback.chapters` | `components/SeekBar.qml` (DESIGN §3.2.1) | design |
| Comments: Top/Newest, pinned, likes, reply pages on demand | A i W | DSC-16 | **done** (device check C1) | Inline CommentsSection on the info flickable; tap to retry; per-thread "More replies" | catalog |
| Post, like or reply to comments | A i W `ACCT` | §1.2 non-goal | **out of scope** | – | – |
| Related videos (60+) | A i W | DSC-13 | **done** | First 5 inline, then RelatedPage; long-press opens VideoDetailsPage | catalog |
| Details-only view that leaves playback alone | – | DSC-14 | **done** | VideoDetailsPage (long-press on a related row) | catalog |
| Like / dislike action | A i W `ACCT` | ACC-11 (MAY) | **out of scope** in v1 (account writes; spec §7 Q4) | – | account |
| Dislike count | – (hidden since 2021) | RYD-01 | **done** (catalog, design): with "Show dislike estimates" on in Settings, the watch page shows the community's estimate as a chip beside the like count, marked "≈" (`watchInfo.dislikeCount` / `dislikeText`). Off by default; nothing is requested while it is off. Device check pending | A passive chip in the watch page's chip row | catalog, ux |
| Share through the system sheet | A i W | DSC-14 | **done** | Action chip in the watch page's chip row (DESIGN §3.4) | catalog |
| Copy link, optionally at the current time | A i W | DSC-14 | **done** | Action chip in the watch page's chip row (DESIGN §3.4) + TextSwitch "Link at the current time" | catalog |
| Open in the browser | W | DSC-14 | **done** | Action chip in the watch page's chip row (DESIGN §3.4) | catalog |
| Save to a playlist / Watch later | A i W `ACCT` | LIB-02 | **done** (local) | Pulley "Add to playlist", then AddToPlaylistDialog | library |
| Download with a quality picker, watch-page indicator | A i (P) | DL-01, DL-04 | **done** | Pulley "Download", then DownloadQualityDialog; "Downloading n %" label | library |
| Subscribe | A i W `ACCT` | LIB-01 | **done** (local, no account) | Button in the owner row; tapping the row opens ChannelPage | library |
| Live streams | A i W | RES-19, DSC-07 | **partial**: HLS live plays and shows "LIVE"; an upcoming video says it has not started yet, with Retry, and is not resolved again before its start (its scheduled start shows once the extractor has named the case); speed and loop are off at the live edge; no DVR seek-back or go-to-live control | LIVE label in place of the duration | playback |
| Premieres / upcoming: countdown, reminder | A i W (`ACCT` for reminders) | RES-19, P-20 | **missing** | Placeholder with the start time; local Nemo notification at start | catalog |
| Live chat (read only) | A i W | P-13 | **missing** | Attached page (`pushAttached`) beside the watch page | catalog |
| Transcript | A i W | P-12 | **missing** | Pushed page listing caption cues; a tap seeks | catalog, ux |
| Clips, Thanks, Remix, Report, Join | A i W `ACCT` | – | **out of scope** | – | – |

## Player

| Feature | Official apps (A/i/W) | Checklist id(s) | SailTube status | Native Sailfish rendering | Component |
|---|---|---|---|---|---|
| Overlay controls: play/pause, seek slider, time, auto-hide | A i W | PLY-17, PLY-24 | **done** (design, player v3): full scrim; centre play/pause disc with previous/next and a buffering spinner; top row minimise / fullscreen title / autoplay / CC / settings; bottom row time (tap for remaining), seek bar, fullscreen; auto-hide 3.5 s | `components/PlayerOverlay.qml` (DESIGN §3.2) | design, playback |
| Double-tap seek ±5/10/15 s with a running total | A i W (mobile) | PLY-17, SET-01 | **done**: OverlayController tap stacking; a ripple on the tapped side with the 10 s icon and the running total | Ripple Item with `icon-m-10s-back`/`forward` | playback, design |
| Seek preview thumbnails (storyboards) | A i W | P-04 | **partial** (playback, design): the seek bar's bubble shows time and chapter name, and a storyboard frame in the cell's own shape when `playback.scrubPreview(ms)` returns one. The session has a storyboard only for videos resolved through the fallback (yt-dlp); the fast path does not parse them yet, so most videos show the time-only bubble. Not seen on a device | `components/SeekBar.qml` bubble | playback, design |
| Pinch to fill the screen in fullscreen | A i | P-18 | **done** (design): pinch in fullscreen switches fill (crop) and fit | PinchArea over the picture; `VideoSurfaceItem.fillMode` | design |
| Long press for 2× while held | A i | P-23 | **done** (design): long press on the picture, a "2× ▸▸" pill; `playback.holdSpeed` when it lands, otherwise rate 2 and back | MouseArea pressAndHold; pill at the top | design, playback |
| Swipe up for fullscreen, down to leave or minimise | A i | P-24 | **done** (design): vertical swipes on the picture; horizontal drags stay Silica's back swipe (scrub-by-drag dropped, DESIGN §3.2.2) | `PlayerUi.swipeAction` | design |
| Seek bar with buffered range, chapters, SponsorBlock colours | A i W | PLY-17, DSC-15, SB-02 | **done** (design, playback): chapter gaps and names while scrubbing, segment colours, growing thumb, time bubble | `components/SeekBar.qml` | design, playback |
| Touch lock in fullscreen | A | P-19 | **partial** (design): built and tested ("Lock screen" in the settings sheet in fullscreen; touches only show Unlock, Back is off; ends with fullscreen, on an error and at the end), but switched off (`PlayerUi.TOUCH_LOCK_RELEASED`) until its device check passes | Settings sheet row; an Unlock pill at the bottom centre (DESIGN §3.2.6) | design |
| Quality menu (Auto plus height and fps rungs) | A i W | SEL-03 | **done** | Watch-page pulley "Quality", then QualityPage | playback |
| Separate Wi-Fi and mobile-data ceilings | A i | SEL-02, SET-01 | **done** (playback): the engine applies the Wi-Fi row on unmetered networks and the mobile-data row (`playback/maxHeightMobile`) on metered ones. Device check pending | Settings ComboBoxes | playback |
| Playback speed 0.25–2×, pitch-corrected, remembered | A i W | PLY-07 | **done** (UI, design): settings sheet speed list 0.25–2× in 0.25 steps, "Normal" at 1×, on `playback.setRate` (remembered by playback); long press plays at 2× while held | Settings sheet (`PlayerSettings.qml`); long-press pill | playback, design |
| Captions: track choice, translations, overlay | A i W | SEL-06 | **done** except translations (playback, catalog, design): opening a video lists its caption tracks; CC turns one on or off through `playback.setCaptionTrack` and remembers it; the text is drawn over the picture on the session's own cue clock; the Captions row names the track and opens CaptionsPage. A chosen track that cannot be loaded is said in the caption box. Automatic translations are refused by the provider and not listed. Device check pending | Caption box over the picture; CC badge; CaptionsPage | catalog, design |
| Caption style (size, background) | A i W | SEL-06, SET-01 | **missing** (Settings link shows "Not available yet") | Settings sub-page with Sliders and a live preview | ux |
| Audio track: original, dubbed, descriptive | A i W | SEL-04, SEL-05 | **done** (playback, design): the settings sheet lists `playback.audioTracks` when a video has more than one and `setAudioTrack` plays the chosen one; the row names the playing track. Device check pending | Settings sheet choice list | playback, design |
| Audio only (listen mode) | – | SEL-08 | **done** | Player settings sheet switch (moved from the pulley); Settings default | playback |
| Background play (screen off, app in the background) | A i (P), W (desktop) | PLY-09, PLT-12, PLY-23, SET-01 | **partial**: audio keeps playing with the video decode frozen (PLY-09) and the audio role and resource in place (PLT-12, PLY-23); with the "Background playback" switch off the player pauses when the app leaves the foreground and stays paused on return. Device check pending | – (process keeps running; Amber.Mpris) | playback |
| Lock-screen and headset controls | A i (P) | MPR-01, MPR-03, PLT-10 | **done** (device checks M1, M2) | Amber.Mpris `MprisPlayer`; next/previous off without a queue | playback |
| Cover: art, title, progress; play/pause, next or stop | – (Android media notification) | MPR-02 | **done**: one `cover/CoverPage.qml` on `playback.coverMode` (progress, next or stop, idle identity, "Continue watching"); `tst_cover.qml` | CoverBackground + CoverActionList | playback (data), design (presentation) |
| Picture-in-picture | A i (P), W | user-approved addition (2026-11-08), MPR-02 | **done** in FULL (STORE: off): live video on the active cover is SailTube's PiP, approved by the user (Silica has no OS PiP surface). Engine: playback's `CoverVideoItem` (same frames, at most 15 fps, only while the cover is visible); layout and the "Video on the cover" switch in Settings; rules in DESIGN.md §7.1. Device check pending | Active cover: live frames in the picture area, thumbnail fallback, text below the picture | playback, ux, design |
| In-app mini-player | A i W | PLY-11, PLY-10 | **done** (device check P8) | DockedPanel `MiniPlayer`: live frame or thumbnail, progress, play/pause, close; a tap re-attaches the watch page | ux |
| Sleep timer | A i | P-01 | **done** (playback, design): Sleep timer in the settings sheet (Off, 15 to 60 min, end of video) through `playback.setSleepTimer`; the row shows the time left. Device check pending | Settings sheet choice list; time left as the row's value | playback, design |
| Autoplay related, with a cancellable countdown | A i W | PLY-25, SET-01 | **done**: "Up next" card with the next picture and title, countdown and Cancel; the autoplay switch in the player's top row | `components/PlayerEndScreen.qml`; top-row Switch | playback, design |
| Queue: play next, add to queue, view and edit | A i W | PLY-25 (panel MAY), LNK-04, P-09 | **partial** (playback component): `setQueue`, `next()`, `previous()`, MPRIS and cover next exist, and playlists fill the queue through `window.playQueue` (design); no row entry ("Play next", "Add to queue"), no queue page | Row ContextMenu "Play next" / "Add to queue"; queue as an attached page of the watch page | playback, ux |
| Loop video | A i W | P-11 | **done** (playback, design): Loop video switch in the settings sheet (`playback.setLoop`), not on live streams. Device check pending | Settings sheet switch | playback, design |
| Fullscreen and orientation (both landscape sides, works with rotation lock, restores auto-rotate) | A i W | PLY-13, PLY-14, PLY-15, PLY-16 | **done** (device checks P10, P11, P16) | Fullscreen button at the bottom right; `allowedOrientations` from `WatchLayout.js`; cutout insets; edge glow hidden when immersive | ux |
| Keep the display on during video | A i | PLY-12, PLT-11 | **done** in FULL; `FULL` (Harbour refuses `Nemo.KeepAlive`, E9) | DisplayBlanking loaded only when not STORE | playback |
| Resume where you left off (Always / Ask / Never) | A i W `ACCT` (sync) | LIB-05 | **done** (local) | `ResumeOffer` over the video; Settings ComboBox | library |
| Error recovery with Retry and a link to the setting | A i W (Retry) | PLY-18, SET-02 | **done**: RecoveryPolicy, Retry, "Open setting", "Play video" for listen-only, and "Open in browser" (also in fullscreen) | Error column on the picture | playback, design |
| Stable volume / loudness normalisation | A i W | PLY-22 | **done** (playback, design): Stable volume switch in the settings sheet (`playback.setStableVolume`), shown only when the session says the playing audio has a levelled and a plain track (`stableVolumeAvailable`). Device check pending | Settings sheet switch | playback, design |
| Equalizer and volume boost | – | PLY-22 | **partial**: Volume boost feeds the audio chain live and with every load; the Equalizer row is hidden in beta 3 (no page, no stored preset key; the engine side has six presets ready). Device check pending | Settings sub-page with Sliders per band | playback |
| SponsorBlock skips | – | SB-01, SB-02 | **done** (playback, ux, design): segments fetched and skipped with a notice and a "Skip %1" button; the seek bar draws them in their category colours; Settings has a master switch and Off/Skip/Show button per category on the table's keys | 1.6 s notice over the video; segments on the Slider; per-category Settings page | playback |
| Ambient mode, cards, end screens | A i W | – | **partial**: end screen with Replay and three suggested videos (design); ambient mode and creator cards stay out of scope | `PlayerEndScreen.qml` | design |
| Ads | A i W | §1.2 non-goal | **out of scope** (never shown) | – | – |
| Casting | A i W | §1.2 non-goal | **out of scope** (post-parity MAY) | – | – |

## Library

| Feature | Official apps (A/i/W) | Checklist id(s) | SailTube status | Native Sailfish rendering | Component |
|---|---|---|---|---|---|
| Library hub | A i W | LIB-* | **done** (design 378cb9f): History and Playlists shelves, then one row per collection | Library section: horizontal shelves of `ShelfTile`s in the list header; ListItem rows (Watch later, Channels, Downloads, Import and export, and Playlists/History when their shelf is empty) | ux, library |
| Watch history with resume points and watched marks | A i W `ACCT` | LIB-04, LIB-06 | **done** (local) | HistoryPage; ContextMenu mark/unmark, add to playlist, remove with remorse | library |
| Pause history, clear history | A i W | LIB-04 | **done** | HistoryPage pulley; clear through RemorsePopup (resume points kept) | library |
| Search within history | A i W | P-15 | **missing** | SearchField in the HistoryPage header | library |
| Private session (no history, no search history) | A i (incognito) | P-15 | **missing** (pause switches exist separately) | One TextSwitch in the Library settings plus an indicator in the bottom bar | library |
| Downloads page: pause, resume, cancel, retry, delete | A i (P) | DL-02, DL-04 | **done** | DownloadsPage ContextMenu, delete with remorse | library |
| Download quality, folder, Wi-Fi only | A i (P) | DL-01, DL-06, SET-01 | **partial**: the folder and Wi-Fi-only rows work (device check W3 for the SD card) and each download asks for its quality; the Settings "Download quality" row decides what the dialog starts as | Settings rows; DownloadQualityDialog; folder sub-page | library |
| Downloads without ffmpeg (remux on the device) | – | DL-03 | **done** (device check W2, E6) | – | library |
| Offline playback in the same player, with resume | A i (P) | PLY-26, DL-07 | **done** (playback component): DownloadsPage calls `playback.openLocal`; saved captions show over the picture and saved segments on the seek bar | DownloadsPage tap opens the watch page in local mode | playback |
| Downloads keep running in the background | A i (P) | DL-09 | **done** in FULL; `FULL` (STORE says downloads run only while SailTube is open) | KeepAlive background job loaded only in FULL | library |
| Download notifications | A i | DL-04 | **done** | Nemo.Notifications (progress, done, folder notice) | library |
| Automatic downloads of new uploads from chosen channels | A i (P) (smart downloads) | P-21 | **missing** | Per-channel TextSwitch; runs while the app is open (FULL: also in the background) | library |
| Import: NewPipe/PipePipe, Takeout CSV, OPML; SailTube backup and restore | W (Takeout export only) | IMP-01, IMP-03, IMP-04 | **done** | ImportExportPage, file picker | library |
| Import subscriptions and playlists from the account | – | IMP-02 | **done** `ACCT` (library component, 2a29519, dc08c28): Import page "From the YouTube account" | Import page entry when signed in | library (account rows) |

## Subscriptions feed

| Feature | Official apps (A/i/W) | Checklist id(s) | SailTube status | Native Sailfish rendering | Component |
|---|---|---|---|---|---|
| Subscribe without an account; list sorted A–Z or by latest upload | – (needs an account) | LIB-01, DSC-19 | **done** | SubscriptionsPage pulley sort; ContextMenu Unsubscribe with remorse; avatars | library |
| Feed of subscribed channels (RSS first, schedule, live pinned, 300+ items) | A i W `ACCT` | DSC-05, DSC-06, DSC-07 | **done**: `CatalogLibraryBridge` passes the subscriptions to the feed (65325e8) | Subscriptions section, pulley Refresh, small BusyIndicator while due channels refresh | catalog, library |
| Hide watched in the feed and on channel pages | – | LIB-07 | **done** (65325e8); the feed shows no "n hidden" footer yet (the channel page does) | Hidden rows with a count footer, as in search | catalog |
| Feed filters (Today, Videos, Shorts, Live, Unwatched) and a channel avatar strip | A i W | P-03 | **missing** | Horizontal avatar ListView in the header; filter ComboBox or pulley | catalog, ux |
| Row actions in the feed | A i W | LIB-02, DL-04 | **done** (ux 48616c3): `VideoMenuItems` on feed and Home rows | ContextMenu `VideoMenuItems` | ux |

## Notifications

| Feature | Official apps (A/i/W) | Checklist id(s) | SailTube status | Native Sailfish rendering | Component |
|---|---|---|---|---|---|
| Push notifications (uploads, replies, mentions) and inbox | A i W `ACCT` | – | **out of scope** (account plus Google push; DSC-06 allows no wake-ups while closed) | – | – |
| Local "new upload" notifications for chosen channels while SailTube runs | – | P-05 | **missing** | Nemo.Notifications from the feed refresh; per-channel switch (bell icon) on ChannelPage | catalog |
| Download notifications | A i | DL-04 | **done** | Nemo.Notifications | library |

## Share and deep links

| Feature | Official apps (A/i/W) | Checklist id(s) | SailTube status | Native Sailfish rendering | Component |
|---|---|---|---|---|---|
| Open youtube.com, www, m., youtu.be and youtube-nocookie links in the app | A i | LNK-01, LNK-03, PLT-13 | **done** (device checks L1, L2; STORE needs `ExecDBus` or the E10 variant, because Harbour cannot ship the D-Bus service file) | `x-url-handler/<host>` MIME types, X-Maemo D-Bus keys, `LinkRouter` (route, then raise) | catalog |
| `t=` / `start=` start time | A i W | LNK-02, LIB-05 | **done**: `libraryPlayback.setLinkStart` before `open()` (65325e8); the playback component also honours the `startSeconds` hint | – | playback, library |
| Video with `list=` plays inside that playlist | A i W | LNK-04 | **done**: a `list=` link opens the video, then the playlist (up to 500 ids, `queueSeeder`) becomes the queue from the linked video. Device check pending | – | playback |
| Playlist and channel links | A i W | LNK-04 | **done** | PlaylistPreviewPage / ChannelPage pushed by the router | catalog |
| `/clip/` links | A i W | LNK-02 (MAY) | **partial**: recognised, then routed as unsupported | – | catalog |
| music.youtube.com hand-off to SailTune | – | LNK-01 | **partial** (intentional): recognised and not registered; the subpackage path is open | – | catalog |
| Share video (sheet), copy link, link at time | A i W | DSC-14 | **done** | See Watch page | catalog |
| Receive "share to SailTube" from other apps | A i (OS share targets) | P-06 | **missing** (links already open through the URL handler) | Share-method registration in the desktop entry (SFOS 4.2+), routed through `LinkRouter`; Harbour acceptance to check | core, catalog |

## Account and sign-in

| Feature | Official apps (A/i/W) | Checklist id(s) | SailTube status | Native Sailfish rendering | Component |
|---|---|---|---|---|---|
| Everything works signed out | – (most features need an account) | ACC-01 | **done**: the account code is optional (`Core/Account`), and `tests/account_service` covers the signed-out behaviour | – | account |
| Sign-in: import from the Sailfish Browser cookie jar | A i W | ACC-02, ACC-03 | **done** `FULL` (library component, dc08c28): AccountPage; Settings links to it | Account page in Settings; button with a "sign in in a normal tab" hint | library (account rows) |
| Sign-in: paste or file import (Cookie header, cURL, cookies.txt) | – | ACC-02, ACC-04 | **done** `ACCT`: the paste dialog and the file picker parse a Cookie header, copied headers, a cURL command or cookies.txt with SailTune's pinned parser; storing follows the Secret storage row, so STORE still refuses. Device check pending | TextArea Dialog; `Sailfish.Pickers` file picker | library (account rows) |
| Sign-in: embedded WebView | A i W (native sign-in) | ACC-05 (MAY), E1 | **missing** (after E1; needs the `WebView` permission) | Sailfish.WebView page | account |
| OAuth device flow | A i W (TV) | ACC-02 | **out of scope** (the spec says do not build it) | – | – |
| Session check, "expired" banner, re-import | A i W | ACC-07, ACC-08 | **done** `ACCT` (library component): 15 min checks, expired state and "Import again" on the Account page; no main-page banner | Banner row on the main page; one-tap re-import | library (account rows) |
| Several logins and brand channels | A i W | ACC-09 | **done** `ACCT` (library component): Account page "Channels" list | ComboBox on the Account page | library (account rows) |
| Secret storage | – | ACC-10 | **partial** `ACCT` (library component): FULL 0600 file with a one-time disclosure; STORE refuses until a vault adapter is pinned | – | library (account rows) |
| Signed-in content: Recommended, Watch later and Liked from the account, age-restricted playback | A i W | ACC-11 | **done** `ACCT` (catalog, account, design): signed in, Home shows the account's recommendations, the Library tab has "YouTube Watch later" and "Liked videos" rows, and the extractor gets the session cookies. Not checked on a device with an account | Existing sections and Library rows | account |
| Account writes: subscribe, like, edit playlists | A i W | ACC-11 (MAY) | **out of scope** for v1 `ACCT` (explicit "sync to account" opt-in if ever) | – | account |
| Premium-only formats, Kids, supervised accounts | A i W | §1.2 non-goal | **out of scope** | – | – |

## Settings

| Feature | Official apps (A/i/W) | Checklist id(s) | SailTube status | Native Sailfish rendering | Component |
|---|---|---|---|---|---|
| One settings page, one control per key, shared defaults table | A i W | SET-01, SET-02, SET-03 | **done** | SettingsPage: SectionHeaders, TextSwitch, ComboBox, Slider, SettingLink to sub-pages; opened from the pulleys | ux |
| Error messages link to the exact setting | – | SET-02 | **done** | "Open setting" button, then `window.openSetting(key)` | ux |
| Settings rows whose engine was not built (autoplay, double-tap step, boost, loudness, caption style, SponsorBlock) | A i W | SET-01 | **partial**: autoplay, the double-tap step, SponsorBlock, volume boost, loudness normalisation, Background playback, the mobile-data ceiling and Download quality are read now; Caption style shows "Not available yet"; the Equalizer and Resolver updates rows are hidden in beta 3 because nothing reads them | – | playback, ux |
| Settings rows not connected to the catalog models: Hide Shorts, comments auto-load (`comments/autoLoad` vs `catalog/comments/autoLoad`), RYD (`integrations/ryd` vs `catalog/ryd/enabled`) | – | SET-02, DSC-16, DSC-17, RYD-01 | **done** (ux c09b875): rows write `dislikes.enabled`, `commentsModel.autoLoadMode`; Hide Shorts reaches search and channel lists. Hide Shorts reaches the feed and Home too | Same rows; align the keys in the shared table | ux, catalog |
| Library settings (resume, history size, hide watched) | A i W | LIB-04, LIB-05, LIB-07 | **done** | "Library and downloads" sub-page | library |
| Video capabilities page and diagnostics report | – | DIA-01, DIA-02, DIA-03 | **partial**: the capabilities page and the timing-log switch exist; the counters and the report builder exist in Core with tests, but no page builds, shows or copies a report | CapabilitiesPage from Settings | core |
| Content region and language override | A i W | P-22 | **missing** (hl/gl follow the device locale) | Settings ComboBox | catalog |
| Restricted mode | A i W | – | **out of scope** (the spec has none; the members-only and restricted hide filters cover lists) | – | – |

## Accessibility and languages

| Feature | Official apps (A/i/W) | Checklist id(s) | SailTube status | Native Sailfish rendering | Component |
|---|---|---|---|---|---|
| UI translations (10 locales) with right-to-left Arabic | A i W (many locales) | I18N-03 | **done** (device checks I3 and Arabic) | `qsTr` with notes; `LayoutMirroring` from `QT_LAYOUT_DIRECTION` | ux |
| Content titles and dates in the device language (hl/gl) | A i W | I18N-01, I18N-02, I18N-04 | **done** | – | catalog |
| Locale-aware counts and dates | A i W | I18N-02 | **done** | `Format.count(n, Qt.locale())` | ux, catalog |
| Large fonts, theme colours, light and dark ambiences | A i W | PLT-06 | **done** (device large-font pass pending) | Theme sizes and colours only (lint) | ux |
| Screen reader | A i W | – | **out of scope** (SFOS has no system screen reader) | – | – |
| Captions and audio-description tracks as accessibility aids | A i W | SEL-04, SEL-06 | **partial** (see Player) | – | playback, catalog |
| Keyboard shortcuts | W | – | **out of scope** (phone UI) | – | – |

## Start-up and perceived speed

The official apps open on content they already have and never show an empty screen while
loading; SailTube is measured from the launcher tap (docs/DESIGN.md §8.1).

| Feature | Official apps (A/i/W) | Checklist id(s) | SailTube status | Native Sailfish rendering | Component |
|---|---|---|---|---|---|
| Opens on cached content, refreshes behind it | A i W | NFR-PERF-01, DSC-06 | **done** (device check I2): only the landing section is built before the first frame; the feed paints from its cache | Main page, landing section | design, catalog |
| Loading placeholders instead of a blank page | A i W (shimmer rows) | NFR-PERF-01 | **done** (design 728bff7): `SkeletonList` in feed, Home, search and channel lists; never both a skeleton and "No videos" | Placeholder rows in the list header plus the pulley's busy line | design |
| Heavy work after the first frame | – | NFR-PERF-01 | **partial**: QML helpers are deferred; GStreamer init and the probe run on a worker during start-up (still joined before the first frame); decoder preload and the warm resolver start after it; the downloads manager still opens before the first frame | – | design, core |
| Tap to first frame within about a second | A i W | NFR-PERF-02, RES-11, RES-21 | **partial** (pre-warm and speculative resolve are in; whether it meets a second is a device measurement, NFR-PERF-02); the watch page appears at once with the row's thumbnail and title | Watch page pushed immediately | playback |
| Smooth scrolling with thumbnails | A i W | NFR-PERF-07 | **done** (device check T1): display-size asynchronous decode, no layers in delegates, landscape rows keep their size | – | design |
| Buffering indicator on the picture | A i W | PLY-08 | **done**: Large BusyIndicator while loading or buffering | BusyIndicator centred on the picture | playback, design |

## Proposed additions

Not covered by the spec. Priority is for parity per unit of effort.

| Id | Feature | Rationale | Component | Priority |
|---|---|---|---|---|
| P-02 | Useful anonymous Home (continue watching, new from subscriptions, mixes from recent videos) | Signed out, Home is an empty "sign in" page; every input already exists locally | catalog | high |
| P-03 | Feed filters (Today, Videos, Shorts, Live, Unwatched) and a channel avatar strip | The official subscriptions tab offers both; the data is already in the feed rows | catalog, ux | medium |
| P-04 | Seek preview thumbnails from storyboards | Makes scrubbing usable on long videos; the storyboard spec is in the player answer | playback, catalog | medium |
| P-05 | Local "new upload" notifications for chosen channels (while running) | Bell analogue without an account; respects DSC-06's no-wake-up rule | catalog | medium |
| P-06 | Receive "share to SailTube" from other apps | Links shared from the browser or a messenger should open in SailTube like URL taps do | core, catalog | medium |
| P-07 | Channel video sort (Latest, Popular, Oldest) | Standard channel control; one browse parameter per tab | catalog | medium |
| P-08 | Block or hide a channel locally | Replaces "Don't recommend channel" without an account; applies to Home, search and related | library | medium |
| P-09 | "Play next" / "Add to queue" in every row menu | Makes the PLY-25 queue reachable from lists, as in the official apps | playback, ux | medium |
| P-10 | Share a playlist or channel link | Only videos can be shared today | catalog | low |
| P-11 | Loop a playlist (loop video is done) | Frequent for music and tutorials; trivial once the queue exists | playback | low |
| P-12 | Transcript view (tap a line to seek) | Reuses the caption cues; helps accessibility and long videos | catalog, ux | low |
| P-13 | Read-only live chat (live and replay) | Live viewing loses context without it; read-only avoids account writes | catalog | low |
| P-14 | Channel Posts (community) tab | Visible on most channels in the official apps; read-only | catalog | low |
| P-15 | History search and a private-session switch | Official equivalents exist; both are small and local | library | low |
| P-16 | Search feature filters (4K, HD, subtitles, Creative Commons, 360°, HDR) | Same public `sp` encoder; extends DSC-08 | catalog | low |
| P-17 | Topic chips on Home | Needs an account for personal chips; anonymous chips only with P-02 | catalog | low |
| P-19 | Touch lock in fullscreen | Prevents pocket and child taps during playback | ux | low |
| P-20 | Premiere / upcoming start reminder | Local notification at the scheduled start, no account needed | catalog | low |
| P-21 | Automatic downloads of new uploads from chosen channels on Wi-Fi | Smart-downloads analogue for commuters; FULL in the background, STORE only while open | library | low |
| P-22 | Content region and language override | The official apps let users pick a location; useful for travellers and learners | catalog | low |
| P-26 | Channel Home tab (featured video, shelves) and banner | First thing the official channel page shows (A i W) | catalog | medium |
| P-27 | Sort a playlist or Watch later; "Remove watched videos" | Standard list management (A i W), all local | library | medium |
| P-28 | "Most replayed" graph above the seek bar | Helps find the useful part of long videos (A i W) | playback, catalog | low |
| P-29 | Precise seeking (frame strip while scrubbing) | Needs storyboards (P-04) | playback | low |
| P-30 | Stats for nerds overlay | Diagnostics for a power-user audience; reuses DIA counters | playback, core | low |
| P-31 | Translate a comment | Needs a translation service; privacy review (NFR-SEC-01) first | catalog | low |
| P-32 | Muted inline preview while scrolling | Costly on phones (decoder, data); only on Wi-Fi and FULL, if ever | playback | low |
| P-33 | Break and bedtime reminders, time watched | Digital-wellbeing parity (A i); local only | library | low |
| P-34 | Explore destinations (Music, Gaming, News, Sports, Live) | Anonymous discovery without an account | catalog | low |

## Checklist ids with UI impact that no row cites yet

PLY-08 (buffering indicator), PLY-19 / PLT-07 (picture survives a GL context loss), PLY-20 / PLY-21
(hardware-decode setting and its restart note), SEL-01 (codec preference), SEL-07 (quality and
listen-only switches keep the position), RES-09 (error classes as user texts), RES-14 / RES-04 /
NFR-MNT-01 / NFR-SEC-04 (PO-token provider and extractor controls), DSC-01 / NFR-ROB-01 (degrade
instead of break), DL-05 (file names), NFR-PERF-01/02/07/08/09 (cold start, tap to first frame,
feed scroll, comments and search timing), RES-11 / RES-21
(prefetch and pre-warm). The next matrix version gives each a row.
