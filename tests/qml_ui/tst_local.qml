// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
import QtQuick 2.6
import Sailfish.Silica 1.0

// PLY-26: while a downloaded file plays, the watch page says so and offers "Stream instead",
// the session's own switch to the stream from the current position; the button is off while a
// switch is already under way.
UiTest {
    id: t
    property var pb: mocks.playback
    function visibleText(text) {
        return findAll(current(), function (i) { return i.visible && i.text === text })
    }
    steps: [
        function () { window.openVideo("d1", { title: "Saved" }); return 500 },
        function () {
            compare(visibleText("Playing downloaded file").length, 0, "no note while streaming")
            pb.finishLoading(true)
            pb.position = 42000
            pb.diagnostics = { local: true }
            pb.canStreamInstead = true
            pb.calls = []
            return 200
        },
        function () {
            compare(visibleText("Playing downloaded file").length, 1, "note while the file plays")
            // The Button, not the Label inside it.
            var b = visibleText("Stream instead").filter(function (i) { return typeof i.clicked === "function" })
            compare(b.length, 1, "Stream instead button")
            verify(b[0].enabled, "the button is on while the session can switch")
            b[0].clicked(null)
            compare(pb.calls.join(","), "streamInstead", "the session switches to the stream itself")
            // While the switch is under way the note stays and the button is off.
            pb.canStreamInstead = false
            verify(!b[0].enabled, "no second switch while one is under way")
            compare(visibleText("Playing downloaded file").length, 1, "the note stays while the file still plays")
            pb.diagnostics = { local: false }
            // A failed switch: the file keeps playing (Ready) and the message shows under the picture.
            pb.state = 3
            pb.errorText = "Streaming is not available right now."
            return 100
        },
        function () {
            compare(visibleText("Streaming is not available right now.").length, 1, "non-fatal message while playing")
            pb.errorText = ""
            // SEL-08: a listen-only failure offers "Play video", not a settings link.
            pb.calls = []
            pb.errorSettingKey = "playback/audioOnly"
            pb.fail("Only video is available")
            return 200
        },
        function () {
            compare(visibleText("Open setting").length, 0, "no settings link for a session mode")
            var b = visibleText("Play video").filter(function (i) { return typeof i.clicked === "function" && !i.hasOwnProperty("menu") })
            verify(b.length >= 1, "Play video on the error")
            b[0].clicked(null)
            verify(pb.calls.indexOf("audioOnly:false") >= 0 && pb.calls.indexOf("retry") >= 0, "switch to video and retry: " + pb.calls.join(","))
            pb.errorSettingKey = ""
            stack().pop(undefined, PageStackAction.Immediate)
            pb.stop()
        }
    ]
}
