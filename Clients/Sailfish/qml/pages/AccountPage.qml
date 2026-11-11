// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
import QtQuick 2.6
import Sailfish.Silica 1.0

// ACC-01..10: the optional YouTube account. Everything in SailTube works
// signed out; this page only imports, checks, switches and removes a session.
// Sign-in methods come in the ACC-02 rank order from `account.methods`.
Page {
    id: page
    objectName: "accountPage"
    readonly property bool available: typeof account !== "undefined"

    function signIn(method) {
        if (method === "browser") account.importFromBrowser()
        else if (method === "paste") pageStack.push(Qt.resolvedUrl("PasteSessionDialog.qml"))
    }

    RemorsePopup { id: remorse }

    SilicaFlickable {
        anchors.fill: parent
        contentHeight: column.height + Theme.paddingLarge

        PullDownMenu {
            visible: page.available && account.hasSession
            MenuItem {
                //: Menu entry removing the imported session from SailTube (the browser stays signed in)
                text: qsTr("Sign out of SailTube")
                //: Remorse text while the session is being removed
                onClicked: remorse.execute(qsTr("Signing out"), function() { account.signOut() })
            }
            MenuItem {
                //: Menu entry checking the imported session now
                text: qsTr("Check now")
                onClicked: account.verifyNow()
            }
        }

        Column {
            id: column
            width: parent.width
            spacing: Theme.paddingMedium

            PageHeader {
                //: Title of the account page
                title: qsTr("YouTube account")
            }

            Label {
                x: Theme.horizontalPageMargin
                width: parent.width - 2 * x
                wrapMode: Text.Wrap
                color: page.available && account.expired ? Theme.errorColor : Theme.highlightColor
                text: page.available ? account.stateText : ""
            }

            // ACC-07: expired -> one-tap re-import with the first method.
            Button {
                anchors.horizontalCenter: parent.horizontalCenter
                visible: page.available && account.expired && account.methods.length > 0
                //: Button importing the session again after it expired
                text: qsTr("Import again")
                onClicked: page.signIn(account.methods[0])
            }

            Label {
                x: Theme.horizontalPageMargin
                width: parent.width - 2 * x
                visible: page.available && account.lastError !== ""
                wrapMode: Text.Wrap
                color: Theme.errorColor
                font.pixelSize: Theme.fontSizeSmall
                text: page.available ? account.lastError : ""
            }

            // ACC-10 [FULL]: the one-time notice when the session is kept in a private file.
            Column {
                width: parent.width
                visible: page.available && account.disclosurePending
                spacing: Theme.paddingSmall
                Label {
                    x: Theme.horizontalPageMargin
                    width: parent.width - 2 * x
                    wrapMode: Text.Wrap
                    font.pixelSize: Theme.fontSizeSmall
                    color: Theme.secondaryHighlightColor
                    //: One-time notice: without the Sailfish Secrets service the session is kept in a private file
                    text: qsTr("Secure storage is not available, so the session is kept in a file only SailTube can read.")
                }
                Button {
                    anchors.horizontalCenter: parent.horizontalCenter
                    //: Button dismissing the storage notice
                    text: qsTr("OK")
                    onClicked: account.acknowledgeDisclosure()
                }
            }

            SectionHeader {
                visible: page.available && account.accounts.length > 0
                //: Section listing the logins and brand channels of the session
                text: qsTr("Channels")
            }
            Repeater {
                model: page.available ? account.accounts : []
                ListItem {
                    contentHeight: Math.max(Theme.itemSizeMedium, accountTexts.height + 2 * Theme.paddingMedium)
                    highlighted: down || modelData.selected
                    Column {
                        id: accountTexts
                        x: Theme.horizontalPageMargin
                        width: parent.width - 2 * x
                        anchors.verticalCenter: parent.verticalCenter
                        Label {
                            width: parent.width
                            truncationMode: TruncationMode.Fade
                            text: modelData.name
                            color: modelData.selected ? Theme.highlightColor : Theme.primaryColor
                        }
                        Label {
                            width: parent.width
                            truncationMode: TruncationMode.Fade
                            font.pixelSize: Theme.fontSizeExtraSmall
                            color: Theme.secondaryColor
                            text: modelData.brand
                                  //: Marks a brand channel in the channel list, %1 is the handle
                                  ? qsTr("Brand channel %1").arg(modelData.handle)
                                  : modelData.handle
                        }
                    }
                    onClicked: if (!modelData.selected) account.select(index)
                }
            }

            SectionHeader {
                visible: page.available && !account.hasSession
                //: Section listing the ways to sign in
                text: qsTr("Sign in")
            }
            Label {
                x: Theme.horizontalPageMargin
                width: parent.width - 2 * x
                visible: page.available && !account.hasSession
                wrapMode: Text.Wrap
                font.pixelSize: Theme.fontSizeSmall
                color: Theme.secondaryColor
                //: Explains that signing in is optional
                text: qsTr("Signing in is optional. Playback, search, the feed and downloads work without it.")
            }
            Repeater {
                model: page.available && !account.hasSession ? account.methods : []
                Column {
                    width: column.width
                    visible: modelData !== "webview"
                    spacing: Theme.paddingSmall
                    Button {
                        anchors.horizontalCenter: parent.horizontalCenter
                        text: modelData === "browser"
                              //: Sign-in method: copy the session from the Sailfish Browser
                              ? qsTr("Import from the Browser")
                              //: Sign-in method: paste cookies or load a cookies file
                              : qsTr("Paste or load cookies")
                        onClicked: page.signIn(modelData)
                    }
                    Label {
                        x: Theme.horizontalPageMargin
                        width: parent.width - 2 * x
                        wrapMode: Text.Wrap
                        font.pixelSize: Theme.fontSizeExtraSmall
                        color: Theme.secondaryColor
                        text: modelData === "browser"
                              //: Hint of the Browser import
                              ? qsTr("Sign in at youtube.com in a normal Browser tab, not a private one, then import.")
                              //: Hint of the paste import
                              : qsTr("A Cookie header, copied request headers, a \"Copy as cURL\" command or a cookies.txt file.")
                    }
                }
            }
        }
        VerticalScrollDecorator { }
    }
}
