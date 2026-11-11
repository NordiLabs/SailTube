<!-- SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0 -->
# Design component device checklist (beta 3)

Status: v4, 2026-11-10 (settings sheet fixed after the owner's first finding; 2026-11-11: a swipe that closes the sheet no longer pops the watch page, covered by A8). Owner: the design component. One sitting on the POCO X3 Pro (Sailfish OS 5.2,
FULL build). It checks what the headless tests cannot: touch against Silica's own gestures, what
is legible on a real picture, motion, and memory. The spec's steps (I1, P1, …) stay in
`docs/DEVICE_CHECKLIST.md`; this file does not repeat them, it names them where they are run
together.

Nothing in this file has been run yet: everything the design component built after beta 1 is checked
here for the first time.

How to run: go down the list once.

- **Section A, the first ten minutes: what can block a release.** If a step here fails, stop and
  report; the rest can wait for the fix.
- Sections B to I: the rest of the first-minute path, in dark ambience, English, portrait unless
  a step says otherwise (about 25 min).
- Sections J and K change the ambience, the language or the text size (about 15 min).
- Section L is long and can run apart.

Record each step as pass, fail (what was seen) or skipped, in a dated note in `docs/`.

## Before starting

- FULL build installed over the previous beta, data kept. Wi-Fi. Dark ambience. System language
  English. Rotation unlocked.
- In the app: three or more subscriptions; one local playlist with three or more videos; one
  YouTube playlist saved to the library; a few videos in the history.
- Have at hand: a long video with chapters and SponsorBlock segments (a sponsored talk or
  review, with captions); a video with dubbed audio tracks; a downloaded video saved with captions; an upcoming live event; a Short; a
  live stream; a channel with a long description.
- Settings → Playback: "Autoplay next video" on, "Video on the cover" on.
- The touch lock (step D2) is hidden in the tree: its release switch is
  `TOUCH_LOCK_RELEASED` in `Clients/Sailfish/qml/logic/PlayerUi.js`. For this sitting set it to
  `true`, either in the build or on the phone in
  `/usr/share/harbour-sailtube/qml/logic/PlayerUi.js` (then restart the app). It stays `true`
  in the tree only if D2 passes.

## A. First ten minutes: release blockers

| Id | Do | Expect | Tests behind it |
|---|---|---|---|
| A1 | Close the app, start it. | Never a blank page: rows from the last run, or the skeleton, within about a second (spec step I2). With subscriptions it opens on Subscriptions. | `tst_startup.qml`, `tests/startup` |
| A2 | Tap each of the six bottom entries; end on Home. | Home, Shorts, Subscriptions, Library, Search, Downloads open; the current one is marked. | `tst_shell.qml`, `tst_shell.js` |
| A3 | Tap a card on Home (the long video). | The watch page is there at once with the card's picture, a spinner and a status line; then the video **plays**, with sound (spec step P1). | `tst_watch.qml`, `tst_startup.qml` |
| A4 | Tap the picture; tap the centre disc twice. | Controls show; pause, then play. | `tst_player.qml` |
| A5 | **Seek:** drag the seek bar's thumb across and release. Double-tap the right side of the picture twice. | One seek on release and playback goes on from there; the drag never turns into a back swipe; the double tap jumps forward and shows the total. | `tst_gestures.qml`, `tst_playerui.js` |
| A6 | **Back swipe against the chips:** below the player, drag the chip row left, then back to its start; from the start, swipe right across the chips. Then swipe from the left screen edge. | The row scrolls sideways; at its start a right swipe goes back to the list; the edge swipe always goes back. Open the video again. | `tst_watchinfo.qml` |
| A7 | **Fullscreen in and out:** tap the fullscreen button; tap it again. Swipe up on the picture; swipe down. Turn the phone sideways and back upright. | Each way enters fullscreen in landscape and leaves it to the upright page; the orientation is never stuck; the swipes do not pull the pulley or the top edge. | `tst_gestures.qml`, `tst_watchlayout.js` |
| A8 | **The sheet:** tap the gear. Look at it over the video and over the text below, in portrait and in fullscreen. Open Quality, pick another one. Open the gear again, set speed 1.5×. Open it once more and swipe Back; open it again and tap outside it; open it again and tap the minimise arrow. | A sheet from the bottom with a solid surface: nothing behind it shows through, and every row reads clearly; the rest of the screen is dimmed. The choice applies and the sheet closes. Back or a tap outside closes the sheet and leaves the page where it is; the dimming goes away with the sheet every time, and never stays on the screen after leaving the page. | `tst_sheet.qml`, `tst_player.qml` |
| A9 | Tap the minimise arrow (top left). Tap the mini-player's body. | The arrow takes the tap without starting a back swipe; the mini-player sits above the bottom bar with the moving picture; the tap returns to the watch page without loading again (spec step P8). | `tst_watch.qml`, `tst_shell.js` |
| A10 | **Back swipe against the shelves:** open Library; drag the History shelf sideways and back. Open the local playlist; drag its chips; from their start swipe right. | Shelves and chips scroll; on the playlist page the right swipe from the chips' start goes back. | `tst_library.qml`, `tst_playlisthead.qml` |

