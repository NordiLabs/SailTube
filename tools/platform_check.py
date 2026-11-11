#!/usr/bin/env python3
# SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
"""Static platform and invariant checks (PLT-03, PLT-04, NFR-TST-03). Python 3.9 stdlib only.

Named checks:
  sailjail_names  PLT-03  desktop entry [X-Sailjail] names == names set in main.cpp before the app object
  paths           PLT-03  no data/cache/config path outside the Sailjail layout in shipped code
  permissions     PLT-04  permission audit: every declared permission maps to a feature flag and code
  cache_key       NFR-TST-03 (1)  the resolve cache key covers every output-affecting setting
  ui_defaults     NFR-TST-03 (2)  default literals in the UI match the defaults table
  sponsorblock    NFR-TST-03 (3)  every SponsorBlock category the engine fetches has a UI row
(NFR-TST-03 (4), "every permission maps to a feature", is the permissions check.)

Configuration lives in tests/platform/{permissions,output_settings,path_exceptions}.json under --root,
so a scratch copy of the tree carries its own declarations. See docs/PLATFORM_CHECKS.md.

  python3 -I tools/platform_check.py [--root DIR] [--desktop FILE] [CHECK ...]
  python3 -I tools/platform_check.py --write-forced-sandbox-desktop OUT [--desktop FILE]
"""
import argparse
import fnmatch
import io
import json
import os
import re
import sys
import tokenize

CHECKS = ["sailjail_names", "paths", "permissions", "cache_key", "ui_defaults", "sponsorblock"]

# PLT-04, verbatim: STORE Internet, Audio; optional Videos, Music, Downloads, RemovableMedia,
# MediaIndexing, Secrets, WebView; FULL (sandboxed) the same list.
PLT04_PERMISSIONS = ["Internet", "Audio", "Videos", "Music", "Downloads", "RemovableMedia",
                     "MediaIndexing", "Secrets", "WebView"]
VARIANTS = ["FULL", "STORE"]

SCAN_ROOTS = ["Core", "Clients", "Shared"]
SCAN_EXCLUDE = ["Shared/SailTune/tests"]
CODE_EXT = (".h", ".hpp", ".cpp", ".cc", ".qml", ".js", ".py")
WALK_SKIP_DIRS = {".git", "RPMS", "build", "vendor", "references-gpl", "node_modules", "tests"}

SETTING_KEY_RE = re.compile(r"^[A-Za-z]+/[A-Za-z0-9_]+$")


# ----------------------------------------------------------------------------- utilities

def read(path):
    with open(path, encoding="utf-8", errors="replace") as f:
        return f.read()


def rel(root, path):
    return os.path.relpath(path, root).replace(os.sep, "/")


def line_of(text, pos):
    return text.count("\n", 0, pos) + 1


def strip_comments_c(text):
    """C/C++/QML/JS: blank out comments, keep strings and every newline (offsets stay valid)."""
    out = []
    i, n = 0, len(text)
    while i < n:
        c = text[i]
        if c == "/" and i + 1 < n and text[i + 1] == "/":
            j = text.find("\n", i)
            j = n if j < 0 else j
            out.append(" " * (j - i))
            i = j
        elif c == "/" and i + 1 < n and text[i + 1] == "*":
            j = text.find("*/", i + 2)
            j = n if j < 0 else j + 2
            out.append("".join("\n" if ch == "\n" else " " for ch in text[i:j]))
            i = j
        elif c in "\"'":
            j = i + 1
            while j < n and text[j] != c and text[j] != "\n":
                j += 2 if text[j] == "\\" else 1
            j = min(j + 1, n)
            out.append(text[i:j])
            i = j
        else:
            out.append(c)
            i += 1
    return "".join(out)


def strip_comments_py(text):
    lines = text.splitlines(True)
    try:
        tokens = list(tokenize.generate_tokens(io.StringIO(text).readline))
    except (tokenize.TokenError, IndentationError, SyntaxError):
        return text
    for tok in tokens:
        if tok.type == tokenize.COMMENT:
            (row, col), (_, end) = tok.start, tok.end
            s = lines[row - 1]
            lines[row - 1] = s[:col] + " " * (end - col) + s[end:]
    return "".join(lines)


def clean_source(path, text):
    return strip_comments_py(text) if path.endswith(".py") else strip_comments_c(text)


def matching(text, open_pos):
    """Index just past the bracket closing the one at open_pos (strings and comments already safe)."""
    pairs = {"{": "}", "(": ")", "[": "]"}
    opener = text[open_pos]
    closer = pairs[opener]
    depth = 0
    i, n = open_pos, len(text)
    while i < n:
        c = text[i]
        if c in "\"'":
            j = i + 1
            while j < n and text[j] != c and text[j] != "\n":
                j += 2 if text[j] == "\\" else 1
            i = j + 1
            continue
        if c == opener:
            depth += 1
        elif c == closer:
            depth -= 1
            if depth == 0:
                return i + 1
        i += 1
    return n


def call_args(text, open_pos):
    """Top-level arguments of the call whose '(' is at open_pos, and the end index."""
    end = matching(text, open_pos)
    inner = text[open_pos + 1:end - 1]
    args, depth, cur, i = [], 0, [], 0
    while i < len(inner):
        c = inner[i]
        if c in "\"'":
            j = i + 1
            while j < len(inner) and inner[j] != c:
                j += 2 if inner[j] == "\\" else 1
            cur.append(inner[i:j + 1])
            i = j + 1
            continue
        if c in "([{":
            depth += 1
        elif c in ")]}":
            depth -= 1
        if c == "," and depth == 0:
            args.append("".join(cur).strip())
            cur = []
        else:
            cur.append(c)
        i += 1
    if "".join(cur).strip():
        args.append("".join(cur).strip())
    return args, end


WRAPPER_RE = re.compile(r"^(?:QStringLiteral|QLatin1String|QString(?:::fromLatin1|::fromUtf8)?|QVariant|"
                        r"QByteArrayLiteral|std::string)\s*\((.*)\)$", re.S)
NUMBER_RE = re.compile(r"^[-+]?(?:\d+\.?\d*|\.\d+)(?:[eE][-+]?\d+)?[fF]?$")


def literal_value(arg):
    """('text'|'bool'|'number', value) for a plain literal, None for an expression."""
    s = arg.strip()
    m = WRAPPER_RE.match(s)
    while m:
        s = m.group(1).strip()
        m = WRAPPER_RE.match(s)
    if len(s) >= 2 and s[0] == s[-1] and s[0] in "\"'" and s.count(s[0]) == 2:
        return ("text", s[1:-1])
    if s in ("true", "false"):
        return ("bool", s)
    if NUMBER_RE.match(s):
        return ("number", float(s.rstrip("fF")))
    return None


