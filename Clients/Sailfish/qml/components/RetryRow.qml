// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
import QtQuick 2.6
import Sailfish.Silica 1.0

// Inline error at the end of a list that already has rows (docs/DESIGN.md §5.3): the error and
// "Tap to retry." in one touch target. Lists never retry on scroll after an error; only this tap
// does. Uses the channel page's text, so every list says it the same way in every language.
BackgroundItem {
    id: row
    property string errorText
    width: parent ? parent.width : 0
    height: Math.max(Theme.itemSizeSmall, label.height + 2 * Theme.paddingMedium)
    Label {
        id: label
        anchors.centerIn: parent
        width: parent.width - 2 * Theme.horizontalPageMargin
        horizontalAlignment: Text.AlignHCenter
        wrapMode: Text.Wrap
        font.pixelSize: Theme.fontSizeExtraSmall
        color: Theme.highlightColor
        //: Inline error at the end of a list, %1 is the error text
        text: qsTranslate("ChannelPage", "%1 Tap to retry.").arg(row.errorText)
    }
}
