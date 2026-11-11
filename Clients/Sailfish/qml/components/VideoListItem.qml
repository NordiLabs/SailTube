// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
import QtQuick 2.6
import Sailfish.Silica 1.0
import "Format.js" as Format
import "../logic/Rows.js" as Rows

ListItem {
    id: item
    property string videoId
    property string videoTitle
    property string channelName
    property string thumbnailUrl
    property string durationText
    property real views: -1
    property bool isLive: false
    // Optional highlighted line under the meta lines: "Members only · Short" (DSC-17/18).
    property string badgeText: ""
    // A channel in a mixed list (search): the picture is a round avatar centred in the
    // thumbnail slot, never cropped into 16:9 (docs/DESIGN.md §4.2).
    property bool channelRow: false
    // The channel of a channel row: with a valid id the row carries a Subscribe button (LIB-01).
    property string channelId: ""
    // YouTube Android's card (docs/DESIGN.md §4.1): lists that opt in (Home, Subscriptions,
    // Search) show a full-width thumbnail with the avatar, title and meta under it on portrait-wide
    // rows; landscape keeps the compact row.
    property bool cardLayout: false
    property string avatarUrl: ""
    // A larger picture for the card (catalog's thumbnailLarge), when the model has one.
    property string largeThumbnailUrl: ""
    readonly property bool cardLarge: card && largeThumbnailUrl !== ""
    readonly property bool card: Rows.useCard(cardLayout && !channelRow, width, Screen.width)
    //: Number of views of a video, %1 is a compact number such as 1.2M
    readonly property string viewsText: views >= 0 ? qsTr("%1 views").arg(Format.count(views, Qt.locale())) : ""

    readonly property real naturalHeight: card
        ? thumb.height + Theme.paddingMedium + Math.max(avatarSlot.height, texts.height) + Theme.paddingLarge
        : Math.max(thumb.height, texts.height) + 2 * Theme.paddingMedium
    contentHeight: naturalHeight

    // E12 (catalog component): a finger on a video row is a hint for playback's speculative resolve.
    property var intentHints: ({ title: videoTitle, channel: channelName, thumbnail: thumbnailUrl })
    property bool intentClicked: false
    onPressedChanged: {
        if (typeof tapIntent === "undefined" || videoId === "") return
        if (pressed) { intentClicked = false; tapIntent.pressed(videoId, intentHints) }
        else intentRelease.restart()   // `clicked` arrives after the release
    }
    Timer { id: intentRelease; interval: 0; onTriggered: if (typeof tapIntent !== "undefined") tapIntent.released(item.videoId, item.intentClicked) }
    onClicked: {
        intentClicked = true
        if (typeof tapIntent !== "undefined" && videoId !== "") tapIntent.opening(videoId)
    }

    Rectangle {
        id: thumb
        // Anchored, not x:, so right-to-left layouts mirror it (I18N-03).
        anchors.left: parent.left
        anchors.leftMargin: item.card ? 0 : Theme.horizontalPageMargin
        y: item.card ? 0 : Theme.paddingMedium
        // Card: the full width. Row: 42 % of the portrait width, kept in landscape (§4.1).
        width: item.card ? item.width : Rows.thumbWidth(item.width, Screen.width)
        height: Rows.thumbHeight(width)
        color: item.channelRow ? "transparent" : Theme.rgba(Theme.highlightBackgroundColor, 0.1) // theme-exempt: no tint behind a round avatar

        // Only channel rows load the round avatar (file and items), not every video row.
        Loader {
            id: avatarLoader
            active: item.channelRow
            anchors.centerIn: parent
            height: parent.height
            width: height
            source: active ? "Avatar.qml" : ""
        }
        Binding {
            target: avatarLoader.item
            when: avatarLoader.status === Loader.Ready
            property: "source"
            value: item.thumbnailUrl
        }
        Image {
            visible: !item.channelRow
            anchors.fill: parent
            source: item.channelRow ? "" : (item.cardLarge ? item.largeThumbnailUrl : item.thumbnailUrl)
            fillMode: Image.PreserveAspectCrop
            clip: true
            asynchronous: true
            // Cards never blow the small list thumbnail up on the CPU (Rows.decodeWidth).
            sourceSize.width: item.card ? Rows.decodeWidth(width, item.cardLarge) : width
        }
        // Library component: watched progress and badge (LIB-06), download state (DL-04).
        WatchOverlay {
            anchors.fill: parent
            videoId: item.videoId
        }
        DownloadBadge {
            anchors { right: parent.right; top: parent.top; margins: Theme.paddingSmall }
            videoId: item.videoId
        }
        Rectangle {
            visible: durationLabel.text !== ""
            anchors { right: parent.right; bottom: parent.bottom; margins: Theme.paddingSmall }
            width: durationLabel.width + Theme.paddingSmall * 2
            height: durationLabel.height
            radius: Theme.paddingSmall / 2
            color: item.isLive ? Theme.errorColor : Qt.rgba(0, 0, 0, 0.75) // theme-exempt: scrim over the thumbnail
            Label {
                id: durationLabel
                anchors.centerIn: parent
                font.pixelSize: Theme.fontSizeTiny
                color: Theme.lightPrimaryColor
                //: Badge on the thumbnail of a live stream
                text: item.isLive ? qsTr("LIVE") : item.durationText
            }
        }
    }

    // Card: the channel's round avatar beside the title, or its initial while there is none.
    Item {
        id: avatarSlot
        visible: item.card
        anchors { left: parent.left; leftMargin: Theme.horizontalPageMargin; top: thumb.bottom; topMargin: Theme.paddingMedium }
        width: item.card ? Theme.iconSizeMedium : 0
        height: width
        Rectangle {
            anchors.fill: parent
            visible: item.avatarUrl === ""
            radius: width / 2
            color: Theme.rgba(Theme.highlightBackgroundColor, 0.25)
            Label {
                anchors.centerIn: parent
                text: Rows.initial(item.channelName)
                font.pixelSize: Theme.fontSizeMedium
                color: Theme.highlightColor
            }
        }
        Loader {
            id: cardAvatar
            anchors.fill: parent
            active: item.card && item.avatarUrl !== ""
            source: active ? "Avatar.qml" : ""
        }
        Binding {
            target: cardAvatar.item
            when: cardAvatar.status === Loader.Ready
            property: "source"
            value: item.avatarUrl
        }
    }

    Column {
        id: texts
        anchors {
            left: item.card ? avatarSlot.right : thumb.right; leftMargin: Theme.paddingMedium
            right: parent.right; rightMargin: Theme.horizontalPageMargin
            top: item.card ? thumb.bottom : thumb.top
            topMargin: item.card ? Theme.paddingMedium : 0
        }
        spacing: item.card ? Theme.paddingSmall / 2 : Theme.paddingSmall
        Label {
            width: parent.width
            text: item.videoTitle
            wrapMode: Text.Wrap
            maximumLineCount: item.card ? 2 : 3
            elide: Text.ElideRight
            font.pixelSize: item.card ? Theme.fontSizeMedium : Theme.fontSizeSmall
            color: item.highlighted ? Theme.highlightColor : Theme.primaryColor
        }
        // Card: one meta line, "channel · views · age".
        Label {
            width: parent.width
            text: item.card ? Rows.cardMeta(item.channelName, item.viewsText) : item.channelName
            truncationMode: TruncationMode.Fade
            font.pixelSize: Theme.fontSizeExtraSmall
            color: item.highlighted ? Theme.secondaryHighlightColor : Theme.secondaryColor
        }
        Label {
            width: parent.width
            visible: item.views >= 0 && !item.card
            text: item.viewsText
            font.pixelSize: Theme.fontSizeExtraSmall
            color: item.highlighted ? Theme.secondaryHighlightColor : Theme.secondaryColor
        }
        Label {
            width: parent.width
            visible: item.badgeText !== ""
            text: item.badgeText
            truncationMode: TruncationMode.Fade
            font.pixelSize: Theme.fontSizeExtraSmall
            color: Theme.highlightColor
        }
    }

    // Channel rows: Subscribe at the row's bottom end (its file loads only for such rows).
    Loader {
        id: subscribeLoader
        objectName: "channelSubscribe"
        active: item.channelRow && typeof subscriptionsModel !== "undefined"
                && item.channelId.length === 24 && item.channelId.indexOf("UC") === 0
        anchors { right: parent.right; rightMargin: Theme.horizontalPageMargin
                  bottom: parent.bottom; bottomMargin: Theme.paddingMedium }
        source: active ? "SubscribeButton.qml" : ""
    }
    Binding { target: subscribeLoader.item; when: subscribeLoader.status === Loader.Ready; property: "channelId"; value: item.channelId }
    Binding { target: subscribeLoader.item; when: subscribeLoader.status === Loader.Ready; property: "channelName"; value: item.videoTitle }
    Binding { target: subscribeLoader.item; when: subscribeLoader.status === Loader.Ready; property: "avatarUrl"; value: item.thumbnailUrl }
}