def canonical(kind, value):
    if kind == "bool":
        return "b:" + str(value).lower()
    if kind == "number":
        return "n:%g" % float(value)
    text = str(value)
    if text in ("true", "false"):
        return "b:" + text
    if NUMBER_RE.match(text):
        return "n:%g" % float(text.rstrip("fF"))
    return "s:" + text


def show(kind, value):
    if kind == "number":
        return "%g" % float(value)
    if kind == "text":
        return '"%s"' % value
    return str(value)


def code_files(root):
    out = []
    for top in SCAN_ROOTS:
        base = os.path.join(root, top)
        for dirpath, dirnames, filenames in os.walk(base):
            r = rel(root, dirpath)
            dirnames[:] = sorted(d for d in dirnames
                                 if not any((r + "/" + d).startswith(x) for x in SCAN_EXCLUDE))
            for name in sorted(filenames):
                if name.endswith(CODE_EXT):
                    out.append(os.path.join(dirpath, name))
    return out


def glob_files(root, pattern):
    """Root-relative glob; '**' spans directories."""
    regex = re.escape(pattern).replace(r"\*\*/", "(?:.*/)?").replace(r"\*\*", ".*").replace(r"\*", "[^/]*")
    regex = re.compile("^" + regex + "$")
    found = []
    for dirpath, dirnames, filenames in os.walk(root):
        dirnames[:] = [d for d in dirnames if d not in (".git", "RPMS")]
        for name in filenames:
            p = os.path.join(dirpath, name)
            if regex.match(rel(root, p)):
                found.append(p)
    return sorted(found)


def load_json(root, name):
    path = os.path.join(root, "tests", "platform", name)
    with open(path, encoding="utf-8") as f:
        return json.load(f)


def parse_desktop(path):
    sections, current = {}, None
    for raw in read(path).splitlines():
        line = raw.strip()
        if not line or line.startswith("#"):
            continue
        if line.startswith("[") and line.endswith("]"):
            current = line[1:-1]
            sections.setdefault(current, {})
        elif "=" in line and current is not None:
            k, v = line.split("=", 1)
            sections[current][k.strip()] = v.strip()
    return sections


def split_list(value):
    return [p.strip() for p in value.split(";") if p.strip()]


class Report:
    def __init__(self, name):
        self.name = name
        self.errors, self.notes = [], []

    def fail(self, msg):
        self.errors.append(msg)

    def note(self, msg):
        self.notes.append(msg)

    def emit(self):
        status = "PASS" if not self.errors else "FAIL"
        print("platform_check: %s: %s" % (self.name, status))
        for n in self.notes:
            print("  note: " + n)
        for e in self.errors:
            print("  FAIL: " + e)
        return not self.errors


def function_bodies(text):
    """(name, body_start, body_end) for plain function definitions in cleaned C++ text."""
    out = []
    for m in re.finditer(r"\b([A-Za-z_]\w*(?:::\w+)*)\s*\([^;{}()]*(?:\([^;{}()]*\)[^;{}()]*)*\)\s*(?:const\s*)?\{", text):
        name = m.group(1)
        if name in ("if", "for", "while", "switch", "catch", "return", "sizeof"):
            continue
        start = m.end() - 1
        out.append((name, start, matching(text, start)))
    return out


# ----------------------------------------------------------------------------- shared tree facts

class Tree:
    def __init__(self, root, desktop_override=None):
        self.root = root
        self.desktop_override = desktop_override
        self.permissions = load_json(root, "permissions.json")
        names = self.permissions.get("sailjail", {})
        self.org = names.get("organization", "")
        self.app = names.get("application", "")
        self._clean = {}

    def path(self, relpath):
        return os.path.join(self.root, relpath)

    def cleaned(self, path):
        if path not in self._clean:
            self._clean[path] = clean_source(path, read(path))
        return self._clean[path]

    def desktop_files(self):
        if self.desktop_override:
            return {v: self.desktop_override for v in VARIANTS}
        files = self.permissions.get("desktop_files", {})
        return {v: self.path(files.get(v, "harbour-sailtube.desktop")) for v in VARIANTS}

    def main_cpp(self):
        return self.path("Clients/Sailfish/Composition/main.cpp")

    def tmp_redirect(self):
        """(ok, detail): main.cpp moves TMPDIR under CacheLocation before the application object."""
        path = self.main_cpp()
        if not os.path.isfile(path):
            return False, "Clients/Sailfish/Composition/main.cpp missing"
        text = self.cleaned(path)
        put = re.search(r'qputenv\s*\(\s*"TMPDIR"', text)
        if not put:
            return False, "main.cpp does not set TMPDIR"
        bodies = function_bodies(text)
        owner = [b for b in bodies if b[1] < put.start() < b[2]]
        if not owner:
            return False, "TMPDIR is not set inside a function"
        name, s, e = min(owner, key=lambda b: b[2] - b[1])
        if "CacheLocation" not in text[s:e]:
            return False, "TMPDIR is not set under CacheLocation"
        main = [b for b in bodies if b[0] == "main"]
        if name == "main":
            call = put.start()
        else:
            if not main:
                return False, "no main() in main.cpp"
            m = re.compile(r"\b" + re.escape(name) + r"\s*\(\s*\)\s*;").search(text, main[0][1], main[0][2])
            if not m:
                return False, "%s() is never called from main()" % name
            call = m.start()
        app = re.search(r"SailfishApp::application\s*\(|\bQ(?:Gui)?Application\s+\w+\s*\(", text)
        if not app or call > app.start():
            return False, "TMPDIR is redirected after the application object"
        return True, "main.cpp %s() moves TMPDIR to CacheLocation/tmp before the application object" % name


# ----------------------------------------------------------------------------- sailjail_names (PLT-03 a)

