// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
import QtQml 2.2
import io.thp.pyotherside 1.5

Python {
    onError: resolverHost.fail()
    Component.onCompleted: {
        addImportPath(Qt.resolvedUrl("."))
        importModule("resolver", function(success) {
            if (!success) {
                resolverHost.fail()
                return
            }
            resolverHost.ready()
            call("resolver.run", [resolverArguments], function(code) {
                if (typeof code !== "number" || code % 1 !== 0)
                    resolverHost.fail()
                else
                    resolverHost.finish(code)
            })
        })
    }
}
