// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
import QtQuick 2.6
import Sailfish.Silica 1.0
import "../components"

// LIB-01: subscribed channels, A–Z or by latest upload; long press unsubscribes.
Page {
    id: page

    SilicaListView {
        id: list
        anchors.fill: parent
        model: subscriptionsModel

        PullDownMenu {
            MenuItem {
                text: subscriptionsModel.sortMode === 0
                      //: Menu entry sorting subscriptions by their newest upload
                      ? qsTr("Sort by latest upload")
                      //: Menu entry sorting subscriptions alphabetically
                      : qsTr("Sort A–Z")
                onClicked: subscriptionsModel.sortMode = subscriptionsModel.sortMode === 0 ? 1 : 0
            }
        }

        header: PageHeader {
            //: Title of the subscriptions page
            title: qsTr("Subscriptions")
            description: subscriptionsModel.pendingHandles > 0
                         //: %n channels imported by handle that still have to be looked up online
                         ? qsTr("%n channel(s) waiting to be looked up", "", subscriptionsModel.pendingHandles) : ""
        }

        delegate: ListItem {
            id: item
            contentHeight: Math.max(Theme.itemSizeMedium, nameLabel.implicitHeight + 2 * Theme.paddingMedium)
            menu: ContextMenu {
                MenuItem {
                    //: Context menu entry removing a channel subscription
                    text: qsTr("Unsubscribe")
                    onClicked: {
                        var id = model.channelId
                        //: Remorse text while a subscription is being removed
                        item.remorseAction(qsTr("Unsubscribing"), function() { subscriptionsModel.unsubscribe(id) })
                    }
                }
            }
            Avatar {
                id: avatar
                // Anchored, not x:, so right-to-left layouts mirror it (I18N-03).
                anchors.left: parent.left
                anchors.leftMargin: Theme.horizontalPageMargin
                anchors.verticalCenter: parent.verticalCenter
                width: Theme.iconSizeMedium
                height: width
                // A subscription that came from an import has no picture of its own: use the one the
                // catalog has learnt for the channel while browsing, as soon as it knows it.
                source: model.avatar !== "" ? model.avatar
                        : (typeof channelAvatars !== "undefined" && channelAvatars.revision >= 0
                           ? channelAvatars.get(model.channelId) : "")
            }
            Label {
                id: nameLabel
                anchors {
                    left: avatar.right; leftMargin: Theme.paddingMedium
                    right: parent.right; rightMargin: Theme.horizontalPageMargin
                    verticalCenter: parent.verticalCenter
                }
                text: model.name
                truncationMode: TruncationMode.Fade
                color: item.highlighted ? Theme.highlightColor : Theme.primaryColor
            }
            onClicked: {
                if (typeof window.openChannel === "function") window.openChannel(model.channelId, model.name)
            }
        }

        ViewPlaceholder {
            enabled: subscriptionsModel.count === 0
            //: Empty subscriptions page
            text: qsTr("No subscriptions")
            //: Hint on the empty subscriptions page
            hintText: qsTr("Subscribe from a channel or video page, or import a backup.")
        }
        VerticalScrollDecorator { }
    }
}