def check_sailjail_names(tree):
    r = Report("sailjail_names")
    if not tree.org or not tree.app:
        r.fail("tests/platform/permissions.json has no sailjail.organization/application")
        return r
    for variant, path in sorted(tree.desktop_files().items()):
        if not os.path.isfile(path):
            r.fail("%s desktop entry missing: %s" % (variant, path))
            continue
        sections = parse_desktop(path)
        jail = sections.get("X-Sailjail")
        name = os.path.basename(path)
        if jail is None:
            r.fail("%s: %s has no [X-Sailjail] section" % (variant, name))
            continue
        for key, want in (("OrganizationName", tree.org), ("ApplicationName", tree.app)):
            got = jail.get(key)
            if got != want:
                r.fail("%s: %s [X-Sailjail] %s=%s, expected %s" % (variant, name, key, got, want))
        sandboxing = jail.get("Sandboxing", "")
        if sandboxing.lower() == "disabled":
            if variant == "STORE":
                r.fail("STORE: %s has Sandboxing=Disabled (Sailjail is mandatory on STORE)" % name)
            else:
                r.note("FULL: %s has Sandboxing=Disabled (allowed until E2/E3; the forced-sandbox test entry drops it)" % name)
    r.note("desktop entries: " + ", ".join("%s=%s" % (v, rel(tree.root, p) if p.startswith(tree.root) else p)
                                         for v, p in sorted(tree.desktop_files().items())))

    main = tree.main_cpp()
    if not os.path.isfile(main):
        r.fail("Clients/Sailfish/Composition/main.cpp missing")
        return r
    text = tree.cleaned(main)
    bodies = [b for b in function_bodies(text) if b[0] == "main"]
    if not bodies:
        r.fail("main.cpp: no main()")
        return r
    _, ms, me = bodies[0]
    app = re.compile(r"SailfishApp::application\s*\(|\bQ(?:Gui)?Application\s+\w+\s*\(").search(text, ms, me)
    if not app:
        r.fail("main.cpp: no application object in main()")
        return r
    first_path = re.compile(r"QStandardPaths::").search(text, ms, me)
    for setter, want in (("setOrganizationName", tree.org), ("setApplicationName", tree.app)):
        static = re.compile(r"\bQ(?:Core|Gui)?Application::" + setter + r"\s*\(").search(text, ms, me)
        if not static:
            r.fail("main.cpp: main() does not call QCoreApplication::%s before the application object" % setter)
            continue
        args, _ = call_args(text, static.end() - 1)
        lit = literal_value(args[0]) if args else None
        line = line_of(text, static.start())
        if not lit or lit[1] != want:
            r.fail("main.cpp:%d: %s(%s), expected \"%s\"" % (line, setter, args[0] if args else "", want))
        if static.start() > app.start():
            r.fail("main.cpp:%d: %s runs after the application object (line %d)" % (line, setter, line_of(text, app.start())))
        if first_path and static.start() > first_path.start():
            r.fail("main.cpp:%d: %s runs after the first QStandardPaths use (line %d)"
                   % (line, setter, line_of(text, first_path.start())))
    ok, detail = tree.tmp_redirect()
    if ok:
        r.note(detail)
    # Every other name setter in shipped code must agree.
    for path in code_files(tree.root):
        if not path.endswith((".cpp", ".h")):
            continue
        t = tree.cleaned(path)
        for m in re.finditer(r"\b(setOrganizationName|setApplicationName)\s*\(", t):
            args, _ = call_args(t, m.end() - 1)
            lit = literal_value(args[0]) if args else None
            want = tree.org if m.group(1) == "setOrganizationName" else tree.app
            if not lit or lit[1] != want:
                r.fail("%s:%d: %s(%s), expected \"%s\"" % (rel(tree.root, path), line_of(t, m.start()), m.group(1),
                                                         args[0] if args else "", want))
    r.note("names %s / %s: desktop [X-Sailjail] and main.cpp agree" % (tree.org, tree.app) if not r.errors else
           "names expected %s / %s" % (tree.org, tree.app))
    return r


# ----------------------------------------------------------------------------- paths (PLT-03 b)

ALLOWED_LOCATIONS = {"AppDataLocation", "AppLocalDataLocation", "DataLocation", "CacheLocation", "AppConfigLocation"}
GENERIC_LOCATIONS = {"GenericCacheLocation", "GenericDataLocation", "GenericConfigLocation"}
QML_ALLOWED_PATHS = {"data", "cache"}
ABSOLUTE_RE = re.compile(r"^(?:/(?:home|tmp|var|usr|opt|etc|media|run|mnt|sdcard|srv|dev|proc|sys|root|boot|data|storage)(?:/|$)|~/)")
FILE_URL_RE = re.compile(r"file://(/(?:home|tmp|media|run/media|mnt|sdcard|root)(?:/[^\"']*)?)")
STRING_RE = re.compile(r"\"(?:[^\"\\\n]|\\.)*\"|'(?:[^'\\\n]|\\.)*'")
ENV_NAMES = r"(HOME|USER|LOGNAME|TMPDIR|TMP|TEMP|XDG_\w+)"
APPEND_WRAPPER_RE = re.compile(r"(?:QStringLiteral|QLatin1String|QString(?:::fromLatin1|::fromUtf8)?|QByteArrayLiteral)\s*\(\s*$")

C_RULES = [
    ("home", re.compile(r"\bQDir::homePath\s*\(|\bQDir::home\s*\(")),
    ("temp", re.compile(r"\bQDir::tempPath\s*\(|\bQDir::temp\s*\(|\bnew\s+QTemporary(?:Dir|File)\s*(?:\(\s*\))?\s*;"
                        r"|\bQTemporary(?:Dir|File)\s+\w+\s*(?:\(\s*\)|\{\s*\})?\s*;")),
    ("env", re.compile(r"\b(?:q?getenv|secure_getenv|qEnvironmentVariable(?:IsSet|IsEmpty|IntValue)?)\s*\(\s*\"" + ENV_NAMES + "\"")),
    ("env", re.compile(r"\.value\s*\(\s*(?:QStringLiteral\s*\(\s*)?\"" + ENV_NAMES + "\"")),
]
PY_RULES = [
    ("home", re.compile(r"\bos\.path\.expanduser\s*\(|\bPath\.home\s*\(|\bos\.path\.expandvars\s*\(")),
    ("temp", re.compile(r"\btempfile\.(?:gettempdir|mkdtemp|mkstemp|TemporaryDirectory|NamedTemporaryFile|TemporaryFile)\s*\(")),
    ("env", re.compile(r"\bos\.environ\s*\[\s*[\"']" + ENV_NAMES + r"[\"']\s*\](?!\s*=[^=])"
                       r"|\bos\.(?:environ\.get|getenv)\s*\(\s*[\"']" + ENV_NAMES)),
]


