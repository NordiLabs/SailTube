#!/usr/bin/env python3
# SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
"""Static outbound-network inventory check for SailTune (host script, no build).

Fails when the code base
  * contains a URL or host literal that is not in tests/privacy/network_allowlist.json
    (or is used from a file the allowlist does not name for that host),
  * uses a networking API (Qt network classes, Python sockets/urllib...) in a file that
    the inventory does not list,
  * references a known analytics / advertising / crash-reporting SDK or a web engine,
  * (YouTube Music account profile) builds Cookie/Authorization headers or names session cookies
    outside the reviewed transport/account paths, lets the resolver child code reach any account
    or cookie code, lets backup/diagnostics/settings/journal code touch session material, logs
    credential-named values, or lets the Harbour/default profile carry the Secrets wiring or
    permission (see the "ytm_profile", "session_material", "resolver_isolation",
    "persistence_sinks" blocks of the allowlist and docs/PRIVACY_NETWORK.md).

Only string literals and identifiers in real code are examined: comments are
removed first (a URL inside a comment makes no request). Dynamic hosts (user
configured endpoints, resolved media URLs, LAN receivers) cannot be seen
statically; they are declared under "dynamic_network_sources" and documented in
docs/PRIVACY_NETWORK.md. This is a static aid, not a proxy capture.

Usage:  check_network_inventory.py [--root DIR] [--allowlist FILE] [--list] [--strict]
Exit:   0 clean, 1 violations (or stale entries with --strict), 2 usage/IO error.
"""
import argparse
import fnmatch
import json
import os
import re
import sys

SCAN_DIRS = ("Core", "Clients", "Runtime")
CODE_EXT = {".cpp": "c", ".h": "c", ".hpp": "c", ".c": "c", ".cc": "c", ".qml": "c", ".js": "c", ".py": "py"}
BUILD_FILES = ("sailtune.pro",)
SPEC_DIR = "rpm"
NETWORK_SCHEMES = ("http", "https", "ws", "wss", "ftp", "ftps", "sftp", "rtsp", "rtmp", "mms", "udp", "tcp")
TLDS = "com|net|org|io|tv|fm|co|dev|app|me|gov|edu|info|xyz|ru|cn"
URL_RE = re.compile(r"\b([A-Za-z][A-Za-z0-9+.\-]*)://(?:[^/@\s\"']*@)?(\[[0-9a-fA-F:]+\]|[A-Za-z0-9.\-%_]+)")
BARE_RE = re.compile(r"(?<![\w.\-/@])(\.?(?:[a-z0-9\-]+\.)+(?:%s))(?![\w\-])" % TLDS, re.I)
NETWORK_API_RE = re.compile(
    r"\bQNetworkAccessManager\b|\bQNetworkRequest\b|\bQNetworkReply\b|\bQTcpSocket\b|\bQUdpSocket\b|\bQTcpServer\b|"
    r"\bQWebSocket\b|\bQSslSocket\b|\bQNetworkDatagram\b|\bQHostInfo\b|XMLHttpRequest|"
    r"^\s*(?:import|from)\s+(?:urllib|http|requests|socket|ssl|ftplib|smtplib|aiohttp|websocket)\b", re.M)
FORBIDDEN = [
    "firebase", "crashlytics", "sentry", "mixpanel", "amplitude", "appcenter", "google-analytics", "googletagmanager",
    "doubleclick", "admob", "appsflyer", "flurry", "bugsnag", "countly", "matomo", "posthog", "newrelic", "segment.io",
    "adjust.com", "branch.io", "onesignal", "telemetry", "analytics", "googlesyndication",
]
FORBIDDEN_QT = ("webengine", "webview", "webkit")

# Source patterns of the form "dir/**" (everything below dir/) or fnmatch globs ("*" also crosses "/")
# are accepted wherever the allowlist names files, so a feature that lives in a directory
# (Core/Ytm/Transport/**) is reviewed as a unit. Patterns whose directory holds no code yet are
# reported as pending notes, not as stale entries, until the first file lands.
PENDING = []


def is_glob(pattern):
    return pattern.endswith("/**") or any(c in pattern for c in "*?[")


def glob_match(rel, pattern):
    if pattern.endswith("/**"):
        return rel.startswith(pattern[:-2])
    if is_glob(pattern):
        return fnmatch.fnmatchcase(rel, pattern)
    return rel == pattern


def matches_any(rel, patterns):
    return any(glob_match(rel, p) for p in patterns)


def words(text):
    """Lower-case words of identifiers (camelCase and snake_case split), space padded."""
    spaced = re.sub(r"(?<=[a-z0-9])(?=[A-Z])", " ", text)
    return " " + re.sub(r"[^a-z0-9]+", " ", spaced.lower()) + " "


def has_term(text, term):
    return (" " + re.sub(r"[^a-z0-9]+", " ", term.lower()).strip() + " ") in words(text)


