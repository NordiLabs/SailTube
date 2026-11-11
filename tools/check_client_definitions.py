#!/usr/bin/env python3
# SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
"""NFR-MNT-04: client definitions and endpoint parameters live in one file per component.

    tools/check_client_definitions.py [--root DIR] [--config FILE] [--waivers FILE] [--list] [--json]

Scans the shipped code (Core/, Clients/, Shared/ minus Shared/SailTune/tests/) for the string
literals that tie SailTube to one YouTube client generation:

  client-name     InnerTube client names ("WEB", "MWEB", "ANDROID_VR", "VISIONOS", ...), also inside a
                  JSON literal ("clientName":"WEB")
  client-version  client versions (N.YYYYMMDD.NN.NN, or a short version on a version assignment)
  identity-key    keys a client version is scraped from (INNERTUBE_CLIENT_VERSION ...)
  client-header   X-YouTube-Client-Name / X-YouTube-Client-Version header names
  endpoint        InnerTube paths ("youtubei/v1/<name>", and the endpoint name passed to post<...>() /
                  sendJson())
  query-param     fixed query parameters of those requests (prettyPrint=, client= of suggestions)
  params          opaque endpoint "params" tokens (protobuf base64: "EgZ2aWRlb3PyBgQKAjoA", ...)
  browse-id       fixed browse ids (FEwhat_to_watch, FEchannels, VLWL, ...)
  visitor-key     visitor data keys (visitorData, X-Goog-Visitor-Id)
  extractor-arg   yt-dlp extractor arguments ("youtube:skip=dash", "youtube:player_client=...")

Comments are ignored; only literals count. Rules (tests/client_definitions/components.json):

  1. Every site belongs to a component (a directory). A site outside every component FAILs.
  2. Per component exactly one file, its definitions file, may hold sites. A site in any other file
     of the component is a stray and FAILs, unless tests/client_definitions/pending.json waives it.
  3. Every value in a definitions file appears in at least one fixture under tests/** or
     Shared/SailTune/tests/** (a recorded request/response, .gz included, or a test source that
     asserts the request shape). tests/client_definitions/ itself never counts.
  4. The waiver list is shrink-only: an entry that no longer matches a site FAILs (delete it).

Exit 0 PASS, 1 FAIL, 2 usage or configuration error. Python 3.9 standard library only.
"""
import argparse
import gzip
import io
import json
import os
import re
import sys
import tokenize

SOURCE_EXTENSIONS = (".h", ".hpp", ".cpp", ".cc", ".c", ".qml", ".js", ".py")

CLIENT_NAME_RE = re.compile(r"(?:WEB|MWEB|ANDROID|IOS|TVHTML5|TVLITE|VISIONOS|MEDIA_CONNECT_FRONTEND)(?:_[A-Z0-9]+)*")
JSON_CLIENT_NAME_RE = re.compile(r'"clientName"\s*:\s*"([A-Za-z0-9_]+)"')
JSON_CLIENT_VERSION_RE = re.compile(r'"(?:clientVersion|osVersion)"\s*:\s*"([0-9][0-9A-Za-z.]{0,31})"')
WEB_VERSION_RE = re.compile(r"(?<![\d.])\d{1,2}\.(?:19|20)\d{2}[01]\d[0-3]\d\.\d{2}\.\d{2}(?![\d.])")
SHORT_VERSION_RE = re.compile(r"\d{1,3}(?:\.[0-9A-Za-z]{1,8}){1,3}")
VERSION_LINE_RE = re.compile(r"(?i)client\w*version|osversion|\b(?:c|client)\.version\b|\"version\"\s*:")
IDENTITY_KEY_RE = re.compile(r"INNERTUBE_[A-Z_]+")
CLIENT_HEADERS = ("X-YouTube-Client-Name", "X-YouTube-Client-Version")
ENDPOINT_PATH_RE = re.compile(r"youtubei/v1/([A-Za-z_][A-Za-z_/]*)?")
ENDPOINT_CALL_RE = re.compile(r'(?:\bpost\s*<[^;"]*?>\s*|\bsendJson\s*|\bit_post\s*)\(\s*(?:QStringLiteral\s*\(\s*)?"([a-z_]+(?:/[a-z_]+)*)"')
QUERY_PARAM_RE = re.compile(r"(?:^|[?&])(prettyPrint=[a-z]+|client=[a-z0-9_]+)")
PARAMS_TOKEN_RE = re.compile(r"Eg[A-Za-z0-9+/_%=-]{4,}")
PARAMS_LINE_TOKEN_RE = re.compile(r"[A-Za-z0-9+/_-]{8,}(?:={1,2}|(?:%3D){1,2})?")
BROWSE_ID_RE = re.compile(r"FE[a-z][a-z_]*|VL(?:WL|LL)")
VISITOR_KEYS = ("visitorData", "X-Goog-Visitor-Id")