def path_findings(tree, path):
    """Raw findings: dicts with rule, line, detail."""
    text = tree.cleaned(path)
    is_py = path.endswith(".py")
    found = []
    for rule, rx in (PY_RULES if is_py else C_RULES):
        for m in rx.finditer(text):
            found.append({"rule": rule, "line": line_of(text, m.start()), "detail": m.group(0).strip()})
    if not is_py:
        for m in re.finditer(r"\bQStandardPaths::([A-Z]\w*Location)\b", text):
            loc = m.group(1)
            if loc == "StandardLocation":
                continue
            stmt_end = text.find(";", m.end())
            stmt = text[m.end():stmt_end if stmt_end >= 0 else len(text)]
            found.append({"rule": "location", "line": line_of(text, m.start()), "detail": "QStandardPaths::" + loc,
                          "location": loc, "statement": stmt})
        if path.endswith((".qml", ".js")):
            for m in re.finditer(r"\bStandardPaths\.(\w+)", text):
                found.append({"rule": "location", "line": line_of(text, m.start()), "detail": "StandardPaths." + m.group(1),
                              "location": "qml:" + m.group(1), "statement": ""})
    for m in STRING_RE.finditer(text):
        content = m.group(0)[1:-1]
        prefix = APPEND_WRAPPER_RE.sub("", text[max(0, m.start() - 80):m.start()].rstrip()).rstrip()
        appended = prefix.endswith("+") or prefix.endswith("+=")
        hit = None
        if ABSOLUTE_RE.match(content) and not appended:
            hit = content
        else:
            fm = FILE_URL_RE.search(content)
            if fm:
                hit = fm.group(1)
        if hit is not None:
            found.append({"rule": "absolute", "line": line_of(text, m.start()), "detail": content, "literal": hit})
    return found


def check_paths(tree):
    r = Report("paths")
    cfg = load_json(tree.root, "path_exceptions.json")
    prefixes = cfg.get("allowed_absolute", [])
    exceptions = cfg.get("exceptions", [])
    used = [0] * len(exceptions)
    perms = tree.permissions.get("permissions", {})
    loc_perm = tree.permissions.get("location_permissions", {})
    redirect_ok, redirect_detail = tree.tmp_redirect()
    tied, temp_ok, allowed_abs, excepted = [], [], [], []
    files = code_files(tree.root)
    for path in files:
        relpath = rel(tree.root, path)
        for f in path_findings(tree, path):
            where = "%s:%d" % (relpath, f["line"])
            # Global allowed absolute paths (read-only package files, kernel interfaces).
            if f["rule"] == "absolute" and any(re.match(p["pattern"], f["literal"]) for p in prefixes):
                allowed_abs.append(where + " " + f["literal"])
                continue
            # Exception list: file glob + rule + regex on the detail.
            hit = None
            for i, ex in enumerate(exceptions):
                if fnmatch.fnmatchcase(relpath, ex["file"]) and ex["rule"] == f["rule"] and re.search(ex["match"], f["detail"]):
                    hit = i
                    break
            if hit is not None:
                used[hit] += 1
                excepted.append("%s %s (%s)" % (where, f["detail"], exceptions[hit]["id"]))
                continue
            if f["rule"] == "location":
                loc = f["location"]
                if loc in ALLOWED_LOCATIONS or loc[4:] in QML_ALLOWED_PATHS:
                    continue
                if loc in GENERIC_LOCATIONS:
                    if ('"/%s' % tree.org) in f["statement"] or ('"/%s/%s' % (tree.org, tree.app)) in f["statement"]:
                        continue
                    r.fail("%s: %s without the /%s/%s subdirectory (outside the Sailjail layout)"
                           % (where, f["detail"], tree.org, tree.app))
                    continue
                key = "QStandardPaths::" + loc if not loc.startswith("qml:") else "StandardPaths." + loc[4:]
                if key in loc_perm:
                    perm = loc_perm[key]
                    entry = perms.get(perm, {})
                    globs = entry.get("code", [])
                    if any(fnmatch.fnmatchcase(relpath, g) for g in globs):
                        tied.append("%s %s -> %s (declared: %s)" % (where, f["detail"], perm, entry.get("declared")))
                        continue
                    r.fail("%s: %s is a %s location but this file is not in that permission's code list "
                           "(tests/platform/permissions.json)" % (where, f["detail"], perm))
                    continue
                if loc in ("TempLocation",) or loc == "qml:temporary":
                    if redirect_ok:
                        temp_ok.append(where + " " + f["detail"])
                        continue
                    r.fail("%s: %s while %s" % (where, f["detail"], redirect_detail))
                    continue
                r.fail("%s: %s is outside the Sailjail layout" % (where, f["detail"]))
                continue
            if f["rule"] == "absolute":
                for key, perm in loc_perm.items():
                    if key.startswith("/") and (f["literal"] == key or f["literal"].startswith(key.rstrip("/") + "/")):
                        entry = perms.get(perm, {})
                        if any(fnmatch.fnmatchcase(relpath, g) for g in entry.get("code", [])):
                            tied.append("%s \"%s\" -> %s (declared: %s)" % (where, f["literal"], perm, entry.get("declared")))
                            break
                else:
                    r.fail("%s: hard-coded path \"%s\" outside the Sailjail layout" % (where, f["detail"]))
                continue
            if f["rule"] == "temp":
                if redirect_ok:
                    temp_ok.append(where + " " + f["detail"])
                    continue
                r.fail("%s: %s while %s" % (where, f["detail"], redirect_detail))
                continue
            label = {"home": "home directory access", "env": "environment path variable"}[f["rule"]]
            r.fail("%s: %s (%s) outside the Sailjail layout" % (where, label, f["detail"]))
    for i, ex in enumerate(exceptions):
        if not used[i]:
            r.fail("stale exception %s (%s, rule %s): nothing matches it any more; remove it from "
                   "tests/platform/path_exceptions.json" % (ex["id"], ex["file"], ex["rule"]))
    r.note("scanned %d shipped files under %s (excluding %s)" % (len(files), ", ".join(SCAN_ROOTS), ", ".join(SCAN_EXCLUDE)))
    for t in tied:
        r.note("permission-tied: " + t)
    for t in temp_ok:
        r.note("temporary dir (TMPDIR redirect): " + t)
    for a in allowed_abs:
        r.note("allowed absolute: " + a)
    for e in excepted:
        r.note("exception: " + e)
    return r


# ----------------------------------------------------------------------------- permissions (PLT-04, NFR-TST-03 4)

def store_drops_dbus(root):
    """True when the spec removes the D-Bus service file from the STORE build (Harbour refuses it)."""
    for name in os.listdir(os.path.join(root, "rpm")) if os.path.isdir(os.path.join(root, "rpm")) else []:
        if name.endswith(".spec") and re.search(r"\brm\s+-[rf]+\s+\S*dbus-1/services", read(os.path.join(root, "rpm", name))):
            return True
    return False