def strip_comments_and_collect(text, lang, keep_strings=False):
    """Returns (code_without_comments_and_strings, [string literals]).
    With keep_strings the literals stay in the code (quotes included) so a call and its
    literal argument can be matched together; comments are removed either way."""
    code, literals = [], []
    i, n = 0, len(text)
    while i < n:
        c = text[i]
        nxt = text[i + 1] if i + 1 < n else ""
        if lang == "c" and c == "/" and nxt == "/":
            while i < n and text[i] != "\n":
                i += 1
        elif lang == "c" and c == "/" and nxt == "*":
            j = text.find("*/", i + 2)
            i = n if j < 0 else j + 2
        elif lang == "py" and c == "#":
            while i < n and text[i] != "\n":
                i += 1
        elif lang == "py" and text.startswith(('"""', "'''"), i):
            q = text[i:i + 3]
            j = text.find(q, i + 3)
            j = n if j < 0 else j
            literals.append(text[i + 3:j])
            if keep_strings:
                code.append(text[i:j + 3])
            i = j + 3
        elif c in "\"'" and not (lang == "c" and c == "'" and not _quote_is_string(text, i)):
            j = i + 1
            buf = []
            while j < n and text[j] != c and text[j] != "\n":
                if text[j] == "\\" and j + 1 < n:
                    buf.append(text[j + 1]); j += 2
                else:
                    buf.append(text[j]); j += 1
            literals.append("".join(buf))
            if keep_strings:
                code.append(text[i:j + 1])
            i = j + 1
        else:
            code.append(c)
            i += 1
    return "".join(code), literals


def _quote_is_string(text, i):
    # C++ char literal 'x' or '\n' is not a URL carrier; QML/JS single quotes are strings.
    return not re.match(r"'(?:\\.|[^\\'])'", text[i:i + 6])


def iter_files(root):
    for top in SCAN_DIRS:
        for dirpath, dirnames, filenames in os.walk(os.path.join(root, top)):
            dirnames[:] = [d for d in dirnames if not d.startswith(".")]
            for name in filenames:
                ext = os.path.splitext(name)[1].lower()
                if ext in CODE_EXT:
                    full = os.path.join(dirpath, name)
                    yield os.path.relpath(full, root).replace(os.sep, "/"), CODE_EXT[ext], full


def read(path):
    with open(path, "r", encoding="utf-8", errors="replace") as handle:
        return handle.read()


def host_of_literal(literal):
    found = []
    for m in URL_RE.finditer(literal):
        scheme, host = m.group(1).lower(), m.group(2)
        if scheme not in NETWORK_SCHEMES:   # image://theme/, qrc:, file:, dbus-style names...
            continue
        if host.startswith("["):
            host = host.strip("[]")
        else:
            host = host.split(":")[0] if host.count(":") == 1 else host
        host = host.strip(".") if not host.startswith(".") else host
        if "%" in host:
            found.append((scheme, "<dynamic>"))
        elif host:
            found.append((scheme, host.lower()))
    stripped = URL_RE.sub(" ", literal)
    for m in BARE_RE.finditer(stripped):
        found.append(("bare", m.group(1).lower()))
    return found


SPEC_BAD_COMMAND_RE = re.compile(r"(?<![\w-])(curl|wget|nc|ncat|telnet|ssh|scp|rsync|ftp|tftp|git\s+clone|git\s+fetch|pip3?\s+install|npm\s+install|gem\s+install)(?![\w-])")
QML_IMPORT_RE = re.compile(r"^\s*import\s+([A-Za-z_][A-Za-z0-9_.]*)", re.M)
QML_WEBVIEW_RE = re.compile(r"\bWebView\s*\{|\bWebSocket\s*\{|\bWebSocketServer\s*\{")
OPEN_URL_RE = re.compile(r"\bQt\.openUrlExternally\b")
QT_MODULE_RE = re.compile(r"^\s*QT\s*\+=\s*(.*)$", re.M)


def _strip_hash_comments(text):
    return "\n".join(l for l in text.splitlines() if not l.strip().startswith("#"))


def parse_desktop(text):
    """{(section, key): value} of a .desktop file; comments and blank lines are ignored."""
    out, section = {}, ""
    for raw in text.splitlines():
        line = raw.strip()
        if not line or line.startswith("#"):
            continue
        if line.startswith("["):
            section = line
        elif "=" in line:
            key, value = line.split("=", 1)
            out[(section, key.strip())] = value.strip()
    return out


def desktop_permissions(kv):
    return set(p.strip() for p in kv.get(("[X-Sailjail]", "Permissions"), "").split(";") if p.strip())