# ---------------------------------------------------------------------------- literal extraction
def _c_unescape(raw):
    out, i = [], 0
    while i < len(raw):
        c = raw[i]
        if c == "\\" and i + 1 < len(raw):
            n = raw[i + 1]
            out.append({"n": "\n", "t": "\t", "r": "\r", "0": "\0"}.get(n, n))
            i += 2
            continue
        out.append(c)
        i += 1
    return "".join(out)


def c_literals(text):
    """(line, value) of every string literal of C/C++/QML/JS text, comments skipped."""
    out, i, n, line = [], 0, len(text), 1
    while i < n:
        c = text[i]
        if c == "\n":
            line += 1
            i += 1
        elif text.startswith("//", i):
            j = text.find("\n", i)
            i = n if j < 0 else j
        elif text.startswith("/*", i):
            j = text.find("*/", i + 2)
            end = n if j < 0 else j + 2
            line += text.count("\n", i, end)
            i = end
        elif c == "R" and text.startswith('R"', i) and (i == 0 or not (text[i - 1].isalnum() or text[i - 1] == "_")):
            k = text.find("(", i + 2)
            if k < 0:
                i += 1
                continue
            close = ")" + text[i + 2:k] + '"'
            j = text.find(close, k + 1)
            end = n if j < 0 else j
            out.append((line, text[k + 1:end]))
            line += text.count("\n", i, end)
            i = n if j < 0 else j + len(close)
        elif c in "\"'`":
            j, buf = i + 1, []
            while j < n and text[j] != c:
                if text[j] == "\\" and j + 1 < n:
                    buf.append(text[j:j + 2])
                    j += 2
                    continue
                if text[j] == "\n" and c != "`":
                    break
                buf.append(text[j])
                j += 1
            raw = "".join(buf)
            out.append((line, _c_unescape(raw)))
            line += raw.count("\n")
            i = j + 1
        else:
            i += 1
    return out


def py_literals(text):
    out = []
    try:
        for tok in tokenize.generate_tokens(io.StringIO(text).readline):
            if tok.type != tokenize.STRING:
                continue
            s = tok.string
            m = re.match(r"(?i)[rbuf]*", s)
            body = s[m.end():]
            q = body[:3] if body[:3] in ('"""', "'''") else body[:1]
            value = body[len(q):len(body) - len(q)] if len(body) >= 2 * len(q) else body
            if "r" not in m.group(0).lower():
                value = _c_unescape(value)
            out.append((tok.start[0], value))
    except (tokenize.TokenError, IndentationError, SyntaxError):
        return c_literals(text)
    return out


def literals(path, text):
    return py_literals(text) if path.endswith(".py") else c_literals(text)


