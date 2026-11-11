// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// components/Format.js: times and compact counts with the locale's decimal separator (I18N-03).
var F = modules.Format

test("times", function () {
    eq(F.time(0), "0:00"); eq(F.time(-5), "0:00"); eq(F.time(65000), "1:05"); eq(F.time(3723000), "1:02:03")
})

test("compact counts", function () {
    eq(F.count(-1), ""); eq(F.count(999), "999"); eq(F.count(1234), "1.2K"); eq(F.count(12345), "12K")
    eq(F.count(1500000), "1.5M"); eq(F.count(2.5e9), "2.5B")
})

test("decimal separator follows the locale", function () {
    eq(F.count(1234, { decimalPoint: "," }), "1,2K")
    eq(F.count(1234, { decimalPoint: "٫" }), "1٫2K")
    eq(F.count(1234, null), "1.2K")
})
