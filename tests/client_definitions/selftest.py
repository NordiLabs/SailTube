# SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
"""Seeded scratch tests of tools/check_client_definitions.py (NFR-MNT-04).

    python3 -I -B tests/client_definitions/selftest.py --root REPO --tmp SCRATCH_DIR

Synthetic trees prove each rule (stray, unowned, unfixtured, shrink-only waivers, comments ignored,
.gz fixtures, tests/client_definitions/ never counting). Then a scratch copy of the real tree
changes one client version in its definitions file and "re-records" the matching fixture: that
alone must keep the check green, while the definitions edit without the fixture must fail.
"""
import argparse
import gzip
import importlib.util
import io
import json
import os
import re
import shutil
import sys
from contextlib import redirect_stdout

sys.dont_write_bytecode = True
failures = []


def check(cond, what):
    print("%s %s" % ("ok  " if cond else "FAIL", what))
    if not cond:
        failures.append(what)


def load_checker(root):
    spec = importlib.util.spec_from_file_location("check_client_definitions",
                                                  os.path.join(root, "tools", "check_client_definitions.py"))
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


def write(path, text):
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, "w", encoding="utf-8") as f:
        f.write(text)


def run(cd, root):
    buf = io.StringIO()
    with redirect_stdout(buf):
        rc = cd.main(["--root", root])
    return rc, buf.getvalue()


CONFIG = {
    "scan": {"roots": ["Core", "Clients", "Shared"], "exclude": ["Shared/Upstream/tests"]},
    "fixtures": {"roots": ["tests", "Shared/Upstream/tests"], "exclude": ["tests/client_definitions"]},
    "components": [
        {"name": "alpha", "root": "Core/Alpha", "definitions": "Core/Alpha/Infrastructure/Definitions.cpp", "owner": "alpha"},
        {"name": "upstream", "root": "Shared/Upstream/Core", "definitions": "Shared/Upstream/Core/Client.h",
         "owner": "upstream", "pinned": True},
    ],
}

DEFS = '''// Alpha client definitions. A comment that names "ANDROID_VR" or 9.20990101.01.00 is no site.
/* Neither is a block comment with "youtubei/v1/guide" in it. */
const char *kName = "WEB";
const char *kVersion = "2.20260101.01.00";
const char *kPlayer = "https://www.youtube.com/youtubei/v1/player?prettyPrint=false";
const char *kHeader = "X-YouTube-Client-Version";
'''
SERVICE = '''// Uses the definitions only.
#include "Definitions.h"
int answer() { return 42; }   // "WEB" in a comment is fine
'''
UPSTREAM = '''inline std::string body() { return "{\\"context\\":{\\"client\\":{\\"clientName\\":\\"VISIONOS\\",\\"clientVersion\\":\\"1.02\\"}}}"; }
'''
FIXTURE = {"request": {"endpoint": "player", "url": "/youtubei/v1/player?prettyPrint=false",
                       "headers": {"X-YouTube-Client-Version": "2.20260101.01.00"},
                       "body": {"context": {"client": {"clientName": "WEB", "clientVersion": "2.20260101.01.00"}}}}}
UP_TEST = 'CHECK(has(body(), "\\"clientName\\":\\"VISIONOS\\",\\"clientVersion\\":\\"1.02\\""));\n'


def synthetic(base, name):
    root = os.path.join(base, name)
    write(os.path.join(root, "tests/client_definitions/components.json"), json.dumps(CONFIG))
    write(os.path.join(root, "Core/Alpha/Infrastructure/Definitions.cpp"), DEFS)
    write(os.path.join(root, "Core/Alpha/Infrastructure/Service.cpp"), SERVICE)
    write(os.path.join(root, "Shared/Upstream/Core/Client.h"), UPSTREAM)
    write(os.path.join(root, "Shared/Upstream/tests/ClientTest.cpp"), UP_TEST)
    write(os.path.join(root, "tests/alpha/fixtures/player.json"), json.dumps(FIXTURE))
    return root


def edit(root, rel, old, new):
    p = os.path.join(root, rel)
    with open(p, encoding="utf-8") as f:
        text = f.read()
    assert old in text, (rel, old)
    write(p, text.replace(old, new))


def append(root, rel, text):
    with open(os.path.join(root, rel), "a", encoding="utf-8") as f:
        f.write(text)


def waive(root, strays=(), unfixtured=()):
    write(os.path.join(root, "tests/client_definitions/pending.json"),
          json.dumps({"strays": list(strays), "unfixtured": list(unfixtured)}))