def dbus_services(root):
    found = []
    for dirpath, dirnames, filenames in os.walk(root):
        dirnames[:] = [d for d in dirnames if d not in WALK_SKIP_DIRS]
        for name in filenames:
            p = os.path.join(dirpath, name)
            if name.endswith(".service"):
                try:
                    if "[D-BUS Service]" in read(p):
                        found.append(rel(root, p))
                except OSError:
                    pass
            elif name.endswith(".spec") and re.search(r"^\s*%\{_datadir\}/dbus-1/services/", read(p), re.M):
                found.append(rel(root, p) + " (packages dbus-1/services)")
    return sorted(found)


def check_permissions(tree):
    r = Report("permissions")
    cfg = tree.permissions
    perms = cfg.get("permissions", {})
    flags = cfg.get("feature_flags", {})
    for p in PLT04_PERMISSIONS:
        if p not in perms:
            r.fail("PLT-04 permission %s has no entry in tests/platform/permissions.json" % p)
    for p in perms:
        if p not in PLT04_PERMISSIONS:
            r.fail("permissions.json maps %s, which PLT-04 does not allow" % p)

    # Every mapping entry: a feature, flags that exist, variants, code that exists and uses it.
    for p, e in sorted(perms.items()):
        for field in ("feature", "requirement", "variants", "flags"):
            if not e.get(field):
                r.fail("%s: mapping has no %s" % (p, field))
        for v in e.get("variants", []):
            if v not in VARIANTS:
                r.fail("%s: unknown variant %s" % (p, v))
        for fl in e.get("flags", []):
            spec = flags.get(fl)
            if spec is None:
                r.fail("%s: feature flag %s is not defined in feature_flags" % (p, fl))
                continue
            if spec.get("kind") == "define":
                where = glob_files(tree.root, spec.get("where", ""))
                if not any(re.search(r"\b" + re.escape(spec["name"]) + r"\b", read(w)) for w in where):
                    r.fail("%s: flag %s: define %s not found in %s" % (p, fl, spec["name"], spec.get("where")))
            if e.get("declared") and spec.get("kind") == "planned":
                r.fail("%s is declared but its feature flag %s is only planned (no feature uses it)" % (p, fl))
        uses = e.get("uses")
        for g in e.get("code", []):
            matched = glob_files(tree.root, g)
            if not matched:
                r.fail("%s: code glob %s matches no file" % (p, g))
                continue
            if uses and not any(re.search(uses, read(m)) for m in matched):
                r.fail("%s: no file matching %s uses it (/%s/)" % (p, g, uses))
        if e.get("declared") and not e.get("code"):
            r.fail("%s is declared but maps to no code" % p)

    desktops = tree.desktop_files()
    service = dbus_services(tree.root)
    store_drops = store_drops_dbus(tree.root)
    for variant, path in sorted(desktops.items()):
        if not os.path.isfile(path):
            r.fail("%s desktop entry missing: %s" % (variant, path))
            continue
        sections = parse_desktop(path)
        jail = sections.get("X-Sailjail", {})
        declared = split_list(jail.get("Permissions", ""))
        name = os.path.basename(path)
        for p in declared:
            if p not in PLT04_PERMISSIONS:
                r.fail("%s: %s declares %s, outside the PLT-04 list" % (variant, name, p))
                continue
            e = perms.get(p)
            if not e:
                r.fail("%s: %s declares %s with no mapping" % (variant, name, p))
                continue
            if not e.get("declared"):
                r.fail("%s: %s declares %s but the mapping says declared:false (no feature uses it)" % (variant, name, p))
            if variant not in e.get("variants", []):
                r.fail("%s: %s declares %s, which is mapped to %s only" % (variant, name, p, "/".join(e.get("variants", []))))
        for p, e in sorted(perms.items()):
            if e.get("declared") and variant in e.get("variants", []) and p not in declared:
                r.fail("%s: mapping marks %s declared but %s does not declare it" % (variant, p, name))
        exec_dbus = "ExecDBus" in jail
        entry = sections.get("Desktop Entry", {})
        keys = [k for k in ("X-DBusActivatable", "X-Maemo-Service") if entry.get(k) and entry.get(k).lower() != "false"]
        activation = list(service) + ["%s in %s" % (k, name) for k in keys]
        ships = bool(activation) and not (variant == "STORE" and store_drops)
        if ships and not exec_dbus:
            r.fail("%s: D-Bus activation (%s) without ExecDBus in [X-Sailjail] of %s" % (variant, ", ".join(activation), name))
        elif exec_dbus and not activation:
            r.fail("%s: %s has ExecDBus but nothing is D-Bus activated (no service file, no X-DBusActivatable/X-Maemo-Service)"
                   % (variant, name))
        elif exec_dbus and not ships:
            r.note("STORE: the spec drops the D-Bus service; it must drop ExecDBus and the activation keys with it")
        r.note("%s: %s declares %s; ExecDBus %s; D-Bus service files: %s"
               % (variant, name, ";".join(declared) or "(none)", "present" if exec_dbus else "absent",
                  ", ".join(service) or "none"))
    for p, e in sorted(perms.items()):
        fl = ", ".join(e.get("flags", []))
        r.note("%s -> %s [%s] %s%s" % (p, fl, "/".join(e.get("variants", [])), "declared" if e.get("declared") else "not declared",
                                      "" if not e.get("code") else "; code: " + ", ".join(e["code"])))
        if not e.get("declared") and e.get("code") and any(flags.get(f, {}).get("kind") == "always" for f in e.get("flags", [])):
            r.note("SANDBOX GAP (device step): %s is not declared but compiled code needs it: %s" % (p, e.get("gap", e["feature"])))
    return r


# ----------------------------------------------------------------------------- settings inventory

def settings_inventory(tree):
    """(source, {key: (kind, value)}) from the shared table, else the engine accessors."""
    table = tree.path("Core/Settings/Domain/SettingsDefaults.cpp")
    if os.path.isfile(table):
        text = tree.cleaned(table)
        macros = {}
        for m in re.finditer(r"#define\s+(\w+)\s*\([^)]*\)\s*\{[^}]*?SettingType::(\w+)", read(table)):
            macros[m.group(1)] = m.group(2)
        inv = {}
        for m in re.finditer(r"\b(\w+)\s*\(\s*\"([^\"]+)\"\s*,\s*\"([^\"]*)\"", text):
            if m.group(1) in macros and SETTING_KEY_RE.match(m.group(2)):
                inv[m.group(2)] = (macros[m.group(1)], m.group(3), line_of(text, m.start()))
        for m in re.finditer(r"\{\s*\"([^\"]+)\"\s*,\s*SettingType::(\w+)\s*,\s*\"([^\"]*)\"", text):
            inv[m.group(1)] = (m.group(2), m.group(3), line_of(text, m.start()))
        out = {}
        for k, (typ, value, line) in inv.items():
            kind = {"Bool": "bool", "Int": "number", "Real": "number"}.get(typ, "text")
            out[k] = {"kind": kind, "value": value if kind != "number" or not NUMBER_RE.match(value) else float(value),
                      "where": "%s:%d" % (rel(tree.root, table), line)}
        return rel(tree.root, table), out
    path = tree.path("Clients/Sailfish/Presentation/AppSettings.cpp")
    out = {}
    if os.path.isfile(path):
        for item in value_calls(tree, path):
            out.setdefault(item["key"], {"kind": item["lit"][0] if item["lit"] else "text",
                                         "value": item["lit"][1] if item["lit"] else None,
                                         "where": "%s:%d" % (rel(tree.root, path), item["line"])})
        for m in re.finditer(r"\bsetValue\s*\(\s*QStringLiteral\s*\(\s*\"([^\"]+)\"", tree.cleaned(path)):
            out.setdefault(m.group(1), {"kind": "text", "value": None, "where": rel(tree.root, path)})
    return rel(tree.root, path) + " (engine accessors; no shared defaults table on this tree)", out