# ---------------------------------------------------------------------------- classification
def classify(value, line_text):
    """(kind, normalized value) pairs a literal contributes."""
    found = []
    if CLIENT_NAME_RE.fullmatch(value):
        found.append(("client-name", value))
    for m in JSON_CLIENT_NAME_RE.finditer(value):
        found.append(("client-name", m.group(1)))
    for m in WEB_VERSION_RE.finditer(value):
        found.append(("client-version", m.group(0)))
    for m in JSON_CLIENT_VERSION_RE.finditer(value):
        found.append(("client-version", m.group(1)))
    if not WEB_VERSION_RE.search(value) and SHORT_VERSION_RE.fullmatch(value) and VERSION_LINE_RE.search(line_text):
        found.append(("client-version", value))
    for m in IDENTITY_KEY_RE.finditer(value):
        found.append(("identity-key", m.group(0)))
    if value in CLIENT_HEADERS:
        found.append(("client-header", value))
    for m in ENDPOINT_PATH_RE.finditer(value):
        name = (m.group(1) or "").rstrip("/")
        found.append(("endpoint", "youtubei/v1/" + name))
    for m in ENDPOINT_CALL_RE.finditer(line_text):
        if m.group(1) == value:
            found.append(("endpoint", "youtubei/v1/" + value))
    for m in QUERY_PARAM_RE.finditer(value):
        found.append(("query-param", m.group(1)))
    if PARAMS_TOKEN_RE.fullmatch(value):
        found.append(("params", value))
    elif re.search(r"(?i)params", line_text) and PARAMS_LINE_TOKEN_RE.fullmatch(value) \
            and re.search(r"[A-Z]", value) and re.search(r"[a-z]", value) and re.search(r"\d|=|%3D", value):
        found.append(("params", value))
    if BROWSE_ID_RE.fullmatch(value):
        found.append(("browse-id", value))
    if value in VISITOR_KEYS:
        found.append(("visitor-key", value))
    if value.startswith("youtube:") and len(value) > len("youtube:"):
        found.append(("extractor-arg", value))
    seen, unique = set(), []
    for f in found:
        if f not in seen:
            seen.add(f)
            unique.append(f)
    return unique


def scan_file(root, rel):
    try:
        with open(os.path.join(root, rel), encoding="utf-8", errors="replace") as f:
            text = f.read()
    except OSError:
        return []
    lines = text.split("\n")
    sites = []
    for line, value in literals(rel, text):
        line_text = lines[line - 1] if 0 < line <= len(lines) else ""
        for kind, v in classify(value, line_text):
            sites.append({"file": rel, "line": line, "kind": kind, "value": v})
    return sites


def walk(root, roots, exclude, extensions=None):
    out = []
    excl = [e.rstrip("/") + "/" for e in exclude]
    for r in roots:
        base = os.path.join(root, r)
        if os.path.isfile(base):
            out.append(r)
            continue
        for d, dirs, files in os.walk(base):
            dirs.sort()
            for name in sorted(files):
                rel = os.path.relpath(os.path.join(d, name), root).replace(os.sep, "/")
                if any((rel + "/").startswith(e) or rel.startswith(e) for e in excl):
                    continue
                if extensions and not rel.endswith(extensions):
                    continue
                out.append(rel)
    return out


