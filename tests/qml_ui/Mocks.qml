// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
import QtQuick 2.6

// Mock back ends with the same QML surface as the C++ context objects (PlaybackSession,
// SearchModel, AppSettings). Signals that the C++ objects declare explicitly (detailsChanged,
// streamChanged, playerChanged) are declared here too; tests/qml_lint checks that QML only
// connects to signals the C++ headers declare.
QtObject {
    id: mocks

    property bool resolverAvailable: true
    property bool javascriptRuntimeAvailable: false
    property string appVersion: "0.0.0-test"
    // Hardware decoding is off by default (SET-01): the probe demotes droidvdec, so the codecs
    // report software decoding while the element is still present.
    property var deviceCapabilities: ({ loaded: true, version: "1.20", h264: "software", vp9: "software",
                                        av1: "none", hevc: "none", vp8: "software", aac: true, opus: true,
                                        hls: true, adaptive: true, playbin3: true, playbinSwitchable: true,
                                        hardwareDecoderDemoted: true, elements: ["droidvdec"], sinkCaps: ({}) })

    property QtObject playback: QtObject {
        property string videoId: ""
        property string title: ""
        property string channel: ""
        property string channelId: ""
        property string description: ""
        property string thumbnail: ""
        property real viewCount: -1
        property real likeCount: -1
        property string uploadDate: ""
        property bool live: false
        property bool detailsLoaded: false
        property var chapters: []
        property var qualities: []
        property int requestedHeight: 0
        property string streamLabel: ""
        property bool audioOnly: false
        property bool hasVideo: false
        property int state: 0
        property string statusText: ""
        property string errorText: ""
        property bool playing: false
        property bool buffering: false
        property real position: 0
        property real duration: 0
        property bool seekable: false
        // Optional contract asked of the playback component.
        property string errorSettingKey: ""
        property var calls: []
        signal detailsChanged()
        signal streamChanged()
        signal playerChanged()

        function log(name) { var c = calls.slice(0); c.push(name); calls = c }
        // PLY-26: diagnostics.local is true while a downloaded file plays.
        property var diagnostics: ({ local: false })
        // The hints of the last open() (a Short opens with loop: true).
        property var _lastHints: ({})
        function open(id, hints) {
            log("open:" + id)
            _lastHints = hints || {}
            shortActive = false
            if (hints && hints.preferStream === true) log("preferStream:" + hints.startSeconds)
            videoId = id; title = (hints && hints.title) || ("Video " + id); channel = "Channel"
            detailsLoaded = false; state = 1; detailsChanged()
        }
        function finishLoading(withVideo) {
            detailsLoaded = true; description = "Line one\nLine two"; hasVideo = withVideo !== false
            state = 3; playing = true; duration = 120000; seekable = true
            detailsChanged(); streamChanged(); playerChanged()
        }
        function fail(message) { state = 4; errorText = message; playing = false; playerChanged() }
        function play() { log("play"); playing = true; playerChanged() }
        function pause() { log("pause"); playing = false; playerChanged() }
        // A tap is the user's own pause: a hold and a release keep it.
        function togglePlayback() { if (playing) pause(); else play(); _playingBeforeHold = playing }
        // Player session API of playback batch 2 (PlaybackSession.h).
        property bool canPlay: videoId !== ""
        property bool canPause: videoId !== ""
        property bool canSeek: seekable
        property bool canGoNext: false
        property bool canGoPrevious: false
        property QtObject mediaObject: null
        property int coverMode: videoId !== "" ? 2 : 0
        property int coverRightAction: 0
        property bool keepDisplayOn: playing && hasVideo
        property string sponsorNotice: ""
        property string sponsorButton: ""
        property string upNextVideoId: ""
        property int upNextSecondsLeft: 0
        signal ended()
        signal replacePageRequested()
        signal repaintRequested()
        property int _contexts: 0
        function claimContext() { _contexts += 1; return _contexts }
        function closeContext(id) { }
        function activateContext(id) { }
        // The last value the page passed (PLY-24: fine position updates only while the controls show).
        property bool _controlsVisible: false
        function setControlsVisible(on) { _controlsVisible = on }
        function surfaceFrameReady() { }
        function setSponsorSegments(list) { }
        function setRelatedCandidate(id) { }
        function skipSponsor() { log("skipSponsor") }
        function cancelUpNext() { upNextVideoId = "" }
        function continueWatching() { log("continue") }
        // Optional queue API (playback component); empty by default.
        property int queueLength: 0
        property int queueIndex: -1
        function next() { log("next") }
        function previous() { log("previous") }
        function setQueue(ids, index) { log("setQueue:" + ids.join(",") + "@" + index); queueLength = ids.length; queueIndex = index }
        // Shorts pager (PlaybackSession.h "Shorts"): the playing item is a Short, opened by the pager,
        // held while the pager's section is covered; the user's own pause survives a hold.
        property bool shortActive: false
        property bool shortHeld: false
        property bool retryAvailable: true
        property var _holds: []
        property bool _playingBeforeHold: false
        function openShort(id, hints) {
            log("openShort:" + id)
            _lastHints = hints || {}
            videoId = id; title = (hints && hints.title) || ("Video " + id); channel = "Channel"
            detailsLoaded = false; state = 1; shortActive = true; _holds = []; shortHeld = false
            detailsChanged()
        }
        function shortsAhead(ids) { log("ahead:" + ids.join(",")) }
        function holdShort(reason) {
            log("hold:" + reason)
            if (_holds.indexOf(reason) >= 0) return
            if (_holds.length === 0) { _playingBeforeHold = playing; if (playing) { playing = false; playerChanged() } }
            _holds = _holds.concat([reason]); shortHeld = true
        }
        function releaseShort(reason) {
            log("release:" + reason)
            var i = _holds.indexOf(reason)
            if (i < 0) return
            var rest = _holds.slice(0); rest.splice(i, 1); _holds = rest
            if (rest.length === 0) { shortHeld = false; if (_playingBeforeHold) { playing = true; playerChanged() } }
        }
        // Watch overlay API (PlaybackSession.h, "Watch overlay").
        property real bufferedPosition: 0
        property var sponsorSegments: []
        // [{ id, label, language, original, descriptive, selected }]; empty without a separate audio stream.
        property var audioTracks: []
        function setAudioTrack(id) {
            log("audioTrack:" + id)
            var l = []
            for (var i = 0; i < audioTracks.length; ++i) {
                var t = audioTracks[i]
                l.push({ id: t.id, label: t.label, language: t.language, original: t.original, descriptive: t.descriptive, selected: t.id === id })
            }
            audioTracks = l
        }
        property bool stableVolume: false
        // True only when the playing audio has both a levelled and a plain track.
        property bool stableVolumeAvailable: false
        function setStableVolume(on) { log("stableVolume:" + on); stableVolume = on }
        property bool loop: false
        function setLoop(on) { log("loop:" + on); loop = on }
        // -1 off; seconds left while it runs; sleepTimerAtEnd: it pauses at the end of the video.
        property int sleepTimerSecondsLeft: -1
        property bool sleepTimerAtEnd: false
        function setSleepTimer(minutes) {
            log("sleep:" + minutes)
            sleepTimerAtEnd = minutes < 0
            sleepTimerSecondsLeft = minutes > 0 ? minutes * 60 : (minutes < 0 ? 300 : -1)
        }
        // [{ id, label, language, autoGenerated, translated }]; captionTrackId "" is off.
        property var captionTracks: []
        property string captionTrackId: ""
        property string captionText: ""
        function setCaptionTrack(id) {
            log("caption:" + id)
            var known = id === ""
            for (var i = 0; i < captionTracks.length; ++i) if (captionTracks[i].id === id) known = true
            if (known) captionTrackId = id
        }
        // Hold to speed: 2x while held, the rate from before after.
        property real _heldRate: 1.0
        function holdSpeed(held) {
            log("hold:" + held)
            if (held) { _heldRate = rate; rate = 2.0 } else rate = _heldRate
            playerChanged()
        }
        // The storyboard cell at a position ({ url, x, y, width, height }; empty without one).
        property var _scrubCell: ({})
        function scrubPreview(ms) { return _scrubCell }
        function scrubPreviewUrl(ms) { return _scrubCell.url || "" }
        // RES-19: a live event that has not started yet (the failed state says when it is scheduled).
        property bool upcoming: false
        // PLY-26: a downloaded file plays and can be streamed instead.
        property bool canStreamInstead: false
        function streamInstead() { log("streamInstead") }
        // PLY-07: playback speed, remembered (SpeedPolicy 0.25-2.0).
        property real rate: 1.0
        property bool rateChangeable: true
        function setRate(r) { log("rate:" + r); rate = r; playerChanged() }
        function stop() {
            log("stop"); playing = false; hasVideo = false; videoId = ""; state = 0; shortActive = false; shortHeld = false; _holds = []
            detailsChanged(); streamChanged(); playerChanged()
        }
        function seek(ms) { log("seek:" + ms); position = ms }
        function setQuality(h) { log("quality:" + h); requestedHeight = h; streamChanged() }
        function setAudioOnlyMode(on) { log("audioOnly:" + on); audioOnly = on; hasVideo = !on; streamChanged() }
        function retry() { log("retry"); state = 1 }
    }

    property ListModel searchModel: ListModel {
        property string query: ""
        property bool busy: false
        property bool canLoadMore: false
        property string errorText: ""
        // Catalog component filters (DSC-02) and hidden-row footer (DSC-18).
        property int sort: 0
        property int uploadDate: 0
        property int type: 0
        property int duration: 0
        property bool live: false
        property bool filtersActive: false
        property int hiddenCount: 0
        property bool mixed: false
        property bool hideShorts: true
        property bool hideMembersOnly: false
        function setFilters(sort, date, type, duration, live) { }
        property var calls: []
        function search(q) { calls = calls.concat(["search:" + q]); query = q; busy = true; clear() }
        function loadMore() { calls = calls.concat(["more"]) }
        function cancel() { calls = calls.concat(["cancel"]); busy = false }
        function deliver(n) {
            for (var i = 0; i < n; ++i)
                append({ videoId: "v" + i, title: "Result " + i, channel: "Channel " + i, thumbnail: "",
                         duration: "3:2" + (i % 10), viewCount: 1000 * (i + 1), live: i === 1 })
            busy = false
        }
    }

    property QtObject appSettings: QtObject {
        property int revision: 0
        property string variant: "FULL"
        property var schema: []
        property bool restartRequired: false
        property int doubleTapSeconds: 10
        property bool hardwareDecoding: false
        property var store: ({})
        property var writes: []
        signal changed()
        signal valueChanged(string key)
        function value(key, fallback) { return store.hasOwnProperty(key) ? store[key] : fallback }
        function setValue(key, v) {
            var s = {}
            for (var k in store) s[k] = store[k]
            s[key] = v; store = s
            writes = writes.concat([key + "=" + v])
            revision += 1
            valueChanged(key)
            changed()
        }
    }

    // Library component objects (LibraryModels, LibraryPlaybackBridge): the members the shell and the
    // watch page reach. `downloads` is left out: every use is guarded by typeof.
    property bool storeVariant: false
    property ListModel subscriptionsModel: ListModel {
        // One subscribed channel, so the app starts as it does for a user who has some: on the
        // Subscriptions feed. (Without any, the landing is Home; tst_shell covers that.)
        ListElement { channelId: "UCmockchannel00000000000"; name: "A subscribed channel"; avatar: "" }
        property int revision: 0
        property int sortMode: 0
        property int pendingHandles: 0
        property var subscribed: ({})
        function isSubscribed(id) { return subscribed.hasOwnProperty(id) }
        function unsubscribe(id) { if (isSubscribed(id)) toggle(id, "", "") }
        function toggle(id, name, thumb) {
            var s = {}
            for (var k in subscribed) if (k !== id) s[k] = true
            if (!subscribed.hasOwnProperty(id)) s[id] = true
            subscribed = s; revision += 1
        }
    }
    property ListModel playlistsModel: ListModel {
        property int revision: 0
        function create(name) { return "" }
        function rename(id, name) { }
        function remove(id) { }
        function move(id, from, to) { }
        function inWatchLater(id) { return false }
        function toggleWatchLater(video) { revision += 1 }
        // The items of one list (PlaylistItemsModel): tests put rows into listRows[id] first.
        property var listRows: ({})
        function openList(id) {
            var m = mocks.playlistItems.createObject(mocks, { playlistId: id })
            var rows = listRows[id] || []
            for (var i = 0; i < rows.length; ++i) m.append(rows[i])
            return m
        }
    }
    property Component playlistItems: Component {
        ListModel {
            property string playlistId: ""
            property string title: "My list"
            property int kind: 0          // 1 Watch later, 2 a saved YouTube playlist
            property string remoteId: ""
            property real refreshed: 1
            function items(shuffled) {
                var out = []
                for (var i = 0; i < count; ++i) out.push(get(i))
                if (shuffled) out.reverse()
                return out
            }
            function remove(videoId) { return true }
            function move(from, to) { return true }
        }
    }
    property ListModel historyModel: ListModel {
        property bool paused: false
        property int cap: 500
        property int total: 0
        function remove(id) { }
        function markWatched(video) { }
        function unmarkWatched(id) { }
    }
    property QtObject watchIndex: QtObject {
        property bool hideWatched: false
        property bool hideWatchedInSearch: false
        signal changed(string videoId)
        signal settingsChanged()
        function state(id) { return { showProgress: false, progress: 0, watched: false } }
        function hidden(id, context) { return false }
        function resumePosition(id) { return -1 }
    }
    property QtObject libraryPlayback: QtObject {
        property int resumeBehaviour: 1
        property bool resumeOfferVisible: false
        property real resumeOfferPosition: 0
        property int resumeOfferSecondsLeft: 0
        function acceptResume() { resumeOfferVisible = false }
        function declineResume() { resumeOfferVisible = false }
    }
    property QtObject importer: QtObject { property bool busy: false }

    // Catalog component objects (CatalogComposition): the members the shell, the views and the
    // watch page reach.
    property QtObject homeSettings: QtObject {
        property string landingPage: "subscriptions"
        property bool signedIn: false
    }
    property ListModel feedModel: ListModel {
        property bool busy: false
        property bool refreshing: false
        property bool canLoadMore: false
        property int channelCount: 0
        property string errorText: ""
        property var calls: []
        function refresh(force) { calls = calls.concat(["refresh:" + force]) }
        function loadMore() { calls = calls.concat(["loadMore"]) }
    }
    // Catalog's Shorts model. `get()` of a ListModel hands out a snapshot that tells nobody when the
    // row changes, like the real model's: the details of a Short arrive after its row exists, and a
    // test changes them with setProperty() to see that the view follows.
    property ListModel shortsModel: ListModel {
        property bool busy: false
        property bool canLoadMore: false
        property string errorText: ""
        property bool signedIn: false
        // Grows whenever any row's data changes (catalog's read-only counter).
        property int revision: 0
        // Written by the pager: the Short on screen is playing, so the details requests may go.
        property bool playbackRunning: false
        property var calls: []
        function refresh() { calls = calls.concat(["refresh"]) }
        function loadMore() { calls = calls.concat(["loadMore"]) }
        function setCurrent(row) { calls = calls.concat(["current:" + row]) }
    }
    property ListModel recommendedModel: ListModel {
        property bool busy: false
        property bool signedIn: false
        property bool popular: true   // DSC-04a: signed out, Home is the popular list
        property bool canLoadMore: false
        property string errorText: ""
        function reload() { }
        function loadMore() { }
    }
    property ListModel suggestionModel: ListModel {
        property string query: ""
        property bool historyPaused: false
        function submit(text) { }
        function removeHistory(text) { }
    }
    property ListModel commentsModel: ListModel {
        property string videoId: ""
        property bool started: false
        property bool busy: false
        property string errorText: ""
        property string emptyText: ""
        property string totalText: ""
        property string footerText: ""
        property bool canLoadMore: false
        property string sort: "top"
        property bool canSort: false
        property string autoLoadMode: "near"
        function setVideo(id, token) { videoId = id }
        function start() { started = true }
        function loadMore() { }
        function toggleReplies(row) { }
        function loadMoreReplies(row) { }
        function retry() { }
        function playbackStarted() { }
        function scrolledNear() { }
    }
    property QtObject watchInfo: QtObject {
        property string videoId: ""
        // Catalog: watch-next waits for the player (WatchInfo.waitForPlayback / playbackRunning).
        property bool waitForPlayback: false
        property bool playbackRunning: false
        property bool offline: false
        property real durationHint: 0
        property var fallbackChapters: []
        property string avatar: ""
        property string owner: ""
        property string ownerId: ""
        property real subscriberCount: -1
        property string subscriberText: ""
        property real viewCount: -1
        property string viewText: ""
        property real likeCount: -1
        // RYD-01: the dislike estimate, -1 while unknown or while the setting is off.
        property real dislikeCount: -1
        property string dislikeText: ""
        property string dateText: ""
        property bool live: false
        property string descriptionStyled: ""
        property string shownDescription: ""
        property bool descriptionTruncated: false
        property bool collapsed: true
        property int collapsedLines: 5
        property string commentsContinuation: ""
        signal seekRequested(real seconds)
        signal openUrlRequested(string url)
        signal hashtagRequested(string tag)
        function shareUrl(atTime, seconds) { return "https://youtu.be/" + videoId }
        function openInBrowser(atTime, seconds) { }
        function activateLink(link) { }
        function toggleCollapsed() { collapsed = !collapsed }
        property QtObject chapters: QtObject {
            property int count: 0
            property string currentTitle: ""
            signal jumpRequested(real seconds)
            function setPosition(seconds) { }
        }
        property QtObject related: QtObject {
            property string videoId: ""
            signal stateChanged()
            // Tests fill rows; count follows them.
            property var rows: []
            property int count: rows.length
            property bool canLoadMore: false
            function get(i) { return rows[i] || {} }
        }
    }

    // StartupTiming (NFR-PERF-01/07): records what the QML marks.
    property QtObject startupTiming: QtObject {
        property var lines: []
        property var open: ({})
        signal marked(string stage)
        function mark(stage) {
            if (lines.indexOf("startup " + stage) >= 0) return
            lines = lines.concat(["startup " + stage])
            marked(stage)
        }
        function hasMark(stage) { return lines.indexOf("startup " + stage) >= 0 }
        function begin(name) { var o = {}; o[name] = true; open = o }
        function end(name, stage) {
            var line = name + " " + stage
            if (open[name] && lines.indexOf(line) < 0) lines = lines.concat([line])
        }
        function running(name) { return open[name] === true }
        function elapsed() { return 0 }
    }
}