def value_calls(tree, path):
    """`.value("group/key", default)` calls: key, literal default (or None), line."""
    text = tree.cleaned(path)
    out = []
    for m in re.finditer(r"\.value\s*\(", text):
        args, _ = call_args(text, m.end() - 1)
        if len(args) < 1:
            continue
        key = literal_value(args[0])
        if not key or key[0] != "text" or not SETTING_KEY_RE.match(key[1]):
            continue
        lit = literal_value(args[1]) if len(args) >= 2 else None
        out.append({"key": key[1], "lit": lit, "expr": args[1] if len(args) >= 2 else "", "line": line_of(text, m.start())})
    return out


def property_keys(tree):
    """AppSettings Q_PROPERTY name -> settings key (through the READ accessor's body)."""
    h = tree.path("Clients/Sailfish/Presentation/AppSettings.h")
    c = tree.path("Clients/Sailfish/Presentation/AppSettings.cpp")
    if not (os.path.isfile(h) and os.path.isfile(c)):
        return {}
    text = tree.cleaned(c)
    getters = {}
    for name, s, e in function_bodies(text):
        m = re.search(r"\"([A-Za-z]+/[A-Za-z0-9_]+)\"", text[s:e])
        if m:
            getters[name.split("::")[-1]] = m.group(1)
    out = {}
    for m in re.finditer(r"Q_PROPERTY\s*\(\s*[\w:<>]+\s+(\w+)\s+READ\s+(\w+)", tree.cleaned(h)):
        if m.group(2) in getters:
            out[m.group(1)] = getters[m.group(2)]
    return out


# ----------------------------------------------------------------------------- cache_key (NFR-TST-03 1)

def struct_fields(text, name):
    m = re.search(r"\bstruct\s+" + re.escape(name) + r"\s*\{", text)
    if not m:
        return None
    start = m.end() - 1
    body = text[start + 1:matching(text, start) - 1]
    stmts, cur, depth = [], [], 0
    for c in body:
        if c == "{":
            depth += 1
        elif c == "}":
            depth -= 1
            if depth == 0:
                cur.append(c)
                stmts.append("".join(cur))
                cur = []
                continue
        if c == ";" and depth == 0:
            stmts.append("".join(cur))
            cur = []
        else:
            cur.append(c)
    fields = []
    for s in stmts:
        s = " ".join(s.split())
        if not s or re.match(r"^(static|using|typedef|friend|enum|struct|class|public|private|protected)\b", s):
            continue
        head = s.split("=")[0]
        if "(" in head or "{" in head and not re.search(r"\w+\s*\{\s*[^}]*\}\s*$", s):
            continue
        parts, cur2, d = [], [], 0
        for c in s:
            if c in "<({[":
                d += 1
            elif c in ">)}]":
                d -= 1
            if c == "," and d == 0:
                parts.append("".join(cur2))
                cur2 = []
            else:
                cur2.append(c)
        parts.append("".join(cur2))
        for i, p in enumerate(parts):
            decl = re.split(r"=|\{", p)[0].strip()
            ids = re.findall(r"[A-Za-z_]\w*", decl)
            if ids:
                fields.append(ids[-1] if i == 0 else ids[0])
    return fields


def key_function_body(text, struct, function):
    m = re.search(r"\b" + re.escape(struct) + r"::" + re.escape(function) + r"\s*\([^)]*\)\s*(?:const\s*)?\{", text)
    if not m:
        m = re.search(r"\b" + re.escape(function) + r"\s*\([^)]*\)\s*(?:const\s*)?\{", text)
    if not m:
        return None
    start = m.end() - 1
    return text[start:matching(text, start)]


