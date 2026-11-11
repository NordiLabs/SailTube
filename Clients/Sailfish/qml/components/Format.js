// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
.pragma library

function time(ms) {
    if (!(ms > 0)) return "0:00"
    var total = Math.floor(ms / 1000)
    var h = Math.floor(total / 3600), m = Math.floor((total % 3600) / 60), s = total % 60
    var ss = s < 10 ? "0" + s : "" + s
    if (h > 0) return h + ":" + (m < 10 ? "0" + m : m) + ":" + ss
    return m + ":" + ss
}

// Compact count; units stay short and untranslated (K, M, B). The decimal separator follows
// the Qt locale when one is passed (I18N-03: formatting uses the Qt locale), e.g. 1,2K in German.
function count(n, locale) {
    if (!(n >= 0)) return ""
    var point = locale && locale.decimalPoint ? locale.decimalPoint : "."
    function fixed(v, digits) { return v.toFixed(digits).replace(".", point) }
    if (n >= 1e9) return fixed(n / 1e9, n >= 1e10 ? 0 : 1) + "B"
    if (n >= 1e6) return fixed(n / 1e6, n >= 1e7 ? 0 : 1) + "M"
    if (n >= 1e3) return fixed(n / 1e3, n >= 1e4 ? 0 : 1) + "K"
    return "" + Math.round(n)
}