def analyse_permissions(root, allow):
    """Permission and capability baseline: Sailjail permissions, Qt modules, rpm
    Requires, QML imports and external URL launches must match the reviewed
    baseline in the allowlist ('build_permissions'). A change needs a deliberate
    edit of the allowlist together with docs/PRIVACY_NETWORK.md."""
    base = allow.get("build_permissions")
    errors, stale = [], []
    if base is None:
        return ["allowlist has no 'build_permissions' baseline (permission changes would go unnoticed)"], stale

    def compare(label, found, expected):
        for item in sorted(found - expected):
            errors.append("%s: %r is not in the reviewed baseline (build_permissions)" % (label, item))
        for item in sorted(expected - found):
            stale.append("build_permissions %s lists %r, which is no longer present" % (label, item))

    # Sailjail permissions in every desktop file. Files listed under ytm_profile.desktop_variants
    # are profile variants (installed only by an opt-in build profile); they are checked against
    # the base file below and never widen the base/Harbour baseline.
    prof = allow.get("ytm_profile", {})
    variants = prof.get("desktop_variants", {})
    profile_only = set(prof.get("profile_only_permissions", []))
    found, desktops = set(), {}
    for dirpath, dirnames, filenames in os.walk(root):
        dirnames[:] = [d for d in dirnames if not d.startswith(".") and d not in ("vendor", "node_modules")]
        for name in filenames:
            if not name.endswith(".desktop"):
                continue
            full = os.path.join(dirpath, name)
            rel = os.path.relpath(full, root).replace(os.sep, "/")
            kv = parse_desktop(read(full))
            desktops[rel] = kv
            if rel in variants:
                continue
            found.update(desktop_permissions(kv))
    compare("sailjail permission", found, set(base.get("sailjail_permissions", [])))
    for item in sorted(found & profile_only):
        errors.append("a base/Harbour desktop file requests the profile-only permission %r" % item)
    if variants:
        base_rel = prof.get("base_desktop", "harbour-sailtune.desktop")
        base_kv = desktops.get(base_rel)
        if base_kv is None:
            errors.append("ytm_profile.base_desktop %s does not exist" % base_rel)
        for rel, vspec in sorted(variants.items()):
            kv = desktops.get(rel)
            if kv is None:
                stale.append("ytm_profile.desktop_variants lists missing file %s" % rel)
                continue
            if base_kv is None:
                continue
            perm_key = ("[X-Sailjail]", "Permissions")
            for key in sorted(set(base_kv) | set(kv)):
                if key != perm_key and base_kv.get(key) != kv.get(key):
                    errors.append("%s differs from %s outside Permissions (%s)" % (rel, base_rel, "/".join(key)))
            expected = desktop_permissions(base_kv) | set(vspec.get("extra_permissions", []))
            actual = desktop_permissions(kv)
            if actual != expected:
                errors.append("%s requests %s, expected the base permissions plus %s" % (
                    rel, sorted(actual), sorted(vspec.get("extra_permissions", []))))
            for item in sorted(actual - desktop_permissions(base_kv)):
                if item not in profile_only:
                    errors.append("%s adds %r, which is not declared in ytm_profile.profile_only_permissions" % (rel, item))

    # Qt modules.
    modules = set()
    pro = os.path.join(root, BUILD_FILES[0])
    if os.path.exists(pro):
        for m in QT_MODULE_RE.finditer(_strip_hash_comments(read(pro))):
            modules.update(m.group(1).replace("\\", " ").split())
        # continuation lines of a QT += block
        text = _strip_hash_comments(read(pro))
        for m in re.finditer(r"^\s*QT\s*\+=\s*((?:[^\n]*\\\n)*[^\n]*)", text, re.M):
            modules.update(w for w in m.group(1).replace("\\", " ").split() if re.match(r"^[A-Za-z0-9_\-]+$", w))
    compare("qmake QT module", modules, set(base.get("qt_modules", [])))

    # rpm: Requires/BuildRequires, URLs outside metadata, download/execute commands.
    requires, spec_dir = set(), os.path.join(root, SPEC_DIR)
    if os.path.isdir(spec_dir):
        for name in sorted(os.listdir(spec_dir)):
            if not name.endswith(".spec"):
                continue
            in_changelog = False
            for raw in read(os.path.join(spec_dir, name)).splitlines():
                line = raw.strip()
                if line.startswith("%changelog"):
                    in_changelog = True
                if in_changelog or not line or line.startswith("#"):
                    continue
                if re.match(r"^(Build)?Requires(\(\w+\))?\s*:", line):
                    requires.add(re.sub(r"\s+", " ", line))
                    continue
                if re.match(r"^(URL|Url|Source\d*|Patch\d*|BugURL)\s*:", line):
                    continue
                if re.search(r"\b[A-Za-z][A-Za-z0-9+.\-]*://", line):
                    errors.append("rpm/%s: URL outside URL:/Source: metadata: %r" % (name, line[:80]))
                if SPEC_BAD_COMMAND_RE.search(line):
                    errors.append("rpm/%s: network/download command in a scriptlet: %r" % (name, line[:80]))
    compare("rpm requirement", requires, set(base.get("spec_requires", [])))

    # QML: imports, web views, external URL launches.
    qml_imports, open_files, webview_used = set(), set(), set()
    forbidden = tuple(base.get("qml_forbidden_import_prefixes", []))
    webview_files = prof.get("qml_webview_files", [])
    webview_imports = set(prof.get("webview_imports", []))
    for rel, lang, full in iter_files(root):
        if not rel.endswith(".qml"):
            continue
        text = read(full)
        webview_ok = matches_any(rel, webview_files)
        for m in QML_IMPORT_RE.finditer(text):
            module = m.group(1)
            if webview_ok and module in webview_imports:
                webview_used.add(rel)
                continue
            qml_imports.add(module)
            if forbidden and module.startswith(forbidden):
                errors.append("%s: QML import %r gives network/web access and is forbidden" % (rel, module))
        code, _ = strip_comments_and_collect(text, "c")
        for m in QML_WEBVIEW_RE.finditer(code):
            if webview_ok and m.group(0).lstrip().startswith("WebView"):
                webview_used.add(rel)
                continue
            errors.append("%s: QML WebView/WebSocket element" % rel)
            break
        if OPEN_URL_RE.search(code):
            open_files.add(rel)
    if webview_used:
        # An embedded web sign-in needs its Sailjail permission in the profile variant, and the
        # permission must be declared as profile-only so the base/Harbour file never gains it.
        if "WebView" not in profile_only:
            errors.append("embedded web sign-in files %s need WebView in ytm_profile.profile_only_permissions" % sorted(webview_used))
        if not any("WebView" in desktop_permissions(desktops.get(v, {})) for v in variants):
            errors.append("embedded web sign-in files %s need the WebView permission in a desktop variant" % sorted(webview_used))
    for pat in webview_files:
        if not any(glob_match(r, pat) for r in webview_used) and not is_glob(pat):
            stale.append("ytm_profile.qml_webview_files lists %s, which no longer uses a web view" % pat)
    compare("QML import", qml_imports, set(base.get("qml_imports", [])))
    for rel in sorted(open_files - set(base.get("qml_external_open_files", []))):
        errors.append("%s: Qt.openUrlExternally is not in the reviewed baseline (qml_external_open_files)" % rel)
    for rel in sorted(set(base.get("qml_external_open_files", [])) - open_files):
        stale.append("build_permissions qml_external_open_files lists %s, which no longer opens URLs" % rel)
    return errors, stale


