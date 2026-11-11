// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Warm resolver worker bridge: imports worker.py next to this file and serves until stdin closes.
import QtQml 2.2
import io.thp.pyotherside 1.5

Python {
    onError: workerHost.fail()
    Component.onCompleted: {
        addImportPath(Qt.resolvedUrl("."))
        importModule("worker", function(success) {
            if (!success) {
                workerHost.fail()
                return
            }
            call("worker.serve", [], function(code) {
                workerHost.finish(typeof code === "number" && code % 1 === 0 ? code : 1)
            })
        })
    }
}