# ---------------------------------------------------------------------------- fixtures
class Corpus:
    """Text of every fixture file; .gz files decompressed; binaries skipped."""

    def __init__(self, root, roots, exclude, max_bytes=64 << 20):
        self.files = []
        plain, packed = [], []
        for rel in walk(root, roots, exclude):
            (packed if rel.endswith(".gz") else plain).append(rel)
        for rel in plain + packed:   # cheap files first: most values are found before any .gz
            path = os.path.join(root, rel)
            try:
                if os.path.getsize(path) > max_bytes:
                    continue
                if rel.endswith(".gz"):
                    with gzip.open(path, "rb") as f:
                        data = f.read(max_bytes)
                else:
                    with open(path, "rb") as f:
                        data = f.read()
            except (OSError, EOFError):   # gzip.BadGzipFile is an OSError
                continue
            if b"\0" in data[:4096]:
                continue
            self.files.append((rel, data.decode("utf-8", "replace")))

    @staticmethod
    def pattern(kind, value):
        v = re.escape(value)
        if kind == "endpoint":
            name = value[len("youtubei/v1/"):]
            if not name:
                return re.compile(r"youtubei/v1/")
            n = re.escape(name)
            return re.compile(r"youtubei/v1/" + n + r"(?![A-Za-z_])|[\"'`]" + n + r"(?:[\"'`\\])")
        if kind == "client-name" or len(value) < 8:
            # Short values must stand alone: "WEB", 'WEB', \"WEB\", =WEB& (never "1.02M views").
            return re.compile(r"[\"'`=]" + v + r"(?:[\"'`\\&,;)]|$)", re.M)
        return re.compile(r"(?<![A-Za-z0-9_.])" + v + r"(?![A-Za-z0-9_])")

    def find(self, kind, value):
        needle = value[len("youtubei/v1/"):] if kind == "endpoint" and value != "youtubei/v1/" else value
        pat = self.pattern(kind, value)
        for rel, text in self.files:
            if needle in text and pat.search(text):
                return rel
        return None


# ---------------------------------------------------------------------------- check
def load_json(path):
    with open(path, encoding="utf-8") as f:
        return json.load(f)


def component_of(rel, components):
    best = None
    for c in components:
        r = c["root"].rstrip("/") + "/"
        if rel.startswith(r) and (best is None or len(r) > len(best["root"].rstrip("/") + "/")):
            best = c
    return best