def synthetic_cases(cd, tmp):
    r = synthetic(tmp, "baseline")
    rc, out = run(cd, r)
    check(rc == 0 and "check_client_definitions: PASS" in out, "baseline tree passes (comments are not sites)")

    # A new client version literal outside the definitions file fails.
    r = synthetic(tmp, "stray_version")
    append(r, "Core/Alpha/Infrastructure/Service.cpp", 'const char *v = "2.20270101.01.00";\n')
    rc, out = run(cd, r)
    check(rc == 1 and 'FAIL stray Core/Alpha/Infrastructure/Service.cpp:4 client-version "2.20270101.01.00"' in out,
          "seeded: new client version outside the definitions file fails")

    r = synthetic(tmp, "stray_endpoint")
    append(r, "Core/Alpha/Infrastructure/Service.cpp",
           'void f() { client_->post<Page>(QStringLiteral("browse"), body, this, parse, done); }\n'
           'const char *p = "EgZ2aWRlb3PyBgQKAjoA";\n')
    rc, out = run(cd, r)
    check(rc == 1 and 'endpoint "youtubei/v1/browse"' in out and 'params "EgZ2aWRlb3PyBgQKAjoA"' in out,
          "seeded: endpoint name and params token outside the definitions file fail")

    r = synthetic(tmp, "stray_py_qml")
    write(os.path.join(r, "Core/Alpha/Runtime/tool.py"), '# "TVHTML5" in a comment\nCLIENT = {"clientName": "IOS"}\n')
    write(os.path.join(r, "Clients/App/qml/Page.qml"), 'Item { property string c: "ANDROID_VR" } // "MWEB"\n')
    rc, out = run(cd, r)
    check(rc == 1 and 'FAIL stray Core/Alpha/Runtime/tool.py:2 client-name "IOS"' in out and "TVHTML5" not in out,
          "seeded: a client name in Python is a stray, its comment is not")
    check('FAIL unowned Clients/App/qml/Page.qml:1 client-name "ANDROID_VR"' in out and "MWEB" not in out,
          "seeded: a site outside every component fails as unowned")

    # A definitions value without a fixture fails; tests/client_definitions/ never counts as one.
    r = synthetic(tmp, "unfixtured")
    append(r, "Core/Alpha/Infrastructure/Definitions.cpp", 'const char *kTab = "EgIQAQ%3D%3D";\n')
    write(os.path.join(r, "tests/client_definitions/notes.txt"), "EgIQAQ%3D%3D\n")
    rc, out = run(cd, r)
    check(rc == 1 and 'FAIL unfixtured Core/Alpha/Infrastructure/Definitions.cpp:7 params "EgIQAQ%3D%3D"' in out,
          "seeded: a definitions value with no fixture fails (tests/client_definitions does not count)")
    with gzip.open(os.path.join(r, "tests/alpha/fixtures/search.json.gz"), "wt") as f:
        f.write('{"request": {"endpoint": "search", "body": {"params": "EgIQAQ%3D%3D"}}}')
    rc, out = run(cd, r)
    check(rc == 0, "a recorded .gz fixture satisfies it")

    # The update procedure: change the version in the definitions file and re-record the fixture.
    r = synthetic(tmp, "bump")
    edit(r, "Core/Alpha/Infrastructure/Definitions.cpp", "2.20260101.01.00", "2.20261201.01.00")
    rc, out = run(cd, r)
    check(rc == 1 and 'FAIL unfixtured Core/Alpha/Infrastructure/Definitions.cpp:4 client-version "2.20261201.01.00"' in out,
          "seeded: a version bump without its fixture fails")
    edit(r, "tests/alpha/fixtures/player.json", "2.20260101.01.00", "2.20261201.01.00")
    rc, out = run(cd, r)
    check(rc == 0, "seeded: version bump in the definitions file + re-recorded fixture passes")

    r = synthetic(tmp, "bump_pinned")
    edit(r, "Shared/Upstream/Core/Client.h", '\\"1.02\\"', '\\"1.03\\"')
    rc, out = run(cd, r)
    check(rc == 1 and 'client-version "1.03"' in out, "pinned component: bump without fixture fails")
    edit(r, "Shared/Upstream/tests/ClientTest.cpp", '\\"1.02\\"', '\\"1.03\\"')
    rc, out = run(cd, r)
    check(rc == 0, "pinned component: bump + its own test fixture passes")

    # Shrink-only waivers.
    stray = {"owner": "alpha", "component": "alpha", "file": "Core/Alpha/Infrastructure/Service.cpp",
             "kind": "client-version", "value": "2.20270101.01.00"}
    r = synthetic(tmp, "waived")
    append(r, "Core/Alpha/Infrastructure/Service.cpp", 'const char *v = "2.20270101.01.00";\n')
    waive(r, [stray])
    rc, out = run(cd, r)
    check(rc == 0 and "WAIVED stray Core/Alpha/Infrastructure/Service.cpp:4" in out, "a waived stray passes")
    append(r, "Core/Alpha/Infrastructure/Service.cpp", 'const char *w = "2.20270101.01.00";\n')
    rc, out = run(cd, r)
    check(rc == 1 and "FAIL stray Core/Alpha/Infrastructure/Service.cpp:5" in out,
          "a second site of a waived value (sites=1) fails")
    waive(r, [dict(stray, sites=3)])
    rc, out = run(cd, r)
    check(rc == 1 and "2 of 3 sites left" in out, "a waiver larger than what is left fails (shrink-only)")
    r = synthetic(tmp, "stale")
    waive(r, [stray], [{"owner": "alpha", "file": "Core/Alpha/Infrastructure/Definitions.cpp", "kind": "client-name",
                        "value": "WEB"}])
    rc, out = run(cd, r)
    check(rc == 1 and "FAIL stale waiver strays[0]" in out and "FAIL stale waiver unfixtured[0]" in out,
          "waivers that no longer match fail (shrink-only)")

    r = synthetic(tmp, "config")
    edit(r, "tests/client_definitions/components.json", "Definitions.cpp", "Missing.cpp")
    rc, out = run(cd, r)
    check(rc == 1 and "FAIL config: definitions file Core/Alpha/Infrastructure/Missing.cpp" in out,
          "a missing definitions file fails")


