// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
import QtQuick 2.6

// Base of every tst_*.qml: a list of step functions run by harness.cpp with the event loop
// spinning between them. A step returns nothing or the milliseconds to wait afterwards.
QtObject {
    id: base
    property var window
    property var steps: []

    function count() { return steps.length }
    function run(i) {
        try {
            var r = steps[i]()
            return typeof r === "number" ? r : 0
        } catch (e) {
            return String(e && e.message ? e.message : e) + (e && e.lineNumber ? " (line " + e.lineNumber + ")" : "")
        }
    }
    function verify(condition, message) { if (!condition) throw new Error(message || "verify failed") }
    function compare(actual, expected, message) {
        if (actual !== expected) throw new Error((message ? message + ": " : "") + "got " + actual + ", want " + expected)
    }
    // Asked by the harness before each step: is the application still loading something? Today:
    // a section of the main page (its Loader is asynchronous while it is not the current one, so
    // on a busy machine a section chosen a moment ago can still be on its way). The harness waits
    // while this is true, and while a page transition runs, for up to 10 s.
    function busy() {
        var main = window ? window.mainPage : null
        if (!main) return false
        // The section loaders sit a few levels under the main page; their content is not walked.
        var level = [main]
        for (var depth = 0; depth < 5 && level.length; ++depth) {
            var next = []
            for (var a = 0; a < level.length; ++a) {
                var item = level[a]
                if (item.hasOwnProperty("sectionId") && item.hasOwnProperty("status")) {
                    if (item.status === Loader.Loading) return true
                    continue
                }
                var kids = item.children || []
                for (var b = 0; b < kids.length; ++b) next.push(kids[b])
            }
            level = next
        }
        return false
    }
    // For a step that only waits for something to be ready: short when it is, long otherwise, so
    // a busy machine gets the time and a quiet one does not pay for it. The harness itself already
    // waits for page transitions to end before each step.
    function waitFor(ready) { return ready ? 50 : 3000 }
    function stack() { return window.pageStack }
    function current() { return window.pageStack.currentPage }
    // Breadth-first search for an item whose property `name` equals `value`.
    function find(root, name, value) {
        var queue = [root]
        while (queue.length) {
            var item = queue.shift()
            if (!item) continue
            if (item[name] === value) return item
            var kids = item.children || []
            for (var i = 0; i < kids.length; ++i) queue.push(kids[i])
            if (item.contentItem && item.contentItem !== item) queue.push(item.contentItem)
        }
        return null
    }
    function findAll(root, predicate) {
        var out = [], queue = [root]
        while (queue.length) {
            var item = queue.shift()
            if (!item) continue
            if (predicate(item)) out.push(item)
            var kids = item.children || []
            for (var i = 0; i < kids.length; ++i) queue.push(kids[i])
        }
        return out
    }
}
