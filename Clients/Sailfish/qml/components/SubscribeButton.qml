// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
import QtQuick 2.6
import Sailfish.Silica 1.0

// Subscribe / Subscribed for a channel (LIB-01), the app's one such button: on the watch page's
// owner row, on the channel page and on channel rows. The same local action and texts everywhere,
// with remorse before unsubscribing. Compact by default, as the official app's pill; the channel
// page, where it is the main action, makes it wider.
Button {
    id: button
    property string channelId
    property string channelName
    property string avatarUrl
    readonly property bool subscribed: subscriptionsModel.revision >= 0 && subscriptionsModel.isSubscribed(channelId)

    preferredWidth: Theme.buttonWidthExtraSmall
    highlighted: !subscribed
    text: subscribed
          //: Button state of a channel the user is subscribed to
          ? qsTranslate("VideoPage", "Subscribed")
          //: Button subscribing to the channel of the current video
          : qsTranslate("VideoPage", "Subscribe")
    onClicked: {
        // The row may be gone when the remorse ends: the callback uses captured locals only.
        var id = channelId, subs = subscriptionsModel
        if (!subscribed) { subs.toggle(id, channelName, avatarUrl); return }
        //: Remorse text while a subscription is being removed
        Remorse.popupAction(pageStack.currentPage, qsTranslate("VideoPage", "Unsubscribing"),
                            function () { if (subs.isSubscribed(id)) subs.unsubscribe(id) })
    }
}