# --------------------------------------------------------------------------------------------
# YouTube Music account profile: where session material may live (docs/PRIVACY_NETWORK.md)
# --------------------------------------------------------------------------------------------
INCLUDE_RE = re.compile(r'#\s*include\s*[<"]([^>"]+)[>"]')
LITERAL_NAME = r'(?:(?:QByteArray(?:Literal)?|QStringLiteral|QLatin1String|QString|std::string)\s*\(\s*)?'
HEADER_CALL_RE = re.compile(
    r"\b(?:setRawHeader|setRequestHeader|add_header|addHeader|insertHeader|putHeader)\s*\(\s*" + LITERAL_NAME +
    r"[\"']([^\"']*)[\"']", re.I)
DYNAMIC_RAW_HEADER_RE = re.compile(r"\bsetRawHeader\s*\(\s*(?![\"']|(?:QByteArray(?:Literal)?|QStringLiteral|QLatin1String)\s*\(\s*[\"'])")
HEADER_LINE_RE = re.compile(r"^\s*(?:cookie|set-cookie|authorization|proxy-authorization)\s*:\s*\S", re.I)
COOKIE_ID_RE = re.compile(r"\b(?:Set)?CookieHeader\b")
COOKIE_API_RE = re.compile(
    r"\bQNetworkCookie::parseCookies\b|[.>]\s*(?:insertCookie|setAllCookies|deleteCookie|setCookiesFromUrl)\s*\(|"
    r"\bQNetworkCookie\s*[({]|\bQNetworkCookie\s+\w+\s*[({=]")
ENV_INSERT_RE = re.compile(r"\benv\w*\s*\.\s*insert\s*\(\s*(?:QStringLiteral\s*\(\s*)?\"([^\"]*)\"", re.I)
LOG_STATEMENT_RES = (
    re.compile(r"\bq(?:Debug|Info|Warning|Critical|Fatal)\s*\([^;]*;"),
    re.compile(r"\bqC(?:Debug|Info|Warning|Critical)\s*\([^;]*;"),
    re.compile(r"\bfprintf\s*\(\s*std(?:err|out)[^;]*;"),
    re.compile(r"\bstd::c(?:err|out|log)\s*<<[^;]*;"),
    re.compile(r"\bthrow\b[^;]*;"),
    re.compile(r"\bconsole\.\w+\s*\([^;\n]*"),
)


def _cfg(allow, key):
    cfg = allow.get(key)
    return cfg if cfg else None


def _bearer_only(code_s):
    """True when every Authorization header the file sets is an OAuth Bearer value."""
    calls = re.findall(r"setRawHeader\s*\(\s*\"Authorization\"\s*,([^;]*);", code_s, re.I)
    return bool(calls) and all('"Bearer ' in c for c in calls)