## B. Home and Subscriptions

| Id | Do | Expect | Tests behind it |
|---|---|---|---|
| B1 | Close and start the app twice more. | As A1 each time; rows replace the skeleton without a jump. Journal: `sailtube-timing startup` lines, `skeleton` or `content` after `first-frame`. | `tst_startup.qml` |
| B2 | Home: look at three cards, then fling the list hard, twice (spec step T1). Tap the current bottom entry. | Card: full-width sharp picture, round avatar or the channel's initial, title on two lines, one meta line. The fling stays smooth; no picture flashes broken while a larger thumbnail falls back to the smaller one. The second tap on the entry scrolls to the top. | `tst_cards.qml`, `tst_rows.js` |
| B3 | Rotate to landscape on Home, then back. | Compact rows in landscape (thumbnail left), cards again in portrait; the bar stays at the bottom. | `tst_rows.js` |
| B4 | Open Subscriptions; pull down to refresh. | Cards as on Home; the pulley shows it is busy; a channel picture that arrives late replaces the initial without the row moving. | `tst_cards.qml`, `tst_states.qml` |

## C. Watch page, windowed

Open the long video again.

| Id | Do | Expect | Tests behind it |
|---|---|---|---|
| C1 | Look at the controls while it plays and while it buffers (seek far ahead). | Centre disc clearly larger than previous/next; previous and next show only with a queue; while buffering the spinner sits where the disc was. | `tst_player.qml`, `tst_playerui.js` |
| C2 | Flip the autoplay switch in the top row. | The switch is legible on the picture and Settings → Playback → "Autoplay next video" follows it. | `tst_player.qml` |
| C3 | Look at the seek bar while it plays; drag its thumb slowly. | A lighter part ahead of the played part shows what is buffered; the thumb grows; a bubble shows the time and the chapter's name (and a small frame of the video above them on the few videos that have a storyboard; most show none); gaps mark chapter starts; sponsor segments are coloured. | `tst_gestures.qml`, `tst_playerapi.qml`, `tst_playerui.js` |
| C4 | Double-tap the left side three times. Hold a finger on the picture, release. Swipe down on the picture; open the video again. | A ripple on the tapped side with the running total; "2×" while held, at double speed with the voice's pitch kept, and the speed from before after; the swipe down leaves to the mini-player. | `tst_gestures.qml` |
| C5 | In fullscreen (landscape), open the gear. Set Sleep timer to 15 minutes, open the gear again, set it to Off. Turn Loop video on near the end of a short video, then off. | The sheet is at most three quarters high and scrolls; each row shows its value; the Sleep timer row counts down and reads Off again; with Loop on the video starts again at its end instead of the end screen. | `tst_player.qml`, `tst_playerapi.qml` |
| C6 | Open a streamed video that has captions. Tap CC twice; choose another track from the sheet's Captions row. Then play a downloaded video saved with captions. | CC shows in the top row; captions sit above the controls, white on a dark box, in step with the speech; CC turns them off and on with the same track. A video without captions has no CC button. If a track's text cannot be loaded, the box says "This track could not be loaded" for a few seconds. | `tst_playerapi.qml`, `tst_playerui.js` |
| C7 | Scroll below the player. Tap Share, then Copy link. Tap the description, then again. Tap the comments card. Open "More related videos". | Title, views and age; owner row with avatar, name, subscriber count and a compact Subscribe; with "Show dislike estimates" on in Settings, a "≈" dislike chip beside the likes; the share sheet opens; "Link copied" shows; the description opens and closes in place; comments open on their page; related videos are cards on both pages, with a skeleton while the page loads. | `tst_watchinfo.qml` |
| C8 | Tap a hashtag in a description. | The Search section opens with the hashtag in the field and its results. | `tst_search.qml` |
| C9 | Open a video that has dubbed audio (a large channel's recent upload). Gear → Audio track → another language. Then flip Stable volume. | The sheet lists the tracks with the original marked; the sound changes language while the picture goes on; the row names the new track; Stable volume switches without a restart. A video with one audio track shows no Audio track row. | `tst_playerapi.qml` |

## D. Fullscreen

| Id | Do | Expect | Tests behind it |
|---|---|---|---|
| D1 | In fullscreen: look at the top row; pinch out, pinch in. | The title and channel in the top row, clear of the camera cutout; pinch fills and fits the picture. | `tst_gestures.qml`, `tst_watchlayout.js` |
| D2 | With the release switch on (see "Before starting"). In fullscreen: gear → Lock screen. Tap, double-tap, swipe, pinch, swipe from the left edge, turn the phone upright. Tap Unlock. Lock again and let the video end (seek near the end first). | While locked nothing reacts except an Unlock pill at the bottom for 3 s; the picture stays in landscape; Unlock brings the controls back; the end of the video unlocks by itself. **Pass: the switch stays on for beta 3. Fail or not run: it stays off and the row does not ship.** | `tst_lock.qml`, `tst_playerui.js` |
| D3 | Let a video end with Autoplay on; tap Cancel. Turn Autoplay off and let one end. | A card "Up next in N s" with the next video and Cancel; without autoplay, Replay in the centre and three suggestions along the bottom; a suggestion plays in place. | `tst_endscreen.qml` |
| D4 | Airplane mode on, open a video that is not downloaded. Airplane mode off, Retry. Then open an upcoming live event (a premiere or a scheduled stream). | The error text and its buttons (Retry, Open in browser) are readable on the dimmed thumbnail; Retry plays. The upcoming event says it has not started and when it is scheduled, in plain light text, not in the error colour. While any video loads, the spinner and status line read clearly on the dimmed thumbnail. | `tst_errorlinks.qml`, `tst_states.qml`, `tst_watchlayout.js` |

## E. Back to the lists

| Id | Do | Expect | Tests behind it |
|---|---|---|---|
| E1 | With the mini-player showing: pause and play from it; look at its progress line; close it. | Play/pause and close work from the bar; the thin line follows the video. | `tst_watch.qml`, `tst_shell.js` |
| E2 | Leave a video half watched, open Home. Tap its tile. Watch it to the end, open Home again. | A "Continue watching" shelf tops Home with that video first and its watched line; the tile resumes where it was left; once finished it is gone from the shelf; the list under the shelf does not jump while scrolling. | `tst_home.qml`, `tst_rows.js` |

## F. Search

| Id | Do | Expect | Tests behind it |
|---|---|---|---|
| F1 | Type three letters; pick a suggestion. Tap Videos, Channels, Live, All. Open Filters, set one, come back. | Suggestions carry a clock or a magnifier; each chip applies at once and shows as chosen; the Filters chip is marked and names the filter. | `tst_search.qml` |
| F2 | Search a channel's name. On its row tap Subscribe, then Subscribed. | The channel row has a round picture and a compact Subscribe; unsubscribing waits behind a remorse timer. | `tst_library.qml`, `tst_cards.qml` |

## G. Library, playlists, channel

| Id | Do | Expect | Tests behind it |
|---|---|---|---|
| G1 | Library: tap a tile of each shelf; tap a shelf's header. | Tiles open the video or the playlist; headers open the full page; no row repeats what a shelf shows. | `tst_library.qml` |
| G2 | Open the local playlist. Tap Play all; tap Next twice, also from the lock screen's media controls. Go back, tap Shuffle, twice. | A head with the first video's picture, Play all and a Shuffle chip, no Share. Play all plays in order and Next moves through the list; Shuffle starts on another video each time. | `tst_playlisthead.qml`, `tst_shell.js` |
| G3 | Open the saved YouTube playlist: Share, Copy link. From Search open a playlist result: Play all, Shuffle. | Share opens the share sheet with a `youtube.com/playlist` address; the search result shows the skeleton, then the same head. | `tst_playlisthead.qml`, `tst_sharelinks.js` |
| G4 | Open the channel with the long description. Tap the description, twice. Pull down: Share. | The description opens and closes in place, the list below moves with it without a jump; a wide Subscribe; Share opens the share sheet with the channel's address. | `tst_sharelinks.js` |
| G5 | Open Playlists, History and Downloads. | Their rows look like the video rows elsewhere: same thumbnail size, same title and meta sizes. | `tst_pages.qml` |

## H. Shorts

| Id | Do | Expect | Tests behind it |
|---|---|---|---|
| H1 | Open Shorts. | The first Short plays by itself, full screen, upright, with sound, looping at its end; no controls over it and no watch page. A thin line along the bottom shows the progress. The bottom bar stays and the mini-player does not show. | `tst_shorts.qml` |
| H2 | Swipe up three times, then down once. Do it fast. | Each swipe moves one Short and the next starts at once; a still picture with a spinner may show for a moment, never a black screen or the previous Short's frame on the new one. The Short left behind is silent. | `tst_shorts.qml`, `tst_shortsui.js` |
| H3 | Tap the picture; tap again. Pause it, go to another tab and come back. | The first tap pauses (a play mark shows) and the second resumes. A Short you paused is still paused when you come back to the tab. | `tst_shorts.qml` |
| H4 | Play a Short, go to another tab and back; lock the screen and unlock; open another app and come back. Look at the cover while it is locked or away. Then long press a Short. | Leaving the tab, locking and switching app pause it; coming back resumes the same Short at the same place. There is no mini-player on any tab. The cover shows the paused Short and its Play resumes it. The long press opens the row menu. | `tst_shorts.qml` |
| H5 | Open a Short from the long press menu's details or any link that pushes a page, then go Back. | The Short is paused while the page is open and plays again on return. | `tst_shorts.qml` |
| H6 | In airplane mode, swipe to a Short that has not been preloaded. | The poster with the reason and a Retry button; swiping on still works. | `tst_shorts.qml` |
| H7 | Look at a Short with a bright picture, then a dark one: the right rail and the bottom left block. | Four round buttons on the right (like, comments, save, share) with counts under them, and the channel line, the description and the related chip at the bottom left, all readable on both; Subscribe is light, not dark, in a light ambience. | `tst_shorts.qml` |
| H8 | Tap like. Tap comments, go Back. Tap save, go Back. Tap share. | Like shows "Liking videos is not available yet" and nothing else. Comments open the comments page (the Short pauses) and Back resumes it. Save opens the playlist dialog. Share opens the system share sheet with a `youtube.com/shorts/` address. | `tst_shorts.qml`, `tst_sharelinks.js` |
| H9 | Tap the channel's name, then Back. Tap Subscribe, then Subscribed. Tap the description twice. Tap the related chip, then Back. | The channel page opens and Back returns to the same Short, resumed. Subscribe works as everywhere and asks before unsubscribing. The description opens and closes in place. The related video plays on the normal watch page. | `tst_shorts.qml` |
| H10 | Swipe to a popular Short and watch it for a second; look at the rail and the bottom left. Use a Short whose creator turned comments off, if you find one. | The four buttons are there at once; the counts, the channel, the description and the chip fill in without moving anything. With comments off the comments button is dimmed and does nothing. | `tst_shorts.qml` |

## I. Cover

| Id | Do | Expect | Tests behind it |
|---|---|---|---|
| I1 | While a video plays, go to the home screen. Then a Short. Then turn Settings → Playback → "Video on the cover" off. | The cover shows the moving picture with the text under it, never over it; a Short takes an upright shape; with the setting off the cover shows the still picture (spec step M6). | `tst_cover.qml` |

## J. Light ambience

Switch to a light ambience.

| Id | Do | Expect | Tests behind it |
|---|---|---|---|
| J1 | Look at Home, the skeleton (clear the cache or use a new search), Library and Settings. | Text is dark on light everywhere on the page; pictures keep their light badges; the bottom bar is readable. | static audit, DESIGN §7.4 |
| J2 | Open a video. Look at the seek bar, the autoplay switch, the gear, the "2×" pill, an error box. | Everything on the picture is light and clearly visible: the played part of the seek bar and its thumb, the switch, the icons, the buttons. | static audit, DESIGN §7.4 |
| J3 | Look at the watched line on thumbnails (a row and a shelf tile) and the mini-player. | The watched line is readable on the picture; the mini-player's text and icons are dark on its light bar. | static audit |

Switch back to the dark ambience.

## K. Arabic and the largest font

| Id | Do | Expect | Tests behind it |
|---|---|---|---|
| K1 | System language Arabic, restart the app. Look at the bar, a card, Search's chips, Library's shelves. | Home is the rightmost entry; avatars sit on the right of their text; chip rows and shelves start at the right. | `tst_rtl.qml`, `tst_rtlpath.qml` |
| K2 | Open a video. Look at the owner row, the chips, the player's controls and the mini-player. | Avatar right, Subscribe left, the name between them; previous, play and next keep their left-to-right order, the time is left of the seek bar, and the seek bar and the mini-player's line fill from the left. | `tst_rtlpath.qml` |
| K3 | Back to English. System text size at its largest, restart. Look at the bar, a card, the chips, the settings sheet, a playlist's head. | The bar keeps all six labels, shrunk to fit and still readable; rows grow instead of cutting text; chips and sheet rows stay one line and tappable. | `tst_a11y.qml`, `tst_shell.js` |

Set the text size back.

## First run (once, on a clean install or with the app's data cleared)

| Id | Do | Expect | Tests behind it |
|---|---|---|---|
| M1 | Start the app with no subscriptions. Subscribe to a channel, close the app, start it again. | The first start opens on Home with the Popular list (skeleton, then cards), not on an empty Subscriptions feed; after subscribing, the next start opens on Subscriptions. | `tst_shell.js`, `tst_shell.qml` |

## L. Long checks (can run apart)

| Id | Do | Expect | Tests behind it |
|---|---|---|---|
| L1 | With the app's counters: cover visible, app in front, display off. | `coverFramesDrawn` grows by about 15 a second only while the cover is visible; `coverDecodeSeconds` grows only then; the `cover-video on` and `off` markers appear. | `tst_cover.qml` |
| L2 | Ten minutes at the home screen with a video on the cover, then ten with audio only. | Battery and CPU of the two runs, for the record. | none |
| L3 | Scroll Home for a minute, then read the app's memory. | Within the feed budget (NFR-PERF-05, 180 MB); by calculation a full-width picture is 2.6 MB decoded at 1080 px. | none |

## What a failure means

| Step fails | Likely owner |
|---|---|
| A1, B1, B2 (speed, blank page), L3 | design with core and catalog |
| A3, A5 (the seek itself), C1 spinner, D3 countdown, D4, A9 and E1 picture, I1, L1, L2 | playback (engine), design for how it is drawn |
| C3 sponsor colours, C6 captions, C7 comments, C8, F1, G3 loading | catalog for data, design for how it is drawn |
| C3 buffered range, C4 hold speed, C5 sleep and loop, C9 audio track and stable volume | playback for the behaviour, design for the rows |
| E2, G1, G2, G5 | library for data, design for how it is drawn |
| Everything else | design |