def check_cache_key(tree):
    r = Report("cache_key")
    cfg = load_json(tree.root, "output_settings.json")
    source, inventory = settings_inventory(tree)
    r.note("settings inventory: %s (%d keys)" % (source, len(inventory)))
    req = cfg["request"]
    req_path = tree.path(req["file"])
    if not os.path.isfile(req_path):
        r.fail("request contract %s missing" % req["file"])
        return r
    fields = struct_fields(tree.cleaned(req_path), req["struct"])
    if fields is None:
        r.fail("%s: struct %s not found" % (req["file"], req["struct"]))
        return r
    declared = req.get("fields", {})
    for f in fields:
        if f not in declared:
            r.fail("%s::%s (%s) is not classified in output_settings.json request.fields"
                   % (req["struct"], f, req["file"]))
    for f in declared:
        if f not in fields:
            r.fail("output_settings.json request.fields lists %s, which %s::%s does not have" % (f, req["file"], req["struct"]))
    output_fields = [f for f in fields if declared.get(f, {}).get("role") == "output"]

    # Settings: every inventory key is classified; output ones name an output request field.
    settings = cfg.get("settings", {})
    for key in sorted(inventory):
        spec = settings.get(key)
        where = inventory[key]["where"]
        if spec is None:
            r.fail("%s (%s) is not classified in output_settings.json: mark it output with its StreamRequest "
                   "field, or not output with a reason" % (key, where))
            continue
        if spec.get("output"):
            field = spec.get("request_field")
            if not field or field not in fields:
                r.fail("%s is output-affecting but its request field %s is not in %s::%s"
                       % (key, field, req["file"], req["struct"]))
            elif declared.get(field, {}).get("role") != "output":
                r.fail("%s maps to %s::%s, which request.fields does not mark output" % (key, req["struct"], field))
        elif not spec.get("reason"):
            r.fail("%s is marked not output without a reason" % key)
    absent = sorted(k for k in settings if k not in inventory)
    if absent:
        r.note("classified but not in the inventory on this tree (checked once they appear): " + ", ".join(absent))

    # Every cache key implementation covers every output field.
    for ck in cfg.get("cache_keys", []):
        path = tree.path(ck["file"])
        if not os.path.isfile(path):
            r.fail("cache key %s: %s missing" % (ck["name"], ck["file"]))
            continue
        text = tree.cleaned(path)
        kfields = struct_fields(text, ck["struct"])
        body = key_function_body(text, ck["struct"], ck["function"])
        if kfields is None or body is None:
            r.fail("cache key %s: %s::%s not found in %s" % (ck["name"], ck["struct"], ck["function"], ck["file"]))
            continue
        refs = set(re.findall(r"[A-Za-z_]\w*", body))
        exempt = ck.get("component_fields_outside_key", {})
        for kf in kfields:
            if kf not in refs and kf not in exempt:
                r.fail("cache key %s: %s::%s is not part of %s() (%s)" % (ck["name"], ck["struct"], kf, ck["function"], ck["file"]))
        for kf in exempt:
            if kf not in kfields:
                r.fail("cache key %s: exemption for %s, which %s::%s does not have" % (ck["name"], kf, ck["file"], ck["struct"]))
        mapping = ck.get("fields", {})
        host = ck.get("host_applied", {})
        for f in [x for x in fields if declared.get(x, {}).get("role") in ("output", "identity")]:
            if f in mapping:
                target = mapping[f]
                if target not in refs:
                    r.fail("cache key %s: %s::%s maps to %s, which %s() does not use"
                           % (ck["name"], req["struct"], f, target, ck["function"]))
                elif target not in kfields and declared.get(f, {}).get("role") != "identity":
                    r.fail("cache key %s: %s::%s maps to %s, which %s::%s does not have"
                           % (ck["name"], req["struct"], f, target, ck["file"], ck["struct"]))
            elif f in host:
                r.note("cache key %s: %s is applied by the host after the shared answer: %s" % (ck["name"], f, host[f]))
            else:
                users = [k for k, s in settings.items() if s.get("request_field") == f and k in inventory]
                r.fail("cache key %s: output field %s::%s%s is not in %s::%s() and not declared host-applied"
                       % (ck["name"], req["struct"], f, " (setting %s)" % ", ".join(users) if users else "",
                          ck["struct"], ck["function"]))
        r.note("cache key %s (%s::%s): %s" % (ck["name"], ck["struct"], ck["function"],
                                               ", ".join("%s->%s" % (k, v) for k, v in mapping.items())))
    outs = [k for k in sorted(inventory) if settings.get(k, {}).get("output")]
    r.note("output-affecting settings: %s" % (", ".join("%s->%s" % (k, settings[k]["request_field"]) for k in outs) or "none"))
    return r


# ----------------------------------------------------------------------------- ui_defaults (NFR-TST-03 2)

def ui_literals(tree, inventory_keys, props):
    """(key, literal, where, form) default literals written in the UI, plus files scanned."""
    out = []
    base = tree.path("Clients/Sailfish/qml")
    files = []
    for dirpath, _, filenames in os.walk(base):
        for name in sorted(filenames):
            if name.endswith((".qml", ".js")):
                files.append(os.path.join(dirpath, name))
    for path in sorted(files):
        relpath = rel(tree.root, path)
        text = tree.cleaned(path)
        for item in value_calls(tree, path):
            if item["lit"] is not None:
                out.append((item["key"], item["lit"], "%s:%d" % (relpath, item["line"]), "value() fallback"))
        for m in re.finditer(r"\bkey\s*:\s*([\"'])([^\"']+)\1", text):
            raw = m.group(2)
            key = raw if raw in inventory_keys else next((k for k in inventory_keys if raw.endswith("/" + k)), None)
            open_pos = text.rfind("{", 0, m.start())
            if open_pos < 0:
                continue
            block = text[open_pos:matching(text, open_pos)]
            d = re.search(r"\b(def|defaultValue)\s*:\s*", block)
            if not d:
                continue
            rest = block[d.end():]
            value = re.match(r"(\"[^\"]*\"|'[^']*'|[^,;\n}]+)", rest)
            lit = literal_value(value.group(1)) if value else None
            if lit is None:
                continue
            if key is None:
                if SETTING_KEY_RE.match(raw) or raw.startswith("/"):
                    out.append((raw, lit, "%s:%d" % (relpath, line_of(text, m.start())), "unknown key"))
                continue
            out.append((key, lit, "%s:%d" % (relpath, line_of(text, m.start())), d.group(1) + ":"))
        for m in re.finditer(r"\bappSettings\.(\w+)\s*\|\|\s*(\"[^\"]*\"|'[^']*'|[\w.+-]+)", text):
            lit = literal_value(m.group(2))
            if lit and m.group(1) in props:
                out.append((props[m.group(1)], lit, "%s:%d" % (relpath, line_of(text, m.start())), "|| fallback"))
    return out, files


def check_ui_defaults(tree):
    r = Report("ui_defaults")
    source, inventory = settings_inventory(tree)
    if not inventory:
        r.fail("no settings inventory found (%s)" % source)
        return r
    r.note("defaults table: %s (%d keys)" % (source, len(inventory)))
    props = property_keys(tree)
    found, files = ui_literals(tree, set(inventory), props)
    shared_table = source.endswith("SettingsDefaults.cpp")
    if shared_table:
        engine = tree.path("Clients/Sailfish/Presentation/AppSettings.cpp")
        if os.path.isfile(engine):
            for item in value_calls(tree, engine):
                if item["lit"] is not None:
                    found.append((item["key"], item["lit"], "%s:%d" % (rel(tree.root, engine), item["line"]), "engine fallback"))
    matched = 0
    for key, lit, where, form in found:
        if form == "unknown key" or key not in inventory:
            r.fail("%s: default %s for %s, which is not in the defaults table (%s)" % (where, show(*lit), key, source))
            continue
        t = inventory[key]
        if t["value"] is None:
            r.fail("%s: default %s for %s, but the table has no literal default for it" % (where, show(*lit), key))
            continue
        if canonical(*lit) != canonical(t["kind"], t["value"]):
            r.fail("%s: %s: UI literal %s, table %s (%s)" % (where, key, show(*lit), show(t["kind"], t["value"]), t["where"]))
        else:
            matched += 1
    ui_count = len([f for f in found if f[3] != "engine fallback"])
    if not ui_count:
        r.note("no hard-coded default literals in %d QML/JS files: the settings pages read the engine at run time"
               % len(files))
    else:
        r.note("%d default literal(s) in the UI checked against the table, %d match" % (ui_count, matched))
    r.note("QML property -> key: %s" % (", ".join("%s=%s" % kv for kv in sorted(props.items())) or "none"))
    return r


