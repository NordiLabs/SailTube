// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// The gate itself (PLT-05): ES6 is found in code and ignored in comments, strings and regexes.
function whats(src) { var v = es5Violations(src), out = []; for (var i = 0; i < v.length; ++i) out.push(v[i].what); return out }

test("rejects let, const, arrows, classes, spread, templates, for-of, generators", function () {
    deepEq(whats("let a = 1"), ["let"])
    deepEq(whats("const a = 1"), ["const"])
    deepEq(whats("var f = x => x + 1"), ["arrow function"])
    deepEq(whats("class A {}"), ["class"])
    deepEq(whats("f(...args)"), ["spread or rest"])
    deepEq(whats("var s = `a${b}`"), ["template literal"])
    deepEq(whats("for (var x of list) {}"), ["for...of"])
    deepEq(whats("function* g() {}"), ["generator"])
})

test("rejects Qt.callLater, which needs Qt 5.8", function () {
    deepEq(whats("Qt.callLater(f)"), ["Qt.callLater (Qt 5.8+)"])
})

test("ignores comments, strings and regular expressions", function () {
    deepEq(whats("// let x = 1\n/* const y => z */\nvar s = 'let `x` ...'; var t = \"=> class\""), [])
    deepEq(whats("var r = /=>|`|let/g; var q = a / b / c"), [])
})

test("accepts ES5 for-in and property names that look like keywords", function () {
    deepEq(whats("for (var k in o) {} var a = o.let; b.class = 1; c.const = 2"), [])
})

test("reports the line of a violation", function () {
    var v = es5Violations("var a\n// c\nlet b")
    eq(v.length, 1); eq(v[0].line, 3)
})
