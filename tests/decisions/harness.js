// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Decision-library test harness (NFR-TST-04, PLT-05). ES5 only, so the same file runs in
// Qt 5.6's QJSEngine (the device's engine, jsrun.cpp) and in a bare Node vm context
// (node_host.js). The host provides one global object, __host, with:
//   read(path) -> string, list(dir) -> [names], print(text), evaluate(code, file) -> {ok, value, error}
//   root: repository root
// Steps: 1) reject ES6 syntax in every library and test file, ignoring comments and strings;
// 2) load every qml/logic/*.js library as a namespace; 3) run tests/decisions/tst_*.js.

var LOGIC_DIRS = ["Clients/Sailfish/qml/logic", "Clients/Sailfish/qml/components"]
var TEST_DIR = "tests/decisions"

var modules = {}
var __failures = []
var __passes = 0
var __current = ""

function __endsWith(s, suffix) { return s.length >= suffix.length && s.substr(s.length - suffix.length) === suffix }

// ---- ES5 gate ----
// Returns a list of { line, what } for ES6+ syntax. Comments, strings and regular-expression
// literals are skipped. Template literals are themselves ES6 and are reported.
function es5Violations(src) {
    var out = []
    var i = 0, n = src.length, line = 1
    var tokens = []          // { t: text, line }
    var prev = ""            // previous significant token, for regex detection
    function isIdStart(c) { return /[A-Za-z_$]/.test(c) }
    function isId(c) { return /[A-Za-z0-9_$]/.test(c) }
    function regexAllowed() {
        if (prev === "") return true
        if (/^[A-Za-z_$][\w$]*$/.test(prev))
            return /^(return|typeof|case|do|else|in|instanceof|new|delete|void|throw)$/.test(prev)
        if (/^[0-9]/.test(prev)) return false
        return !(prev === ")" || prev === "]" || prev === "}")
    }
    while (i < n) {
        var c = src.charAt(i)
        if (c === "\n") { ++line; ++i; continue }
        if (c === " " || c === "\t" || c === "\r") { ++i; continue }
        if (c === "/" && src.charAt(i + 1) === "/") { while (i < n && src.charAt(i) !== "\n") ++i; continue }
        if (c === "/" && src.charAt(i + 1) === "*") {
            i += 2
            while (i < n && !(src.charAt(i) === "*" && src.charAt(i + 1) === "/")) { if (src.charAt(i) === "\n") ++line; ++i }
            i += 2; continue
        }
        if (c === "'" || c === "\"") {
            var q = c; ++i
            while (i < n && src.charAt(i) !== q) { if (src.charAt(i) === "\\") ++i; else if (src.charAt(i) === "\n") ++line; ++i }
            ++i; prev = "str"; continue
        }
        if (c === "`") { out.push({ line: line, what: "template literal" }); ++i; while (i < n && src.charAt(i) !== "`") { if (src.charAt(i) === "\n") ++line; ++i } ++i; prev = "str"; continue }
        if (c === "/" && regexAllowed()) {
            ++i
            var inClass = false
            while (i < n) {
                var d = src.charAt(i)
                if (d === "\\") { i += 2; continue }
                if (d === "\n") break
                if (d === "[") inClass = true
                else if (d === "]") inClass = false
                else if (d === "/" && !inClass) break
                ++i
            }
            ++i
            while (i < n && isId(src.charAt(i))) ++i
            prev = "regex"; continue
        }
        if (isIdStart(c)) {
            var s = i
            while (i < n && isId(src.charAt(i))) ++i
            var w = src.substring(s, i)
            tokens.push({ t: w, line: line }); prev = w; continue
        }
        if (/[0-9]/.test(c)) { var s2 = i; while (i < n && /[0-9A-Za-z_.]/.test(src.charAt(i))) ++i; prev = src.substring(s2, i); continue }
        var three = src.substr(i, 3), two = src.substr(i, 2)
        if (three === "...") { tokens.push({ t: "...", line: line }); prev = "..."; i += 3; continue }
        if (two === "=>") { tokens.push({ t: "=>", line: line }); prev = "=>"; i += 2; continue }
        tokens.push({ t: c, line: line }); prev = c; ++i
    }
    var banned = { "let": "let", "const": "const", "class": "class", "=>": "arrow function",
                   "...": "spread or rest", "async": "async", "await": "await", "yield": "yield",
                   "import": "import", "export": "export" }
    for (var k = 0; k < tokens.length; ++k) {
        var tok = tokens[k]
        var before = k > 0 ? tokens[k - 1].t : ""
        if (banned.hasOwnProperty(tok.t) && before !== ".") out.push({ line: tok.line, what: banned[tok.t] })
        if (tok.t === "function" && k + 1 < tokens.length && tokens[k + 1].t === "*") out.push({ line: tok.line, what: "generator" })
        if (tok.t === "callLater" && before === "." && k >= 2 && tokens[k - 2].t === "Qt") out.push({ line: tok.line, what: "Qt.callLater (Qt 5.8+)" })
        if (tok.t === "for" && k + 1 < tokens.length && tokens[k + 1].t === "(") {
            // for (... of ...) at depth 1 of the parenthesis
            var depth = 0
            for (var m = k + 1; m < tokens.length; ++m) {
                if (tokens[m].t === "(") ++depth
                else if (tokens[m].t === ")") { if (--depth === 0) break }
                else if (depth === 1 && tokens[m].t === "of") { out.push({ line: tokens[m].line, what: "for...of" }); break }
                else if (depth === 1 && tokens[m].t === ";") break
            }
        }
    }
    return out
}