def analyse_session_material(root, allow, all_files):
    """Cookie/Authorization headers and session cookie names stay in reviewed paths."""
    cfg = _cfg(allow, "session_material")
    errors, stale = [], []
    if not cfg:
        return errors, stale
    header_ok = cfg.get("header_construction_allowed_paths", [])
    names = set(n.lower() for n in cfg.get("forbidden_header_names", []))
    bearer_only = cfg.get("bearer_only_exceptions", {})
    dynamic_ok = cfg.get("dynamic_raw_header_files", {})
    term_ok = cfg.get("session_terms_allowed_paths", [])
    terms = [t.lower() for t in cfg.get("session_terms", [])]
    seen_bearer, seen_dynamic = set(), set()
    for rel, lang, full in all_files:
        text = read(full)
        code_s, lits = strip_comments_and_collect(text, lang, keep_strings=True)
        code_ns, _ = strip_comments_and_collect(text, lang)
        if not matches_any(rel, header_ok):
            for m in HEADER_CALL_RE.finditer(code_s):
                name = m.group(1).strip().lower()
                if name not in names:
                    continue
                if name == "authorization" and rel in bearer_only and _bearer_only(code_s):
                    seen_bearer.add(rel)
                    continue
                errors.append("%s: sets a %s header; only %s may build credential headers" % (
                    rel, m.group(1).strip(), ", ".join(header_ok) or "the reviewed session paths"))
            if COOKIE_ID_RE.search(code_ns):
                errors.append("%s: uses a Cookie header accessor outside the reviewed session paths" % rel)
            if COOKIE_API_RE.search(code_ns):
                errors.append("%s: builds or stores cookies outside the reviewed session paths" % rel)
            for lit in lits:
                if HEADER_LINE_RE.match(lit):
                    errors.append("%s: string literal looks like a Cookie/Authorization header line: %r" % (rel, lit[:40]))
                    break
            if DYNAMIC_RAW_HEADER_RE.search(code_s):
                if rel in dynamic_ok:
                    seen_dynamic.add(rel)
                else:
                    errors.append("%s: sets a header whose name is computed at run time; list it in session_material.dynamic_raw_header_files after review" % rel)
        if not matches_any(rel, term_ok):
            low = code_s.lower()
            for t in terms:
                if t in low:
                    errors.append("%s: names the session credential %r outside the reviewed session paths" % (rel, t))
                    break
    for f in bearer_only:
        if f not in seen_bearer:
            stale.append("session_material.bearer_only_exceptions lists %s, which no longer sets a Bearer header" % f)
    for f in dynamic_ok:
        if f not in seen_dynamic:
            stale.append("session_material.dynamic_raw_header_files lists %s, which no longer forwards computed headers" % f)
    return errors, stale


def analyse_resolver_isolation(root, allow, all_files):
    """The yt-dlp / QuickJS resolver child code never receives cookies or account material,
    and account/YouTube Music code never starts or feeds a child process."""
    cfg = _cfg(allow, "resolver_isolation")
    errors, stale = [], []
    if not cfg:
        return errors, stale
    resolver = cfg.get("files", [])
    account = cfg.get("account_code", [])
    bad_includes = cfg.get("forbidden_includes", [])
    bad_ids = [re.compile(r"\b%s\b" % re.escape(i)) for i in cfg.get("forbidden_identifiers", [])]
    bad_lits = set(l.lower() for l in cfg.get("forbidden_literals", []))
    bad_prefixes = tuple(p.lower() for p in cfg.get("forbidden_literal_prefixes", []))
    env_words = [w.lower() for w in cfg.get("forbidden_environment_words", [])]
    account_bad_ids = [re.compile(r"\b%s\b" % re.escape(i)) for i in cfg.get("account_forbidden_identifiers", [])]
    account_bad_includes = cfg.get("account_forbidden_includes", [])
    seen_resolver = 0
    for rel, lang, full in all_files:
        in_resolver, in_account = matches_any(rel, resolver), matches_any(rel, account)
        if not (in_resolver or in_account):
            continue
        text = read(full)
        code_s, lits = strip_comments_and_collect(text, lang, keep_strings=True)
        code_ns, _ = strip_comments_and_collect(text, lang)
        if in_resolver:
            seen_resolver += 1
            for m in INCLUDE_RE.finditer(code_s):
                if any(frag in m.group(1) for frag in bad_includes):
                    errors.append("%s: resolver child code includes %r, which can reach account or cookie material" % (rel, m.group(1)))
            for rx in bad_ids:
                if rx.search(code_ns):
                    errors.append("%s: resolver child code references %s" % (rel, rx.pattern.strip(r"\b").replace("\\", "")))
            for lit in lits:
                low = lit.strip().lower()
                if low in bad_lits or (bad_prefixes and low.startswith(bad_prefixes)):
                    errors.append("%s: resolver child code names the credential option or header %r" % (rel, lit[:40]))
            for m in ENV_INSERT_RE.finditer(code_s):
                key = m.group(1).lower()
                if any(w in key for w in env_words):
                    errors.append("%s: resolver child environment variable %r looks credential-related" % (rel, m.group(1)))
        if in_account:
            for m in INCLUDE_RE.finditer(code_s):
                if any(frag in m.group(1) for frag in account_bad_includes):
                    errors.append("%s: account/YouTube Music code includes %r; it must never reach the resolver or a child process" % (rel, m.group(1)))
            for rx in account_bad_ids:
                if rx.search(code_ns):
                    errors.append("%s: account/YouTube Music code references %s" % (rel, rx.pattern.strip(r"\b").replace("\\", "")))
    if resolver and not seen_resolver:
        stale.append("resolver_isolation.files matches no source file")
    return errors, stale