def copy_tree(src, dst, tops):
    for top in tops:
        s = os.path.join(src, top)
        if os.path.isdir(s):
            shutil.copytree(s, os.path.join(dst, top), symlinks=True,
                            ignore=shutil.ignore_patterns("__pycache__", ".tmp", "*.o"))


def real_scratch(cd, root, tmp):
    """In a scratch copy: one version changed in a definitions file, its fixture re-recorded."""
    scratch = os.path.join(tmp, "repo")
    copy_tree(root, scratch, ("Core", "Clients", "Shared", "tests", "tools"))
    buf = io.StringIO()
    with redirect_stdout(buf):
        rc = cd.main(["--root", scratch, "--json"])
    result = json.loads(buf.getvalue())
    check(rc == 0, "scratch copy of the real tree passes before the change")
    config = json.load(open(os.path.join(scratch, "tests/client_definitions/components.json")))
    pinned = {c["name"] for c in config["components"] if c.get("pinned")}
    versions = [e for e in result["inventory"] if e["kind"] == "client-version" and e["fixture"]]
    versions.sort(key=lambda e: e["component"] in pinned)   # prefer a SailTube-owned definitions file
    if not versions:
        check(False, "the real tree has a fixtured client version in a definitions file")
        return
    e = versions[0]
    old = e["value"]
    m = re.fullmatch(r"(\d+\.)(\d{8})(\..*)", old)
    new = m.group(1) + "20991231" + m.group(3) if m else old + "9"
    corpus = cd.Corpus(scratch, config["fixtures"]["roots"], config["fixtures"].get("exclude", []))
    check(corpus.find("client-version", new) is None, "scratch: the new version %s is in no fixture yet" % new)
    # The same standalone-value rule the checker applies (a short "1.02" never matches "1.02M views").
    if len(old) < 8:
        lit = re.compile(r"(?<=[\"'`=])" + re.escape(old) + r"(?=[\"'`\\&,;)]|$)", re.M)
    else:
        lit = re.compile(r"(?<![A-Za-z0-9_.])" + re.escape(old) + r"(?![A-Za-z0-9_])")
    path = os.path.join(scratch, e["file"])
    lines = open(path, encoding="utf-8").read().split("\n")
    lines[e["line"] - 1] = lit.sub(new, lines[e["line"] - 1])
    write(path, "\n".join(lines))
    rc, out = run(cd, scratch)
    check(rc == 1 and ('FAIL unfixtured %s:%d client-version "%s"' % (e["file"], e["line"], new)) in out,
          "scratch: %s %s -> %s in %s without its fixture fails" % (e["component"], old, new, e["file"]))
    # "Re-record": every fixture that carried the old value now carries the new one.
    rerecorded = []
    for rel, text in corpus.files:
        if lit.search(text) and not rel.endswith(".gz"):
            write(os.path.join(scratch, rel), lit.sub(new, text))
            rerecorded.append(rel)
    rc, out = run(cd, scratch)
    check(rc == 0, "scratch: the definitions edit + re-recorded fixture(s) %s pass" % ", ".join(rerecorded))
    if rc:
        print(out)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--root", required=True)
    ap.add_argument("--tmp", required=True)
    args = ap.parse_args()
    cd = load_checker(os.path.abspath(args.root))
    synthetic_cases(cd, os.path.join(args.tmp, "synthetic"))
    real_scratch(cd, os.path.abspath(args.root), args.tmp)
    if failures:
        print("client_definitions selftest: %d FAILED" % len(failures))
        return 1
    print("client_definitions selftest: PASS")
    return 0


if __name__ == "__main__":
    sys.exit(main())
