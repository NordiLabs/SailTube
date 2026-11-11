// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
import QtQuick 2.6
import org.nordilabs.sailtube 1.0

// The mocks stay faithful: PlaybackSession's State enum and every mocked property exist in the
// C++ header, so a UI test can never pass against an API the device does not have.
UiTest {
    function properties(header) {
        var out = {}, re = /Q_PROPERTY\([^)]*?[\s*](\w+)\s+READ/g, m
        while ((m = re.exec(header)) !== null) out[m[1]] = true
        return out
    }
    function invokables(header) {
        var out = {}, re = /Q_INVOKABLE\s+[\w:<>]+\s*\*?\s*(\w+)\s*\(/g, m
        while ((m = re.exec(header)) !== null) out[m[1]] = true
        return out
    }
    steps: [
        function () {
            var h = harness.readFile("Clients/Sailfish/Presentation/PlaybackSession.h")
            verify(/enum State \{ Idle, Loading, Preparing, Ready, Failed \};/.test(h), "State enum unchanged")
            compare(PlaybackSession.Idle, 0); compare(PlaybackSession.Failed, 4)
            var props = properties(h), calls = invokables(h)
            // Signals: the mock declares them as QML signals, which read as functions.
            var sigBlock = /\bsignals:([\s\S]*?)(?:\n\s*(?:public|private|protected)\b|\n\};)/.exec(h)
            var sre = /void\s+(\w+)\s*\(/g, sm
            while (sigBlock && (sm = sre.exec(sigBlock[1])) !== null) calls[sm[1]] = true
            var pb = mocks.playback
            // Every mocked member must be in the header; only the mock's own bookkeeping is left out.
            var skip = { calls: true, objectName: true }
            for (var k in pb) {
                if (skip[k] || /Changed$/.test(k) || k.charAt(0) === "_") continue
                if (typeof pb[k] === "function") {
                    if (k === "log" || k === "finishLoading" || k === "fail") continue
                    verify(calls[k], "playback." + k + "() is Q_INVOKABLE in PlaybackSession.h")
                } else verify(props[k], "playback." + k + " is a Q_PROPERTY in PlaybackSession.h")
            }
            var s = harness.readFile("Clients/Sailfish/Presentation/AppSettings.h")
            verify(/Q_INVOKABLE QVariant value\(/.test(s) && /Q_INVOKABLE void setValue\(/.test(s), "AppSettings value/setValue")
            verify(/Q_PROPERTY\(int revision/.test(s), "AppSettings revision")
            var m = harness.readFile("Clients/Sailfish/Presentation/SearchModel.h")
            var sp = properties(m), sc = invokables(m)
            var names = ["query", "busy", "canLoadMore", "errorText", "count"]
            for (var i = 0; i < names.length; ++i) verify(sp[names[i]], "searchModel." + names[i])
            var fns = ["search", "loadMore", "cancel", "get"]
            for (var j = 0; j < fns.length; ++j) verify(sc[fns[j]], "searchModel." + fns[j] + "()")
        }
    ]
}