def analyse_persistence_sinks(root, allow, all_files):
    """Backup, diagnostics, settings and journal/snapshot code cannot serialize session material,
    and code that could log or throw it must not name credential values."""
    cfg = _cfg(allow, "persistence_sinks")
    errors, stale = [], []
    if not cfg:
        return errors, stale
    sinks = cfg.get("files", [])
    redactors = cfg.get("redactor_files", [])
    bad_includes = cfg.get("forbidden_includes", [])
    bad_ids = [re.compile(r"\b%s\b" % re.escape(i)) for i in cfg.get("forbidden_identifiers", [])]
    bad_terms = [t.lower() for t in cfg.get("forbidden_terms", [])]
    log_scope = cfg.get("log_scan_files", [])
    log_words = re.compile(cfg.get("log_forbidden_words", r"cookie|sapisid|authorization|bearer|secret"), re.I)
    seen = 0
    for rel, lang, full in all_files:
        in_sink = matches_any(rel, sinks) and not matches_any(rel, redactors)
        in_log = matches_any(rel, log_scope)
        if not (in_sink or in_log):
            continue
        text = read(full)
        code_s, lits = strip_comments_and_collect(text, lang, keep_strings=True)
        code_ns, _ = strip_comments_and_collect(text, lang)
        if in_sink:
            seen += 1
            for m in INCLUDE_RE.finditer(code_s):
                if any(frag in m.group(1) for frag in bad_includes):
                    errors.append("%s: persistence/diagnostics code includes %r, which carries session material" % (rel, m.group(1)))
            for rx in bad_ids:
                if rx.search(code_ns):
                    errors.append("%s: persistence/diagnostics code references the session type %s" % (rel, rx.pattern.strip(r"\b").replace("\\", "")))
            low = code_s.lower()
            for t in bad_terms:
                if t in low:
                    errors.append("%s: persistence/diagnostics code names the session credential %r" % (rel, t))
                    break
        if in_log:
            for rx in LOG_STATEMENT_RES:
                for m in rx.finditer(code_ns):
                    hit = log_words.search(m.group(0))
                    if hit:
                        errors.append("%s: a log/throw statement names %r; credential values must never reach logs or exception text: %s" % (
                            rel, hit.group(0), " ".join(m.group(0).split())[:70]))
    if sinks and not seen:
        stale.append("persistence_sinks.files matches no source file")
    return errors, stale


def _strip_hash(line):
    out, quoted = [], False
    for ch in line:
        if ch == '"':
            quoted = not quoted
        if ch == "#" and not quoted:
            break
        out.append(ch)
    return "".join(out)


def strip_optin_blocks(lines, tokens):
    """The text of a qmake project file without the blocks whose opening line (ends with '{')
    contains one of `tokens`; nested blocks of an exempt block are exempt too."""
    if not tokens:
        return "\n".join(lines)
    out, stack, exempt = [], [], 0
    for raw in lines:
        line = _strip_hash(raw).strip()
        if line.startswith("}") and stack:
            if stack.pop():
                exempt -= 1
            if not line.endswith("{"):
                continue
        opens = line.endswith("{")
        optin = opens and any(t in line for t in tokens)
        if not exempt and not optin:
            out.append(raw)
        if opens:
            stack.append(optin)
            if optin:
                exempt += 1
    return "\n".join(out)


def gate_pro_text(text, gates, guard_tokens):
    """Lines naming an opt-in feature must sit inside a block whose condition names it."""
    errors, stack = [], []
    for n, raw in enumerate(text.splitlines(), 1):
        line = _strip_hash(raw).strip()
        if not line:
            continue
        if line.startswith("}") and stack:
            stack.pop()
        context, low = " ".join(stack), line.lower()
        guard = any(t in line for t in guard_tokens)
        for gate in gates:
            needed = gate["require_block"]
            if not any(t.lower() in low for t in gate["terms"]):
                continue
            if needed in context or guard or (line.endswith("{") and needed in line):
                continue
            errors.append("sailtune.pro:%d: %r belongs inside a block conditioned on %s: %s" % (
                n, gate["terms"][0], needed, line[:70]))
        if line.endswith("{"):
            stack.append(line)
    return errors


