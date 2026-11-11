// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
import QtQuick 2.6

// Headless stand-in for the catalog component's ChannelModel (Clients/Sailfish/Presentation): the
// header fields, the tabs and the list state a channel page reads. Nothing loads: a test finds
// the model through the page's list (its `model`) and sets what the page should show.
ListModel {
    property string reference: ""
    property string channelId: ""
    property string title: ""
    property string handle: ""
    property string avatarUrl: ""
    property string subscriberText: ""
    property real subscriberCount: -1
    property string videoCountText: ""
    property real videoCount: -1
    property string description: ""
    property string tab: "videos"
    property var tabs: []
    property bool fallbackTab: false
    property bool busy: false
    property bool canLoadMore: false
    property string errorText: ""
    property string outcome: ""
    readonly property bool empty: count === 0
    property bool rssPainted: false
    property int hiddenShorts: 0
    property int hiddenMembersOnly: 0
    property int hiddenRestricted: 0
    property int hiddenWatched: 0
    readonly property int hiddenCount: hiddenShorts + hiddenMembersOnly + hiddenRestricted + hiddenWatched
    property bool hideShorts: false
    property bool hideMembersOnly: false
    property bool hideRestricted: false
    // What the page asked for, in order ("open:@handle", "refresh", "loadMore").
    property var calls: []
    function open(ref) { reference = ref; calls = calls.concat(["open:" + ref]) }
    function setTab(name) { tab = name }
    function loadMore() { calls = calls.concat(["loadMore"]) }
    function refresh() { calls = calls.concat(["refresh"]) }
}