// ---- module loading ----
function __namespace(name, src, file) {
    var names = []
    var re = /^(?:function\s+([A-Za-z_$][\w$]*)|var\s+([A-Za-z_$][\w$]*))/mg
    var m
    while ((m = re.exec(src)) !== null) names.push(m[1] || m[2])
    var lines = src.split("\n")
    var prologue = []
    for (var i = 0; i < lines.length; ++i) {
        var imp = /^\s*\.import\s+"([^"]+)\.js"\s+as\s+([A-Za-z_$][\w$]*)/.exec(lines[i])
        if (imp) { prologue.push("var " + imp[2] + " = modules." + imp[1].split("/").pop() + ";"); lines[i] = "" }
        else if (/^\s*\.pragma\s+library/.test(lines[i]) || /^\s*\.import\s/.test(lines[i])) lines[i] = ""
    }
    var ret = []
    for (var j = 0; j < names.length; ++j) ret.push(names[j] + ": " + names[j])
    var code = "(function (modules) {" + prologue.join(" ") + lines.join("\n") +
               "\nreturn {" + ret.join(", ") + "};\n})"
    var r = __host.evaluate(code, file)
    if (!r.ok) throw new Error("cannot load " + file + ": " + r.error)
    return r.value(modules)
}

function __gate(path) {
    var v = es5Violations(__host.read(__host.root + "/" + path))
    for (var i = 0; i < v.length; ++i) __failures.push("ES5 gate: " + path + ":" + v[i].line + ": " + v[i].what)
    return v.length === 0
}

// ---- assertions ----
// Canonical JSON: object keys sorted, because engines differ in property order.
function __canon(v) {
    if (v === null || typeof v !== "object") return v
    if (v instanceof Array) { var a = []; for (var i = 0; i < v.length; ++i) a.push(__canon(v[i])); return a }
    var keys = [], o = {}
    for (var k in v) if (v.hasOwnProperty(k)) keys.push(k)
    keys.sort()
    for (var j = 0; j < keys.length; ++j) o[keys[j]] = __canon(v[keys[j]])
    return o
}
function __fmt(v) { try { return JSON.stringify(__canon(v)) } catch (e) { return String(v) } }
function AssertionError(message) { this.message = message }
function ok(value, message) { if (!value) throw new AssertionError(message || "expected a true value") }
function eq(actual, expected, message) {
    if (actual !== expected) throw new AssertionError((message ? message + ": " : "") + "got " + __fmt(actual) + ", want " + __fmt(expected))
}
function deepEq(actual, expected, message) {
    if (__fmt(actual) !== __fmt(expected)) throw new AssertionError((message ? message + ": " : "") + "got " + __fmt(actual) + ", want " + __fmt(expected))
}
function test(name, fn) {
    try { fn(); ++__passes }
    catch (e) { __failures.push(__current + ": " + name + ": " + (e && e.message ? e.message : String(e))) }
}

function __run() {
    var libs = []
    for (var d = 0; d < LOGIC_DIRS.length; ++d) {
        var names = __host.list(__host.root + "/" + LOGIC_DIRS[d])
        for (var i = 0; i < names.length; ++i) if (__endsWith(names[i], ".js")) libs.push(LOGIC_DIRS[d] + "/" + names[i])
    }
    var tests = []
    var tnames = __host.list(__host.root + "/" + TEST_DIR)
    for (var t = 0; t < tnames.length; ++t) if (/^tst_.*\.js$/.test(tnames[t])) tests.push(TEST_DIR + "/" + tnames[t])

    var gated = true
    for (var a = 0; a < libs.length; ++a) gated = __gate(libs[a]) && gated
    for (var b = 0; b < tests.length; ++b) gated = __gate(tests[b]) && gated
    gated = __gate(TEST_DIR + "/harness.js") && gated
    if (gated) {
        // Libraries may import each other; load in dependency-free order, retrying once.
        var pending = libs.slice(0)
        for (var pass = 0; pass < 2 && pending.length; ++pass) {
            var next = []
            for (var p = 0; p < pending.length; ++p) {
                var file = pending[p]
                var name = file.split("/").pop().replace(/\.js$/, "")
                try { modules[name] = __namespace(name, __host.read(__host.root + "/" + file), file) }
                catch (e) { if (pass === 1) __failures.push(String(e && e.message ? e.message : e)); else next.push(file) }
            }
            pending = next
        }
        for (var c = 0; c < tests.length; ++c) {
            __current = tests[c]
            var r = __host.evaluate(__host.read(__host.root + "/" + tests[c]), tests[c])
            if (!r.ok) __failures.push(tests[c] + ": " + r.error)
        }
    }
    for (var f = 0; f < __failures.length; ++f) __host.print("FAIL " + __failures[f])
    __host.print("decisions: " + __passes + " passed, " + __failures.length + " failed, " +
                 libs.length + " libraries, " + tests.length + " test files")
    return __failures.length === 0
}