def gate_spec_text(text, gates):
    errors, stack = [], []
    for n, raw in enumerate(text.splitlines(), 1):
        line = raw.strip()
        if line.startswith("%changelog"):
            break
        if not line or line.startswith("#"):
            continue
        if line.startswith("%if"):
            stack.append(line)
            continue
        if line.startswith("%elif") or line.startswith("%else"):
            if stack:
                stack[-1] = "NOT " + stack[-1]
            continue
        if line.startswith("%endif"):
            if stack:
                stack.pop()
            continue
        if line.startswith("%bcond"):
            continue
        positive = " ".join(c for c in stack if not c.startswith("NOT "))
        low = line.lower()
        for gate in gates:
            if any(t.lower() in low for t in gate["terms"]) and gate["require_block"] not in positive:
                errors.append("spec:%d: %r belongs inside a conditional on '%s': %s" % (n, gate["terms"][0], gate["require_block"], line[:70]))
    return errors


def analyse_profile_gates(root, allow, all_files):
    """The default and Harbour profiles carry no Secrets wiring or permission: every such line
    of sailtune.pro and of the rpm spec sits under the opt-in condition."""
    prof = allow.get("ytm_profile")
    errors, stale = [], []
    if not prof:
        return errors, stale
    pro = os.path.join(root, BUILD_FILES[0])
    if os.path.exists(pro):
        errors.extend(gate_pro_text(read(pro), prof.get("pro_gates", []), prof.get("pro_guard_tokens", [])))
    spec_dir = os.path.join(root, SPEC_DIR)
    if os.path.isdir(spec_dir):
        for name in sorted(os.listdir(spec_dir)):
            if not name.endswith(".spec"):
                continue
            text = read(os.path.join(spec_dir, name))
            errors.extend(gate_spec_text(text, prof.get("spec_gates", [])))
            if prof.get("spec_requires_exclusion") and not re.search(
                    r"%if\s+%\{with harbour\}\s*&&\s*%\{with secrets\}\s*\n\s*%\{error:", text):
                errors.append("rpm/%s: lacks the guard that rejects --with harbour together with --with secrets" % name)
    return errors, stale


def analyse_docs(root, allow, all_files):
    """docs/PRIVACY_NETWORK.md must name every host, every credential-carrying path and every
    profile-only permission of the allowlist (the page is the user-facing contract)."""
    path = os.path.join(root, "docs", "PRIVACY_NETWORK.md")
    errors, stale = [], []
    if not os.path.exists(path):
        return errors, stale
    doc = read(path)
    for host, entry in sorted(allow.get("hosts", {}).items()):
        if host not in doc:
            errors.append("docs/PRIVACY_NETWORK.md does not mention allowlisted host %r" % host)
        for pat in entry.get("authenticated_files", []):
            if pat not in doc:
                errors.append("docs/PRIVACY_NETWORK.md does not name the credential-carrying path %s (host %s)" % (pat, host))
    prof = allow.get("ytm_profile", {})
    for perm in prof.get("profile_only_permissions", []):
        if perm not in doc:
            errors.append("docs/PRIVACY_NETWORK.md does not mention the profile-only permission %s" % perm)
    return errors, stale



def host_patterns(entry):
    """Files that may mention the host: the reviewed request files, the credential-carrying
    (authenticated) files, and files that only name it (matching or pinning, no request)."""
    return (list(entry.get("files", [])) + list(entry.get("authenticated_files", [])) +
            list(entry.get("reference_files", [])))


