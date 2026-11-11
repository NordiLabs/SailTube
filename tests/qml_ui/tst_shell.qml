// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
import QtQuick 2.6
import Sailfish.Silica 1.0

// App shell: main page sections load lazily, the landing section follows the setting,
// pulley menus stay within 5 items (PLT-06), Silica's orientation enum matches WatchLayout.js.
UiTest {
    id: t
    property var main
    steps: [
        function () {
            main = current()
            verify(main !== null, "main page")
            // The mocks have one subscribed channel; without any the landing would be Home
            // (Shell.landingSection, tst_shell.js).
            compare(mocks.subscriptionsModel.count, 1, "a user with a subscription")
            compare(main.current, "subscriptions", "default landing section for a user with subscriptions (DSC-04)")
            var loaders = findAll(main, function (i) { return i.hasOwnProperty("sectionId") })
            compare(loaders.length, 6, "six section loaders")
            var active = 0
            for (var i = 0; i < loaders.length; ++i) if (loaders[i].active) ++active
            compare(active, 1, "only the landing section is built before first use (NFR-PERF-01)")
        },
        function () { main.show("search"); return 400 },
        function () {
            compare(main.current, "search")
            var loaders = findAll(main, function (i) { return i.hasOwnProperty("sectionId") && i.active })
            compare(loaders.length, 2, "visited sections stay loaded")
            main.show("library"); main.show("home")
            return 400
        },
        function () {
            // docs/DESIGN.md §2.2: the bottom bar shows on the main page, switches sections, and a
            // second tap on the current one keeps it.
            verify(window.navigationShown, "bottom bar on the main page")
            var entries = findAll(window, function (i) { return i.hasOwnProperty("isCurrent") && i.hasOwnProperty("showLabel") })
            compare(entries.length, 6, "six bar entries")
            window.selectSection("subscriptions")
            compare(main.current, "subscriptions", "bar switches the section")
            window.selectSection("subscriptions")
            compare(main.current, "subscriptions", "second tap keeps the section")
            window.openVideo("v1", { title: "V" })
            return 500
        },
        // A busy machine needs longer for a page change: wait more only while it has not happened.
        function () { return waitFor(current().isWatchPage === true) },
        function () {
            verify(!window.navigationShown, "no bottom bar on the watch page")
            window.selectSection("library")
            return 500
        },
        function () { return waitFor(current().isMainPage === true) },
        function () {
            verify(current().isMainPage === true, "the bar returns to the main page")
            compare(main.current, "library", "and shows the chosen section")
            verify(window.navigationShown, "bar back")
            var menus = findAll(window, function (i) { return i.hasOwnProperty("_content") && i.hasOwnProperty("busy") && i.hasOwnProperty("quickSelect") })
            verify(menus.length >= 4, "pulley menus found: " + menus.length)
            for (var m = 0; m < menus.length; ++m) {
                var items = findAll(menus[m], function (i) { return i.hasOwnProperty("__silica_menuitem") || (i.hasOwnProperty("onClicked") && i.hasOwnProperty("text") && i.hasOwnProperty("down")) })
                var visible = 0
                for (var k = 0; k < items.length; ++k) if (items[k].visible) ++visible
                verify(visible <= 5, "pulley with " + visible + " visible items")
            }
            compare(Orientation.Portrait, 1); compare(Orientation.LandscapeMask, 10); compare(Orientation.All, 15)
        }
    ]
}