def check(root, config, waivers):
    scan = config["scan"]
    components = config["components"]
    files = walk(root, scan["roots"], scan.get("exclude", []), tuple(scan.get("extensions", SOURCE_EXTENSIONS)))
    sites = []
    for rel in files:
        sites.extend(scan_file(root, rel))

    problems, notes = [], []
    defs_values = {}   # component name -> {(kind, value): first site}
    strays = []
    for c in components:
        if not os.path.isfile(os.path.join(root, c["definitions"])):
            problems.append("FAIL config: definitions file %s of component %s is missing" % (c["definitions"], c["name"]))
        defs_values[c["name"]] = {}
    for s in sites:
        c = component_of(s["file"], components)
        s["component"] = c["name"] if c else None
        if c is None:
            problems.append('FAIL unowned %s:%d %s "%s" (no component in tests/client_definitions/components.json owns it)'
                            % (s["file"], s["line"], s["kind"], s["value"]))
        elif s["file"] == c["definitions"]:
            defs_values[c["name"]].setdefault((s["kind"], s["value"]), s)
        else:
            strays.append(s)

    # A waiver covers `sites` (default 1) literal sites of one value in one file: one more site is a
    # new stray (FAIL), one fewer means the entry must shrink (FAIL: lower `sites` or delete it).
    stray_waivers = waivers.get("strays", [])
    used = [0] * len(stray_waivers)
    for s in strays:
        hit = None
        for i, w in enumerate(stray_waivers):
            if w.get("file") == s["file"] and w.get("kind") == s["kind"] and w.get("value") == s["value"]:
                hit = i
                break
        c = component_of(s["file"], components)
        if hit is not None and used[hit] < int(stray_waivers[hit].get("sites", 1)):
            used[hit] += 1
            notes.append('WAIVED stray %s:%d %s "%s" (owner %s)' % (s["file"], s["line"], s["kind"], s["value"],
                                                                     stray_waivers[hit].get("owner", "?")))
        else:
            problems.append('FAIL stray %s:%d %s "%s" (component %s keeps these in %s only)'
                            % (s["file"], s["line"], s["kind"], s["value"], c["name"], c["definitions"]))
    for i, w in enumerate(stray_waivers):
        want = int(w.get("sites", 1))
        if used[i] == 0:
            problems.append('FAIL stale waiver strays[%d] %s %s "%s": the stray is gone, delete the entry (shrink-only)'
                            % (i, w.get("file"), w.get("kind"), w.get("value")))
        elif used[i] < want:
            problems.append('FAIL stale waiver strays[%d] %s %s "%s": %d of %d sites left, lower "sites" (shrink-only)'
                            % (i, w.get("file"), w.get("kind"), w.get("value"), used[i], want))

    fixture_cfg = config["fixtures"]
    corpus = Corpus(root, fixture_cfg["roots"], fixture_cfg.get("exclude", []))
    fix_waivers = waivers.get("unfixtured", [])
    fused = [False] * len(fix_waivers)
    inventory = []
    for c in components:
        for (kind, value), s in sorted(defs_values[c["name"]].items(), key=lambda kv: kv[1]["line"]):
            ref = corpus.find(kind, value)
            inventory.append({"component": c["name"], "file": s["file"], "line": s["line"], "kind": kind,
                              "value": value, "fixture": ref})
            if ref:
                continue
            hit = None
            for i, w in enumerate(fix_waivers):
                if w.get("file") == s["file"] and w.get("kind") == kind and w.get("value") == value:
                    hit = i
                    break
            if hit is None:
                problems.append('FAIL unfixtured %s:%d %s "%s" (no fixture under %s mentions it; re-record or add a '
                                'request-shape assertion)' % (s["file"], s["line"], kind, value, ", ".join(fixture_cfg["roots"])))
            else:
                fused[hit] = True
                notes.append('WAIVED unfixtured %s:%d %s "%s" (owner %s)' % (s["file"], s["line"], kind, value,
                                                                             fix_waivers[hit].get("owner", "?")))
    for i, w in enumerate(fix_waivers):
        if not fused[i]:
            problems.append('FAIL stale waiver unfixtured[%d] %s %s "%s": the value now has a fixture or is gone, delete '
                            'the entry (shrink-only)' % (i, w.get("file"), w.get("kind"), w.get("value")))
    return {"sites": sites, "inventory": inventory, "strays": strays, "problems": problems, "notes": notes,
            "files": len(files), "fixtures": len(corpus.files)}


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--root", default=os.path.join(os.path.dirname(os.path.abspath(__file__)), ".."))
    ap.add_argument("--config", default=None, help="default <root>/tests/client_definitions/components.json")
    ap.add_argument("--waivers", default=None, help="default <root>/tests/client_definitions/pending.json")
    ap.add_argument("--list", action="store_true", help="print every definitions value with its fixture")
    ap.add_argument("--json", action="store_true", help="machine-readable result on stdout")
    args = ap.parse_args(argv)
    root = os.path.abspath(args.root)
    try:
        config = load_json(args.config or os.path.join(root, "tests", "client_definitions", "components.json"))
        wpath = args.waivers or os.path.join(root, "tests", "client_definitions", "pending.json")
        waivers = load_json(wpath) if os.path.exists(wpath) else {}
        for key in ("scan", "fixtures", "components"):
            if key not in config:
                raise ValueError("config has no %r" % key)
    except (OSError, ValueError) as e:
        print("check_client_definitions: configuration error: %s" % e, file=sys.stderr)
        return 2
    result = check(root, config, waivers)
    ok = not result["problems"]
    if args.json:
        json.dump({"ok": ok, "problems": result["problems"], "notes": result["notes"], "inventory": result["inventory"],
                   "sites": result["sites"]}, sys.stdout, indent=1)
        sys.stdout.write("\n")
        return 0 if ok else 1
    if args.list:
        for e in result["inventory"]:
            print('%-15s %s:%d %-14s "%s" -> %s' % (e["component"], e["file"], e["line"], e["kind"], e["value"],
                                                   e["fixture"] or "NO FIXTURE"))
    for n in result["notes"]:
        print(n)
    for p in result["problems"]:
        print(p)
    print("check_client_definitions: %d source files, %d fixture files, %d sites, %d definitions values, %d waived"
          % (result["files"], result["fixtures"], len(result["sites"]), len(result["inventory"]), len(result["notes"])))
    print("check_client_definitions: %s" % ("PASS" if ok else "FAIL"))
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
