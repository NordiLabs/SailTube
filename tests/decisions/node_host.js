// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Runs harness.js under Node in a bare vm context (NFR-TST-04). Host-side only: Node is not
// in the build container, where jsrun.cpp runs the same harness in Qt 5.6's engine.
"use strict";
const fs = require("fs");
const path = require("path");
const vm = require("vm");
const root = path.resolve(process.argv[2] || path.join(__dirname, "..", ".."));
const context = vm.createContext({});
context.__host = {
    root: root,
    read: (p) => { try { return fs.readFileSync(p, "utf8"); } catch (e) { return ""; } },
    list: (d) => { try { return fs.readdirSync(d).filter((n) => fs.statSync(path.join(d, n)).isFile()).sort(); } catch (e) { return []; } },
    print: (t) => console.log(t),
    evaluate: (code, file) => {
        try { return { ok: true, value: vm.runInContext(code, context, { filename: file }) }; }
        catch (e) { return { ok: false, error: String(e && e.stack ? e.stack.split("\n").slice(0, 2).join(" ") : e) }; }
    },
};
vm.runInContext(fs.readFileSync(path.join(root, "tests/decisions/harness.js"), "utf8"), context, { filename: "harness.js" });
const passed = vm.runInContext("__run()", context);
console.log("engine: node " + process.version + " (bare vm context)");
process.exit(passed ? 0 : 1);