# ----------------------------------------------------------------------------- sponsorblock (NFR-TST-03 3)

def engine_categories(tree):
    cats = {}
    fn_re = re.compile(r"\b((?:\w+::)*sponsor\w*Categor(?:y|ies)\w*)\s*\([^;{)]*\)\s*(?:const\s*)?\{", re.I)
    const_re = re.compile(r"\b(k\w*Sponsor\w*Categor\w*)\s*(?:\[\s*\])?\s*=\s*(?:QStringLiteral\s*\(\s*)?\"([a-z][a-z0-9_]*)\"", re.I)
    for path in code_files(tree.root):
        if not path.endswith((".h", ".hpp", ".cpp", ".cc")) or "/Clients/" in "/" + rel(tree.root, path):
            continue
        text = tree.cleaned(path)
        relpath = rel(tree.root, path)
        for m in fn_re.finditer(text):
            body = text[m.end() - 1:matching(text, m.end() - 1)]
            for s in re.finditer(r"\"([a-z][a-z0-9_]*)\"", body):
                cats.setdefault(s.group(1), "%s:%d %s()" % (relpath, line_of(text, m.start() + s.start()), m.group(1)))
        for m in const_re.finditer(text):
            cats.setdefault(m.group(2), "%s:%d %s" % (relpath, line_of(text, m.start()), m.group(1)))
    return cats


def check_sponsorblock(tree):
    r = Report("sponsorblock")
    cats = engine_categories(tree)
    if not cats:
        r.note("no SponsorBlock categories in the engine yet (no sponsor*Categories() list or kSponsor*Category "
               "constant under Core/ or Shared/); the invariant holds vacuously")
        return r
    ui = []
    base = tree.path("Clients/Sailfish/qml")
    for dirpath, _, filenames in os.walk(base):
        for name in sorted(filenames):
            if name.endswith((".qml", ".js")):
                p = os.path.join(dirpath, name)
                t = tree.cleaned(p)
                if re.search(r"sponsor", t, re.I):
                    ui.append((p, t))
    # Shrink-only waiver for rows another component still owes (tests/platform/pending.json): a waived category
    # only warns, and an entry that is no longer needed (row added, category gone) fails until removed.
    pending_path = tree.path("tests/platform/pending.json")
    pending = {}
    if os.path.exists(pending_path):
        pending = json.loads(read(pending_path)).get("sponsorblock_ui_rows", {})
    waived = set(pending.get("categories", []))
    for cat, where in sorted(cats.items()):
        rows = [rel(tree.root, p) for p, t in ui if re.search(r"([\"'])" + re.escape(cat) + r"\1", t)]
        if rows:
            r.note("%s (%s): UI row in %s" % (cat, where, ", ".join(rows)))
            if cat in waived:
                r.fail("tests/platform/pending.json waives %s, which has a UI row now: remove it (the waiver may only shrink)" % cat)
        elif cat in waived:
            r.note("PENDING (%s, %s): %s has no UI row yet" % (pending.get("owner", "?"), pending.get("requirement", "?"), cat))
        else:
            r.fail("SponsorBlock category %s is fetched by the engine (%s) but has no UI row under Clients/Sailfish/qml"
                   % (cat, where))
    for cat in sorted(waived - set(cats)):
        r.fail("tests/platform/pending.json waives %s, which the engine no longer fetches: remove it" % cat)
    r.note("engine categories: %s; UI files mentioning SponsorBlock: %s"
           % (", ".join(sorted(cats)), ", ".join(rel(tree.root, p) for p, _ in ui) or "none"))
    return r


# ----------------------------------------------------------------------------- forced-sandbox desktop (PLT-03 c)

def forced_sandbox_desktop(source_path):
    """The desktop entry with Sailjail sandboxing forced on: any Sandboxing=Disabled is dropped."""
    lines = read(source_path).splitlines()
    out = ["# PLT-03 forced-sandbox FULL test entry, generated by tools/platform_check.py from %s. Never shipped."
           % os.path.basename(source_path)]
    section, saw_jail = None, False
    for raw in lines:
        s = raw.strip()
        if s.startswith("[") and s.endswith("]"):
            section = s[1:-1]
            saw_jail = saw_jail or section == "X-Sailjail"
        elif section == "X-Sailjail" and "=" in s:
            k, v = s.split("=", 1)
            if k.strip() == "Sandboxing" and v.strip().lower() == "disabled":
                continue
        out.append(raw)
    if not saw_jail:
        raise SystemExit("platform_check: %s has no [X-Sailjail] section" % source_path)
    return "\n".join(out) + "\n"


# ----------------------------------------------------------------------------- main

RUNNERS = {
    "sailjail_names": check_sailjail_names,
    "paths": check_paths,
    "permissions": check_permissions,
    "cache_key": check_cache_key,
    "ui_defaults": check_ui_defaults,
    "sponsorblock": check_sponsorblock,
}


def main(argv):
    parser = argparse.ArgumentParser(description="SailTube static platform and invariant checks")
    parser.add_argument("--root", default=os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
    parser.add_argument("--desktop", help="desktop entry to audit for every variant (default: permissions.json desktop_files)")
    parser.add_argument("--write-forced-sandbox-desktop", metavar="OUT",
                        help="write the forced-sandbox FULL test desktop entry and exit")
    parser.add_argument("checks", nargs="*", help="checks to run (default: all): " + ", ".join(CHECKS))
    args = parser.parse_args(argv)
    root = os.path.abspath(args.root)
    desktop = os.path.abspath(args.desktop) if args.desktop else None
    if args.write_forced_sandbox_desktop:
        src = desktop or os.path.join(root, "harbour-sailtube.desktop")
        with open(args.write_forced_sandbox_desktop, "w", encoding="utf-8") as f:
            f.write(forced_sandbox_desktop(src))
        print("platform_check: forced-sandbox desktop entry written to %s" % args.write_forced_sandbox_desktop)
        return 0
    unknown = [c for c in args.checks if c not in RUNNERS]
    if unknown:
        parser.error("unknown check(s): %s" % ", ".join(unknown))
    tree = Tree(root, desktop)
    failed = []
    for name in args.checks or CHECKS:
        if not RUNNERS[name](tree).emit():
            failed.append(name)
    print("platform_check: %d check(s), %d failed%s" % (len(args.checks or CHECKS), len(failed),
                                                        ": " + ", ".join(failed) if failed else ""))
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
