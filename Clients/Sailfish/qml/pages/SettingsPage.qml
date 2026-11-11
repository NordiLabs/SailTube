// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
import QtQuick 2.6
import Sailfish.Silica 1.0
import "../components"
import "../logic/SettingsInventory.js" as Inventory

// All settings (SET-01), each control in exactly one place (SET-02). The order and the
// defaults come from logic/SettingsInventory.js; this page only adds labels. Pass focusKey to
// scroll to a control and flash it, e.g. from an error message (window.openSetting).
Page {
    id: page
    allowedOrientations: Orientation.All
    property string focusKey: ""
    readonly property bool miniPlayerOptOut: true
    readonly property string variant: appSettings.variant === "STORE" ? "STORE" : "FULL"
    // The decoder element, not the codec report: with the setting off the probe demotes
    // droidvdec and reports software decoding, but the switch must stay usable.
    readonly property bool hardwareDecodeOffered: typeof deviceCapabilities !== "undefined"
        && (deviceCapabilities.elements || []).indexOf("droidvdec") >= 0

    // Adapter over the catalog component's HomeSettings for the start page row.
    property QtObject homeStore: QtObject {
        readonly property int revision: typeof homeSettings !== "undefined" ? (homeSettings.landingPage, page._homeRevision) : 0
        function value(key, fallback) { return homeSettings.landingPage }
        function setValue(key, v) { homeSettings.landingPage = v; page._homeRevision += 1 }
    }
    property int _homeRevision: 0

    // Keys the library and catalog components store (SET-02): the row here writes their object.
    property QtObject historyCapStore: PropertyStore {
        target: typeof historyModel !== "undefined" ? historyModel : null; property: "cap" }
    property QtObject historyPausedStore: PropertyStore {
        target: typeof historyModel !== "undefined" ? historyModel : null; property: "paused" }
    property QtObject hideWatchedStore: PropertyStore {
        target: typeof watchIndex !== "undefined" ? watchIndex : null; property: "hideWatched" }
    property QtObject resumeStore: PropertyStore {
        target: typeof libraryPlayback !== "undefined" ? libraryPlayback : null; property: "resumeBehaviour"
        fromTarget: function (v) { return ["always", "ask", "never"][v] || "always" }
        toTarget: function (v) { return Math.max(0, ["always", "ask", "never"].indexOf(v)) }
    }
    property QtObject unmeteredStore: PropertyStore {
        target: typeof downloads !== "undefined" ? downloads : null; property: "unmeteredOnly" }
    property QtObject rydStore: PropertyStore {
        target: typeof dislikes !== "undefined" ? dislikes : null; property: "enabled" }
    property QtObject commentsStore: PropertyStore {
        target: typeof commentsModel !== "undefined" ? commentsModel : null; property: "autoLoadMode"
        // The comments model names the modes off / near / playback.
        fromTarget: function (v) { return v === "off" ? "never" : v === "playback" ? "always" : "near" }
        toTarget: function (v) { return v === "never" ? "off" : v === "always" ? "playback" : "near" }
    }

    function heightLabelsFor(zeroLabel) {
        var out = []
        var values = Inventory.entry("playback/maxHeight").values
        for (var i = 0; i < values.length; ++i) out.push(values[i] === 0 ? zeroLabel : values[i] + "p")
        return out
    }
    readonly property var sponsorModes: [
        //: SponsorBlock category mode: never act on this category
        qsTr("Off"),
        //: SponsorBlock category mode: skip the segment automatically
        qsTr("Skip"),
        //: SponsorBlock category mode: show a skip button during the segment
        qsTr("Show button")
    ]
    // SEL-02: 0 lets the selection choose (720p, 1080p with verified hardware decode).
    //: Maximum quality choice: chosen by the app for this device and decoder
    readonly property var heightLabels: heightLabelsFor(qsTr("Automatic"))
    //: Maximum quality on mobile data without a limit
    readonly property var mobileHeightLabels: heightLabelsFor(qsTr("No limit"))


    function controlFor(key, item) {
        item = item || column
        var kids = item.children || []
        for (var i = 0; i < kids.length; ++i) {
            var child = kids[i]
            if (child.settingKey === key) return child
            var found = controlFor(key, child)
            if (found) return found
        }
        return null
    }

    onStatusChanged: {
        if (status !== PageStatus.Active || focusKey === "") return
        var control = controlFor(focusKey)
        if (!control) return
        // Offset from the heights of the visible rows above it (independent of when the
        // Column positions its children).
        var y = 0
        for (var i = 0; i < column.children.length && column.children[i] !== control; ++i)
            if (column.children[i].visible) y += column.children[i].height
        flick.contentY = Math.max(0, Math.min(y - Theme.itemSizeLarge, flick.contentHeight - flick.height))
        control.flash = true
        flashTimer.target = control
        flashTimer.restart()
    }
    Timer {
        id: flashTimer
        property Item target
        interval: 1500
        onTriggered: if (target) target.flash = false
    }

    SilicaFlickable {
        id: flick
        anchors.fill: parent
        contentHeight: column.height + Theme.paddingLarge

        Column {
            id: column
            width: parent.width

            PageHeader {
                //: Title of the settings page
                title: qsTr("Settings")
            }

            // Library component (account rows): the optional YouTube account, first on the page.
            SettingLink {
                visible: typeof account !== "undefined"
                page: "AccountPage.qml"
                //: Settings entry opening the optional YouTube account page
                text: qsTr("YouTube account")
            }


            SectionHeader {
                //: Settings section about what lists show
                text: qsTr("Content")
            }
            SettingCombo {
                settingKey: "content/landingPage"
                // The catalog component stores the landing page (DSC-04).
                store: typeof homeSettings !== "undefined" ? page.homeStore : appSettings
                //: Setting: which main page section opens at start
                label: qsTr("Start page")
                labels: [
                    //: Start page choice: the subscriptions feed
                    qsTr("Subscriptions"),
                    //: Start page choice: recommended videos (needs an account)
                    qsTr("Recommended"),
                    //: Start page choice: search
                    qsTr("Search")
                ]
            }
            SettingSwitch {
                settingKey: "content/hideShorts"
                //: Setting: leave Shorts out of lists
                text: qsTr("Hide Shorts")
                //: Explanation of the hide Shorts setting
                description: qsTr("In search, channel lists and the feed.")
            }
            SettingSwitch {
                settingKey: "content/hideWatched"
                store: page.hideWatchedStore.target ? page.hideWatchedStore : appSettings
                //: Setting: leave watched videos out of the feed
                text: qsTr("Hide watched videos")
                //: Explanation of the hide watched setting
                description: qsTr("In the feed and channel lists.")
            }
            TextSwitch {
                // LIB-07 option of the library component, not a SET-01 row: a sub-switch of the one above.
                visible: typeof watchIndex !== "undefined"
                enabled: visible && watchIndex.hideWatched
                leftMargin: Theme.horizontalPageMargin + Theme.paddingLarge
                //: Setting: also hide watched videos from search results
                text: qsTr("Also in search results")
                checked: visible && watchIndex.hideWatchedInSearch
                automaticCheck: false
                onClicked: watchIndex.hideWatchedInSearch = !watchIndex.hideWatchedInSearch
            }

            SectionHeader {
                //: Settings section about the watch history
                text: qsTr("History")
            }
            SettingSlider {
                settingKey: "history/cap"
                store: page.historyCapStore.target ? page.historyCapStore : appSettings
                //: Setting: how many videos the watch history keeps
                label: qsTr("History size")
            }
            SettingSwitch {
                settingKey: "history/paused"
                store: page.historyPausedStore.target ? page.historyPausedStore : appSettings
                //: Setting: stop recording watched videos
                text: qsTr("Pause history")
            }

            SectionHeader {
                //: Settings section about video playback
                text: qsTr("Playback")
            }
            SettingCombo {
                settingKey: "playback/resume"
                store: page.resumeStore.target ? page.resumeStore : appSettings
                //: Setting: continue a video where it was left
                label: qsTr("Resume videos")
                labels: [
                    //: Resume choice: always continue from the saved position
                    qsTr("Always"),
                    //: Resume choice: ask each time
                    qsTr("Ask"),
                    //: Resume choice: always start from the beginning
                    qsTr("Never")
                ]
            }
            SettingSwitch {
                settingKey: "playback/keepDisplayOn"
                //: Setting: keep the display on while a video plays
                text: qsTr("Keep display on")
                //: Explanation of the keep display on setting
                description: qsTr("While a video plays in the foreground.")
            }
            SettingSwitch {
                settingKey: "playback/background"
                //: Setting: keep the sound playing after leaving the video
                text: qsTr("Background playback")
                //: Explanation of the background playback setting
                description: qsTr("Keeps the sound playing when you leave the video or lock the screen.")
            }
            SettingCombo {
                settingKey: "playback/verticalFullscreen"
                //: Setting: how vertical videos and Shorts go fullscreen
                label: qsTr("Vertical videos in fullscreen")
                labels: [
                    //: Vertical-video fullscreen choice: stay upright
                    qsTr("Upright"),
                    //: Vertical-video fullscreen choice: turn sideways like other videos
                    qsTr("Sideways")
                ]
            }
            SettingCombo {
                settingKey: "playback/edgeGlow"
                //: Setting: the glowing page indicator at the screen edge on the watch page
                label: qsTr("Edge glow on the video")
                labels: [
                    //: Edge glow choice: hide it while watching, show it when paused
                    qsTr("Automatic"),
                    //: Edge glow choice: never show it on the watch page
                    qsTr("Always hidden")
                ]
                //: Explanation of the edge glow setting
                description: qsTr("Swiping back works either way.")
            }
            SettingSwitch {
                settingKey: "playback/autoplayRelated"
                //: Setting: play a related video after the current one ends
                text: qsTr("Autoplay next video")
                //: Explanation of the autoplay setting
                description: qsTr("Starts after a short countdown that you can cancel.")
            }
            SettingCombo {
                settingKey: "playback/doubleTapSeconds"
                //: Setting: how far a double tap on the video seeks
                label: qsTr("Double-tap seek")
                labels: [
                    //: Seek step in seconds, %1 is a number
                    qsTr("%1 s").arg(5), qsTr("%1 s").arg(10), qsTr("%1 s").arg(15)
                ]
            }
            SettingSwitch {
                settingKey: "playback/keepSessionOnLeave"
                //: Setting: keep the video loaded after leaving the watch page
                text: qsTr("Keep playing after leaving the video")
                //: Explanation of the keep session setting
                description: qsTr("The mini-player stays at the bottom of the app.")
            }
            SettingSwitch {
                settingKey: "playback/coverVideo"
                visible: page.variant === "FULL"
                //: Setting: show the playing video live on the app cover in the home screen
                text: qsTr("Video on the cover")
                //: Explanation of the cover video setting
                description: qsTr("While a video plays, the app cover shows it live. Uses more battery while the home screen is visible.")
            }
            SettingSwitch {
                settingKey: "playback/resumeAfterInterruption"
                //: Setting: continue playing after a call or an alarm
                text: qsTr("Resume after calls and alarms")
            }
            SettingCombo {
                settingKey: "comments/autoLoad"
                store: page.commentsStore.target ? page.commentsStore : appSettings
                //: Setting: when comments are loaded on the watch page
                label: qsTr("Load comments")
                labels: [
                    //: Comment loading choice: when scrolled near them
                    qsTr("When near"),
                    //: Comment loading choice: as soon as the video opens
                    qsTr("Always"),
                    //: Comment loading choice: only on request
                    qsTr("On request")
                ]
            }

            SectionHeader {
                //: Settings section about video quality and decoding
                text: qsTr("Quality")
            }
            SettingCombo {
                settingKey: "playback/maxHeight"
                //: Setting: tallest picture chosen automatically on Wi-Fi
                label: qsTr("Maximum quality on Wi-Fi")
                labels: page.heightLabels
            }
            SettingCombo {
                settingKey: "playback/maxHeightMobile"
                //: Setting: tallest picture chosen automatically on mobile data
                label: qsTr("Maximum quality on mobile data")
                labels: page.mobileHeightLabels
            }
            SettingSwitch {
                settingKey: "playback/hardwareDecode"
                enabled: page.hardwareDecodeOffered
                //: Setting: decode video with the device's hardware decoder
                text: qsTr("Hardware decoding")
                description: page.hardwareDecodeOffered
                             //: Explanation of the hardware decoding setting
                             ? qsTr("Uses the phone's video decoder. Turn off if videos stay black or stall.")
                             //: Shown when the device has no usable hardware decoder
                             : qsTr("No hardware decoder was found on this device.")
            }
            SettingCombo {
                settingKey: "playback/codecPreference"
                //: Setting: which video codec is preferred when several are playable
                label: qsTr("Preferred codec")
                labels: [
                    //: Codec preference: let the application decide
                    qsTr("Automatic"),
                    "H.264", // i18n-exempt: codec name
                    "VP9" // i18n-exempt: codec name
                ]
                //: Explanation under the codec preference setting
                description: qsTr("Automatic prefers codecs that this device decodes in hardware.")
            }
            SettingSwitch {
                settingKey: "playback/highFrameRate"
                //: Setting: allow 50/60 frames per second streams
                text: qsTr("High frame rate")
            }
            SettingCombo {
                settingKey: "playback/adaptivePipeline"
                //: Setting: how separate audio and video streams are combined
                label: qsTr("Adaptive streams")
                labels: [
                    //: Adaptive streams disabled: only combined (progressive) streams
                    qsTr("Off"),
                    //: Adaptive streams chosen automatically
                    qsTr("Automatic"),
                    //: Adaptive streams: always use the newer GStreamer pipeline (diagnostics)
                    qsTr("Always use playbin3")
                ]
                //: Explanation of the adaptive streams setting
                description: qsTr("Higher qualities need separate audio and video streams. Off limits playback to combined streams (usually 360p).")
            }
            Label {
                x: Theme.horizontalPageMargin
                width: parent.width - 2 * x
                visible: appSettings.restartRequired
                wrapMode: Text.Wrap
                color: Theme.highlightColor
                font.pixelSize: Theme.fontSizeSmall
                //: Shown after changing a setting that applies at start-up
                text: qsTr("Restart SailTube to apply the decoder settings.")
            }

            SectionHeader {
                //: Settings section about sound
                text: qsTr("Sound and captions")
            }
            SettingLink {
                settingKey: "audio/equalizer"
                //: Settings row opening the equalizer
                text: qsTr("Equalizer")
            }
            SettingSlider {
                settingKey: "audio/boost"
                unit: " %"
                //: Setting: amplify quiet videos
                label: qsTr("Volume boost")
            }
            SettingSwitch {
                settingKey: "audio/loudness"
                //: Setting: even out loudness between videos
                text: qsTr("Loudness normalisation")
            }
            SettingSwitch {
                settingKey: "playback/stableVolume"
                //: Setting: prefer the audio version that keeps the volume steady within a video
                text: qsTr("Stable volume")
                //: Explanation of the stable volume setting
                description: qsTr("Uses the steadier audio version of a video when YouTube offers one.")
            }
            SettingLink {
                settingKey: "captions/style"
                //: Settings row opening the caption appearance page
                text: qsTr("Caption style")
            }

            SectionHeader {
                //: Settings section about community services
                text: qsTr("Community services")
            }
            SettingSwitch {
                settingKey: "integrations/ryd"
                store: page.rydStore.target ? page.rydStore : appSettings
                //: Setting: show estimated dislike counts from a community service
                text: qsTr("Show dislike estimates")
                //: Explanation of the dislike estimate setting
                description: qsTr("Asks the Return YouTube Dislike service for each video you open.")
            }

            SectionHeader {
                //: Settings section about SponsorBlock segment skipping
                text: qsTr("SponsorBlock")
            }
            SettingSwitch {
                id: sponsorMaster
                settingKey: "sponsorblock/enabled"
                //: Setting: skip or mark community-submitted segments such as sponsors
                text: qsTr("SponsorBlock")
                //: Explanation of the SponsorBlock master switch
                description: qsTr("Uses segments that viewers submit to the SponsorBlock community.")
            }
            SettingCombo {
                settingKey: "sponsorblock/sponsor"
                // SB-02 category (SponsorBlock API name), checked by tools/platform_check.py.
                readonly property string category: "sponsor"
                enabled: sponsorMaster.checked
                //: SponsorBlock category: paid promotion inside a video
                label: qsTr("Sponsor")
                labels: page.sponsorModes
            }
            SettingCombo {
                settingKey: "sponsorblock/selfpromo"
                // SB-02 category (SponsorBlock API name), checked by tools/platform_check.py.
                readonly property string category: "selfpromo"
                enabled: sponsorMaster.checked
                //: SponsorBlock category: unpaid or self promotion
                label: qsTr("Unpaid or self promotion")
                labels: page.sponsorModes
            }
            SettingCombo {
                settingKey: "sponsorblock/interaction"
                // SB-02 category (SponsorBlock API name), checked by tools/platform_check.py.
                readonly property string category: "interaction"
                enabled: sponsorMaster.checked
                //: SponsorBlock category: reminder to like, subscribe or follow
                label: qsTr("Interaction reminder")
                labels: page.sponsorModes
            }
            SettingCombo {
                settingKey: "sponsorblock/intro"
                // SB-02 category (SponsorBlock API name), checked by tools/platform_check.py.
                readonly property string category: "intro"
                enabled: sponsorMaster.checked
                //: SponsorBlock category: intermission or intro animation
                label: qsTr("Intermission or intro")
                labels: page.sponsorModes
            }
            SettingCombo {
                settingKey: "sponsorblock/outro"
                // SB-02 category (SponsorBlock API name), checked by tools/platform_check.py.
                readonly property string category: "outro"
                enabled: sponsorMaster.checked
                //: SponsorBlock category: end cards and credits
                label: qsTr("End cards and credits")
                labels: page.sponsorModes
            }
            SettingCombo {
                settingKey: "sponsorblock/preview"
                // SB-02 category (SponsorBlock API name), checked by tools/platform_check.py.
                readonly property string category: "preview"
                enabled: sponsorMaster.checked
                //: SponsorBlock category: preview or recap of other content
                label: qsTr("Preview or recap")
                labels: page.sponsorModes
            }
            SettingCombo {
                settingKey: "sponsorblock/music_offtopic"
                // SB-02 category (SponsorBlock API name), checked by tools/platform_check.py.
                readonly property string category: "music_offtopic"
                enabled: sponsorMaster.checked
                //: SponsorBlock category: non-music part of a music video
                label: qsTr("Non-music section")
                labels: page.sponsorModes
            }
            SettingCombo {
                settingKey: "sponsorblock/filler"
                // SB-02 category (SponsorBlock API name), checked by tools/platform_check.py.
                readonly property string category: "filler"
                enabled: sponsorMaster.checked
                //: SponsorBlock category: tangent or joke that is not needed
                label: qsTr("Filler tangent")
                labels: page.sponsorModes
            }
            SettingCombo {
                // A highlight is a point, so it is shown, never skipped (SB-02).
                settingKey: "sponsorblock/poi_highlight"
                readonly property string category: "poi_highlight"
                enabled: sponsorMaster.checked
                //: SponsorBlock category: the highlight of a video, offered as a jump
                label: qsTr("Highlight")
                labels: [
                    //: SponsorBlock category mode: never act on this category
                    qsTr("Off"),
                    //: SponsorBlock highlight mode: offer a "jump to highlight" button
                    qsTr("Show jump button")
                ]
            }

            SectionHeader {
                //: Settings section about downloads
                text: qsTr("Downloads")
            }
            SettingLink {
                settingKey: "downloads/folder"
                page: "DownloadFolderPage.qml"
                description: typeof downloads !== "undefined"
                             ? (downloads.folder === "" || downloads.folder === downloads.defaultFolder
                                //: Download folder row: the app's private folder is used
                                ? qsTr("App folder") : downloads.folder)
                             : ""
                //: Settings row choosing where downloads are saved
                text: qsTr("Download folder")
            }
            SettingCombo {
                settingKey: "downloads/quality"
                //: Setting: quality of downloaded videos
                label: qsTr("Download quality")
                labels: [
                    "1080p", "720p", "480p", "360p", // i18n-exempt: video heights
                    //: Download quality choice: sound only
                    qsTr("Audio only")
                ]
            }
            SettingSwitch {
                settingKey: "downloads/unmeteredOnly"
                store: page.unmeteredStore.target ? page.unmeteredStore : appSettings
                //: Setting: download only on Wi-Fi or other unmetered networks
                text: qsTr("Download on Wi-Fi only")
            }

            SectionHeader {
                visible: page.variant === "FULL"
                //: Settings section about the video resolver
                text: qsTr("Providers")
            }
            SettingCombo {
                // Hidden while nothing reads the key (SettingsInventory.HIDDEN_KEYS).
                visible: page.variant === "FULL" && inUse
                settingKey: "providers/extractorChannel"
                //: Setting: which yt-dlp release channel updates come from
                label: qsTr("Resolver updates")
                labels: [
                    //: Resolver update channel: stable releases
                    qsTr("Stable"),
                    //: Resolver update channel: nightly builds
                    qsTr("Nightly")
                ]
            }
            SettingSwitch {
                visible: page.variant === "FULL"
                settingKey: "providers/poProvider"
                //: Setting: use a proof-of-origin token provider
                text: qsTr("Proof-of-origin tokens")
            }
            SettingSlider {
                visible: page.variant === "FULL"
                enabled: (appSettings.revision, appSettings.value("providers/poProvider", false) === true)
                settingKey: "providers/poTokenHeapMb"
                //: Unit after the memory limit value, mebibytes
                unit: " " + qsTr("MiB")
                //: Setting: the most memory the proof-of-origin token helper may use
                label: qsTr("Token helper memory limit")
            }

            SectionHeader {
                //: Settings section with diagnostic options
                text: qsTr("Diagnostics")
            }
            SettingCombo {
                settingKey: "providers/clientOverride"
                //: Setting: force a specific YouTube client for diagnostics
                label: qsTr("Player client")
                labels: [
                    //: Player client choice: automatic
                    qsTr("Automatic")
                ]
                //: Explanation of the player client setting
                description: qsTr("For diagnostics only.")
            }
            SettingSwitch {
                settingKey: "diagnostics/timingLog"
                //: Setting: write a timing log for performance measurements
                text: qsTr("Timing log")
                //: Explanation of the timing log setting
                description: qsTr("Records start-up and playback timings on this device. Never uploaded.")
            }
            SettingSwitch {
                settingKey: "diagnostics/startLowRung"
                //: Setting (experiment): start videos at a low quality, then switch to the chosen one
                text: qsTr("Start at low quality, then switch up")
                //: Explanation of the low-quality start experiment
                description: qsTr("A start-up experiment: the picture may appear sooner, then sharpens once.")
            }
            BackgroundItem {
                width: parent.width
                Label {
                    x: Theme.horizontalPageMargin
                    width: parent.width - 2 * x
                    anchors.verticalCenter: parent.verticalCenter
                    truncationMode: TruncationMode.Fade
                    color: parent.highlighted ? Theme.highlightColor : Theme.primaryColor
                    //: Opens the page listing the device's decoders
                    text: qsTr("Video capabilities")
                }
                onClicked: pageStack.push(Qt.resolvedUrl("CapabilitiesPage.qml"))
            }
        }
        VerticalScrollDecorator { }
    }
}