def analyse(root, allow):
    del PENDING[:]
    hosts_allowed = allow.get("hosts", {})
    ignored = allow.get("ignored_hosts", {})
    api_files = allow.get("network_api_files", {})
    dyn_url_files = allow.get("dynamic_url_files", {})
    prof = allow.get("ytm_profile", {})
    webview_files = prof.get("qml_webview_files", [])
    webview_hosts = set(prof.get("webview_hosts", []))
    errors, used_hosts, used_api = [], {}, set()
    seen_hosts = {}
    all_files = list(iter_files(root))
    rels = [r for r, _, _ in all_files]
    for rel, lang, full in all_files:
        code, literals = strip_comments_and_collect(read(full), lang)
        for literal in literals:
            for scheme, host in host_of_literal(literal):
                if host in ignored and rel in ignored[host].get("files", []):
                    used_hosts.setdefault(("ignored", host), set()).add(rel)
                    continue
                if host == "<dynamic>":
                    if rel not in dyn_url_files:
                        errors.append("%s: URL with a runtime-built host in a literal (declare in dynamic_url_files): %r" % (rel, literal[:80]))
                    continue
                seen_hosts.setdefault(host, set()).add(rel)
                entry = hosts_allowed.get(host)
                if entry is None:
                    errors.append("%s: host %r (%s) is not in the network allowlist" % (rel, host, scheme))
                elif (not entry.get("any_file") and not matches_any(rel, host_patterns(entry)) and
                      not (host in webview_hosts and matches_any(rel, webview_files))):
                    errors.append("%s: host %r is allowlisted but not for this file" % (rel, host))
                else:
                    used_hosts.setdefault(("host", host), set()).add(rel)
        if NETWORK_API_RE.search(code):
            if matches_any(rel, list(api_files)):
                used_api.add(rel)
            else:
                errors.append("%s: uses a networking API but is not listed in network_api_files" % rel)
        # identifiers only: user-visible text may say "no analytics"
        for term in FORBIDDEN:
            if has_term(code, term) and rel not in allow.get("forbidden_term_exceptions", {}).get(term, []):
                errors.append("%s: references forbidden analytics/ads term %r" % (rel, term))
    # Build description and packaging: no tracking SDK / web engine.
    pro = os.path.join(root, BUILD_FILES[0])
    if os.path.exists(pro):
        lines = [l for l in read(pro).splitlines() if not l.strip().startswith("#")]
        text = "\n".join(lines)
        # The embedded Google sign-in (Sailfish.WebView) is an opt-in profile: lines of a block whose
        # condition names an allowlisted opt-in token are not judged for web-engine terms. Every
        # other mention of such a term in the file is still an error.
        qt_text = strip_optin_blocks(lines, prof.get("pro_optin_webview_tokens", [])) if prof else text
        for term in FORBIDDEN:
            if has_term(text, term):
                errors.append("sailtune.pro: references forbidden term %r" % term)
        for term in FORBIDDEN_QT:
            if has_term(qt_text, term):
                errors.append("sailtune.pro: references forbidden term %r" % term)
    spec_dir = os.path.join(root, SPEC_DIR)
    if os.path.isdir(spec_dir):
        for name in sorted(os.listdir(spec_dir)):
            if name.endswith(".spec"):
                text = "\n".join(l for l in read(os.path.join(spec_dir, name)).splitlines() if not l.strip().startswith("#"))
                for term in FORBIDDEN:
                    if has_term(text, term):
                        errors.append("rpm/%s: references forbidden term %r" % (name, term))
    stale = []

    def check_pattern(label, pattern, used_files, exact_exists=None):
        """A listed file or pattern that nothing uses any more is stale; a pattern whose
        directory holds no code yet is pending (its slice has not landed)."""
        if any(glob_match(f, pattern) for f in used_files):
            return
        if is_glob(pattern) and not any(glob_match(r, pattern) for r in rels):
            PENDING.append("%s lists %s, which holds no source yet" % (label, pattern))
            return
        stale.append("%s lists %s, which no longer %s" % (label, pattern, exact_exists or "contains it"))

    for host, entry in hosts_allowed.items():
        if entry.get("any_file"):
            continue
        for f in list(entry.get("files", [])) + list(entry.get("authenticated_files", [])):
            check_pattern("allowlist host %r" % host, f, seen_hosts.get(host, set()))
    for host, entry in ignored.items():
        for f in entry.get("files", []):
            if ("ignored", host) not in used_hosts or f not in used_hosts[("ignored", host)]:
                stale.append("ignored_hosts %r lists %s, which no longer contains it" % (host, f))
    for f in api_files:
        check_pattern("network_api_files", f, used_api, "uses a network API")
    for f in dyn_url_files:
        if not os.path.exists(os.path.join(root, f)):
            stale.append("dynamic_url_files lists missing file %s" % f)
    perm_errors, perm_stale = analyse_permissions(root, allow) if "build_permissions" in allow else ([], [])
    errors.extend(perm_errors)
    stale.extend(perm_stale)
    for fn in (analyse_profile_gates, analyse_session_material, analyse_resolver_isolation,
               analyse_persistence_sinks, analyse_docs):
        e, st = fn(root, allow, all_files)
        errors.extend(e)
        stale.extend(st)
    return errors, stale, seen_hosts


def main(argv=None):
    here = os.path.dirname(os.path.abspath(__file__))
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--root", default=os.path.abspath(os.path.join(here, "..", "..")))
    ap.add_argument("--allowlist", default=os.path.join(here, "network_allowlist.json"))
    ap.add_argument("--list", action="store_true", help="print the discovered host inventory")
    ap.add_argument("--strict", action="store_true", help="stale allowlist entries are errors")
    args = ap.parse_args(argv)
    try:
        allow = json.loads(read(args.allowlist))
    except (OSError, ValueError) as exc:
        print("cannot read allowlist: %s" % exc, file=sys.stderr)
        return 2
    errors, stale, seen = analyse(args.root, allow)
    if args.list:
        for host in sorted(seen):
            print("%-32s %s" % (host, ", ".join(sorted(seen[host]))))
    errors = list(dict.fromkeys(errors))
    for note in PENDING:
        print("NOTE: " + note)
    for e in errors:
        print("ERROR: " + e)
    for s in stale:
        print(("ERROR: " if args.strict else "WARN: ") + s)
    if errors or (args.strict and stale):
        print("network inventory check FAILED (%d error(s), %d stale)" % (len(errors), len(stale)))
        return 1
    print("network inventory check OK (%d host(s), %d stale warning(s))" % (len(seen), len(stale)))
    return 0


if __name__ == "__main__":
    sys.exit(main())
