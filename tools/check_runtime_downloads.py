# SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
"""NFR-SEC-04 static check: shipped code has no path that downloads, installs
or runs executable content at run time.

SailTube has no helper download or update today (NFR-MNT-01 is a v1 item, not
built), so the check is strict: any code that could fetch, stage, unpack, mark
executable, load or run code from a writable location is a finding unless
tests/runtime_downloads/exceptions.json lists it with a reason. An exception
that no longer matches anything is an error too (the list stays small and
current). `--variant store` ignores exceptions marked "variant": "full", so a
FULL-only updater can be declared later while the STORE build stays clean.

Scanned: Core/, Clients/, Shared/ (*.cpp *.cc *.c *.h *.hpp *.py *.qml *.js *.sh),
skipping any directory named tests/test/fixtures; plus rpm/*.spec scriptlets.

Rules
  RD-URL        a release/asset download URL (GitHub releases, *.githubusercontent.com
                asset hosts, codeload, PyPI file hosts, ".../releases/download/...")
  RD-FLAG       a yt-dlp self-update / remote-component / plugin-dir flag literal
                (--update, -U, --update-to, --remote-components, --plugin-dirs)
  RD-GUARD      YtDlpRunner's common arguments lost --ignore-config, --no-plugin-dirs
                or --no-remote-components
  RD-EXEC-MODE  a file (not a directory) given an execute bit: setPermissions/chmod/
                fchmod/fchmodat/open/creat with Exe* flags, S_IX*, S_IRWX* or an octal
                mode with x bits; os.chmod/os.open/Path.chmod in Python; chmod +x in sh
  RD-CODE-WRITE a file write/copy/rename/extract whose target names an executable or
                code file (.pyz .py .pyc .so[.N] .dylib .zip .whl .egg .tar[.*] .tgz
                .jar .js .mjs .qml .sh .bin .AppImage .rpm .deb .exe), or any write into
                the install/runtime directory (runtimeFile, NORDTILE_RUNTIME_ROOT)
  RD-EXTRACT    archive extraction to disk (libarchive, QuaZip, KArchive, zipfile/tarfile
                extract/extractall, shutil.unpack_archive)
  RD-PROC       a process launch whose program does not come from the install
                (QProcess start/startDetached/execute/startHelper, exec*, posix_spawn,
                system, popen; subprocess/os.exec*/os.spawn*/os.system in Python)
  RD-DLOPEN     dlopen/QLibrary/QPluginLoader/ctypes on a path that is not a bare system
                library name or an install path
  RD-LOADPATH   a code search path from a non-install origin: QML import/plugin paths,
                library paths, LD_LIBRARY_PATH/LD_PRELOAD/PYTHONPATH/... in the
                environment, NORDTILE_WORKER_ARCHIVE, sys.path, site.addsitedir
  RD-PYLOAD     Python code loaded from a path: zipimport, importlib spec_from_file_location,
                runpy.run_path, exec/compile of file contents
  RD-QMLREMOTE  QML/JS that loads code from the network or evaluates text
                (remote Qt.include/createComponent/Loader source, createQmlObject of a
                non-literal, eval, Function)
  RD-SCRIPTLET  an RPM scriptlet that downloads or installs (curl, wget, pip, git)

Origins. A path expression is followed through local definitions, function
parameters and (for `x.field`) every assignment to that field in the scanned
tree. install = applicationFilePath/applicationDirPath, runtimeFile(),
NORDTILE_RUNTIME_ROOT, QLibraryInfo, Qt.resolvedUrl, __file__, sysconfig, /usr
literals; writable = QStandardPaths, temp/home paths, /tmp, ~, XDG; env =
getenv/qgetenv/os.environ; anything unresolved is unknown. Only install (and,
for libraries, a bare name the system linker resolves) passes.

Usage: python3 -I tools/check_runtime_downloads.py [--root DIR] [--exceptions FILE]
                                                   [--variant full|store] [--verbose]
Exit 0 clean, 1 findings or stale exceptions, 2 broken setup.
"""
import argparse
import ast
import json
import os
import re
import sys

CODE_EXTS = {".cpp", ".cc", ".c", ".h", ".hpp", ".qml", ".js", ".py", ".sh"}
SKIP_DIRS = {"tests", "test", "fixtures", "testdata", ".git"}
SCAN_ROOTS = ("Core", "Clients", "Shared")
RUNNER = "Core/Resolver/Infrastructure/YtDlpRunner.cpp"
RUNNER_GUARDS = ("--ignore-config", "--no-plugin-dirs", "--no-remote-components")

URL_RE = re.compile(
    r"(?i)(?:github\.com/[^\s\"'<>]*/releases\b|api\.github\.com/repos/|objects\.githubusercontent\.com"
    r"|release-assets\.githubusercontent\.com|github-releases\.githubusercontent\.com|raw\.githubusercontent\.com"
    r"|codeload\.github\.com|files\.pythonhosted\.org|pypi\.org/(?:simple|pypi)/|/releases/(?:latest/)?download/)")
CODE_FILE_RE = re.compile(
    r"(?i)\.(?:pyz|pyc|py|so(?:\.\d+)*|dylib|zip|whl|egg|tar(?:\.(?:gz|bz2|xz|zst))?|tgz|tbz2?|txz|jar|js|mjs"
    r"|qml|sh|bin|appimage|rpm|deb|exe)$")
LOAD_ENV = ("LD_LIBRARY_PATH", "LD_PRELOAD", "PYTHONPATH", "PYTHONHOME", "PYTHONSTARTUP", "PYTHONUSERBASE",
            "QML2_IMPORT_PATH", "QML_IMPORT_PATH", "QT_PLUGIN_PATH", "QT_QPA_PLATFORM_PLUGIN_PATH",
            "NORDTILE_WORKER_ARCHIVE")

INSTALL_TOKENS = ("applicationFilePath", "applicationDirPath", "NORDTILE_RUNTIME_ROOT", "runtimeFile",
                  "QLibraryInfo", "SailfishApp::pathTo", "resolvedUrl")
WRITABLE_TOKENS = ("writableLocation", "standardLocations", "QStandardPaths", "tempPath", "QDir::temp", "homePath",
                   "QDir::home", "QTemporaryDir", "QTemporaryFile", "StandardPaths", "XDG_", "TMPDIR")
ENV_TOKENS = ("qgetenv", "getenv", "qEnvironmentVariable", "QProcessEnvironment", "environment()")
INSTALL, WRITABLE, ENV, PARAM, UNKNOWN, SYSTEM = "install", "writable", "env", "parameter", "unknown", "system-library"
CHECKED = []   # sites that were analysed and passed (--verbose)
SAFE_PY_CALLS = {"os.getpid", "os.getppid", "os.getuid", "os.geteuid", "time.time", "time.monotonic", "int", "len",
                 "uuid.uuid4", "os.urandom"}   # values that cannot carry a path prefix
LAUNCH_WRAPPERS = {"startHelper", "start"}   # pass-through launchers: their callers are sites

CPP_SKIP_WORDS = {
    "QString", "QStringLiteral", "QLatin1String", "QByteArray", "QByteArrayLiteral", "QStringList", "QFile", "QDir",
    "QFileInfo", "QFileDevice", "QUrl", "const", "auto", "true", "false", "nullptr", "new", "return", "static_cast",
    "reinterpret_cast", "std", "string", "char", "int", "bool", "void", "this", "if", "else", "Qt", "QIODevice",
    "O_WRONLY", "O_CREAT", "O_EXCL", "O_TRUNC", "O_RDWR", "O_CLOEXEC", "O_NOFOLLOW", "O_APPEND", "QLatin1Char",
}
GENERIC_NAMES = {"path", "p", "s", "d", "f", "file", "info", "this", "name", "target", "out", "x"}


# ------------------------------------------------------------------ lexing helpers

def strip_c_comments(text):
    out, i, n = [], 0, len(text)
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
            out.append(text[i:j + 1])
            i = j + 1
        else:
            out.append(c)
            i += 1
    return "".join(out)


def literals(text):
    """(offset, content) of every string literal in comment-free C-like text."""
    out, i, n = [], 0, len(text)
    while i < n:
        c = text[i]
        if c == '"':
            j = i + 1
            while j < n and text[j] != '"' and text[j] != "\n":
                j += 2 if text[j] == "\\" else 1
            out.append((i, text[i + 1:j]))
            i = j + 1
        elif c == "'":
            j = i + 1
            while j < n and text[j] != "'" and text[j] != "\n":
                j += 2 if text[j] == "\\" else 1
            out.append((i, text[i + 1:j]))
            i = j + 1
        else:
            i += 1
    return out


def blank_strings(text):
    out, i, n = [], 0, len(text)
    while i < n:
        c = text[i]
        if c in "\"'":
            j = i + 1
            while j < n and text[j] != c and text[j] != "\n":
                j += 2 if text[j] == "\\" else 1
            out.append(c + " " * max(0, j - i - 1) + (c if j < n else ""))
            i = j + 1
        else:
            out.append(c)
            i += 1
    return "".join(out)


def balanced_args(code, open_paren):
    """Top-level comma-separated arguments of the call whose '(' is at open_paren."""
    depth, i, n, start, args = 0, open_paren, len(code), open_paren + 1, []
    while i < n:
        c = code[i]
        if c in "([{":
            depth += 1
        elif c in ")]}":
            depth -= 1
            if depth == 0:
                args.append((start, i))
                return args, i
        elif c == "," and depth == 1:
            args.append((start, i))
            start = i + 1
        elif c == ";" and depth <= 1:
            break
        i += 1
    return args, i


def line_of(text, offset):
    return text.count("\n", 0, offset) + 1


def source_line(raw_lines, line):
    return raw_lines[line - 1].strip() if 0 < line <= len(raw_lines) else ""


# ------------------------------------------------------------------ the tree

class Tree:
    def __init__(self, root):
        self.root = root
        self.files = {}       # rel -> dict(raw, text (no comments), code (no strings))
        for top in SCAN_ROOTS:
            base = os.path.join(root, top)
            for dirpath, dirnames, filenames in os.walk(base):
                dirnames[:] = sorted(d for d in dirnames if d not in SKIP_DIRS)
                for name in sorted(filenames):
                    if os.path.splitext(name)[1] not in CODE_EXTS:
                        continue
                    path = os.path.join(dirpath, name)
                    rel = os.path.relpath(path, root).replace(os.sep, "/")
                    raw = open(path, encoding="utf-8", errors="replace").read()
                    entry = {"raw": raw, "lines": raw.split("\n")}
                    if rel.endswith((".py", ".sh")):
                        entry["text"] = raw
                    else:
                        entry["text"] = strip_c_comments(raw)
                        entry["code"] = blank_strings(entry["text"])
                    self.files[rel] = entry
        self._fields = None

    def c_files(self):
        return [r for r in self.files if not r.endswith((".py", ".sh"))]

    def field_assignments(self, field):
        if self._fields is None:
            self._fields = {}
            pat = re.compile(r"(?:\.|->)\s*([A-Za-z_]\w*)\s*=(?!=)\s*([^;]+);")
            for rel in self.c_files():
                text = self.files[rel]["text"]
                for m in pat.finditer(text):
                    self._fields.setdefault(m.group(1), []).append((rel, m.start(2), m.group(2)))
        return self._fields.get(field, [])


# ------------------------------------------------------------------ C-like origin analysis

def enclosing_function(text, offset):
    """(name, parameter text) of the top-level function around offset (column-0 definitions)."""
    head = text[:offset]
    for m in reversed(list(re.finditer(r"(?m)^[A-Za-z_][^\n;{}#]*?\b([A-Za-z_]\w*)\s*\(([^)]*)\)[^;{]*\{", head))):
        return m.group(1), m.group(2)
    return None, ""


def definitions(text, ident, before):
    """Right-hand sides that define `ident` in this file (declarations and assignments)."""
    out = []
    pats = [
        re.compile(r"(?<![\w.>])" + re.escape(ident) + r"\s*=(?!=)\s*([^;]+);"),
        re.compile(r"\b(?:QString|QByteArray|QStringList|QDir|QFileInfo|QUrl|auto|std::string)\s*&?\s*"
                   + re.escape(ident) + r"\s*[({]([^;]*)[)}]\s*;"),
    ]
    for p in pats:
        for m in p.finditer(text):
            out.append((m.start(1), m.group(1)))
    return out


def classify_literal(s):
    if s.startswith(("/usr/", "/opt/", "/system/")):
        return INSTALL
    if s.startswith(("/tmp", "/var/", "/home/", "/run/", "~")) or "/.cache" in s or "/.local" in s:
        return WRITABLE
    if s.startswith(("qrc:", ":/")):
        return INSTALL
    return None


def c_origins(tree, rel, expr, offset, depth=0, seen=None):
    """Set of origin classes of a C-like expression."""
    seen = set() if seen is None else seen
    origins = set()
    entry = tree.files[rel]
    text = entry["text"]
    if depth > 6:
        return {UNKNOWN}
    code = blank_strings(expr)
    lits = [c for _, c in literals(expr)]
    for tok in INSTALL_TOKENS:
        if tok in code:
            origins.add(INSTALL)
    for tok in WRITABLE_TOKENS:
        if tok in code or any(tok in l for l in lits):
            origins.add(WRITABLE)
    for tok in ENV_TOKENS:
        if tok in code:
            origins.add(ENV)
    stripped = code.strip()
    if lits and stripped.startswith(('"', "QStringLiteral(\"", "QLatin1String(\"", "QString::fromLatin1(\"")):
        cls = classify_literal(lits[0])
        if cls:
            origins.add(cls)
    if origins:
        return origins
    chains = list(re.finditer(r"(?<![\w.>:])([A-Za-z_]\w*(?:\s*(?:\.|->|::)\s*[A-Za-z_]\w*)*)(\s*\()?", code))
    resolved_any = False
    fname, params = enclosing_function(text, offset)
    param_names = set(re.findall(r"([A-Za-z_]\w*)\s*(?:=[^,]*)?(?:,|$)", params))
    for m in chains:
        chain, call = m.group(1), m.group(2)
        parts = re.split(r"\s*(?:\.|->|::)\s*", chain)
        if call:
            # A call: its arguments are chains of their own; a known name was handled above.
            if m.end() < len(code) and code[m.end():].lstrip().startswith(")"):
                if parts[-1] not in ("path", "canonicalPath", "absolutePath", "filePath", "absoluteFilePath",
                                     "canonicalFilePath", "toString", "constData", "toLocalFile", "fileName"):
                    origins.add(UNKNOWN)
                    resolved_any = True
                    continue
            if len(parts) > 1 and parts[-1] in ("path", "canonicalPath", "absolutePath", "filePath",
                                                "absoluteFilePath", "canonicalFilePath", "toLocalFile"):
                sub = c_origins(tree, rel, ".".join(parts[:-1]), offset, depth + 1, seen)
                origins |= sub
                resolved_any = True
            continue
        if "::" in chain or parts[0] in CPP_SKIP_WORDS or parts[-1] in CPP_SKIP_WORDS:
            continue
        resolved_any = True
        key = (rel, chain)
        if key in seen:
            continue
        seen.add(key)
        if len(parts) > 1:
            field = parts[-1]
            assigns = tree.field_assignments(field)
            if assigns:
                for frel, foff, rhs in assigns:
                    origins |= c_origins(tree, frel, rhs, foff, depth + 1, seen)
                continue
            chain = parts[0]
        ident = chain
        if ident in param_names:
            origins.add(PARAM)
            continue
        defs = definitions(text, ident, offset)
        if not defs:
            assigns = tree.field_assignments(ident)
            if assigns:
                for frel, foff, rhs in assigns:
                    origins |= c_origins(tree, frel, rhs, foff, depth + 1, seen)
                continue
            origins.add(UNKNOWN)
            continue
        for doff, rhs in defs:
            origins |= c_origins(tree, rel, rhs, doff, depth + 1, seen)
    if not resolved_any and not origins:
        if lits:
            cls = classify_literal(lits[0])
            origins.add(cls or UNKNOWN)
        else:
            origins.add(UNKNOWN)
    return origins


def mode_has_exec(tree, rel, expr, depth=0):
    if re.search(r"Exe(?:Owner|User|Group|Other)|S_IX(?:USR|GRP|OTH)|S_IRWX[UGO]", expr):
        return True
    for lit in re.findall(r"(?<![\w.])0[0-7]{3,4}\b", expr):
        if int(lit, 8) & 0o111:
            return True
    if depth < 3:
        text = tree.files[rel]["text"]
        for ident in re.findall(r"(?<![\w.:>])([A-Za-z_]\w*)\b(?!\s*\()", expr):
            if ident in CPP_SKIP_WORDS or ident.startswith(("QFileDevice", "QFile")):
                continue
            for m in re.finditer(r"\b" + re.escape(ident) + r"\s*=\s*([^;]+);", text):
                if mode_has_exec(tree, rel, m.group(1), depth + 1):
                    return True
    return False


def looks_like_directory(tree, rel, target, mode_expr):
    if re.search(r"(?i)dir|directory|folder", target) or re.search(r"(?i)\b\w*dir\w*\b", mode_expr):
        return True
    text = tree.files[rel]["text"]
    statements = text.split(";")
    for ident in set(re.findall(r"[A-Za-z_]\w*", blank_strings(target))) - GENERIC_NAMES - CPP_SKIP_WORDS:
        pat = re.compile(r"\b" + re.escape(ident) + r"\b")
        for st in statements:
            if pat.search(st) and re.search(r"QTemporaryDir|mkpath|mkdir|QDir\s*\(", st):
                return True
    return False


def code_file_target(tree, rel, expr, depth=0, seen=None):
    """True when the target expression names a code/executable file (literal or via definitions)."""
    seen = set() if seen is None else seen
    for _, lit in literals(expr):
        if CODE_FILE_RE.search(lit.strip()):
            return True
    if depth >= 4:
        return False
    text = tree.files[rel]["text"]
    for ident in re.findall(r"(?<![\w.:>])([A-Za-z_]\w*)\b(?!\s*\()", blank_strings(expr)):
        if ident in CPP_SKIP_WORDS or ident in seen:
            continue
        seen.add(ident)
        for _, rhs in definitions(text, ident, 0):
            if code_file_target(tree, rel, rhs, depth + 1, seen):
                return True
    return False


# ------------------------------------------------------------------ C-like rules

def check_c(tree, rel, report):
    entry = tree.files[rel]
    text, code, lines = entry["text"], entry["code"], entry["lines"]
    is_qml = rel.endswith((".qml", ".js"))

    def add(rule, offset, detail):
        line = line_of(text, offset)
        report(rel, line, rule, detail, source_line(lines, line))

    for off, lit in literals(text):
        if URL_RE.search(lit):
            add("RD-URL", off, "release/asset download URL")
        if lit in ("--update", "-U", "--update-to") or lit.startswith(("--update-to=", "--remote-components",
                                                                       "--plugin-dirs")):
            add("RD-FLAG", off, "extractor self-update/remote component/plugin flag %s" % lit)
        if is_qml and re.match(r"(?i)https?://", lit) and CODE_FILE_RE.search(lit.split("?")[0]):
            add("RD-QMLREMOTE", off, "remote QML/JS code URL")

    if is_qml:
        for m in re.finditer(r"\bQt\s*\.\s*(include|createComponent|createQmlObject)\s*\(", code):
            args, _ = balanced_args(code, m.end() - 1)
            first = text[args[0][0]:args[0][1]].strip() if args else ""
            literal = first.startswith(("'", '"'))
            if m.group(1) == "createQmlObject" and not literal:
                add("RD-QMLREMOTE", m.start(), "createQmlObject of a non-literal")
            elif m.group(1) != "createQmlObject" and (not literal or re.match(r"['\"](?i:https?|file):", first)):
                add("RD-QMLREMOTE", m.start(), "Qt.%s of a non-relative source" % m.group(1))
        for m in re.finditer(r"(?<![\w.])(eval|Function)\s*\(", code):
            add("RD-QMLREMOTE", m.start(), "%s() evaluates text as code" % m.group(1))
        for m in re.finditer(r"\baddImportPath\s*\(", code):
            args, _ = balanced_args(code, m.end() - 1)
            if args:
                expr = text[args[0][0]:args[0][1]]
                o = c_origins(tree, rel, expr, m.start())
                if o - {INSTALL}:
                    add("RD-LOADPATH", m.start(), "import path from %s" % ",".join(sorted(o)))
                else:
                    CHECKED.append("%s:%d RD-LOADPATH addImportPath -> install" % (rel, line_of(text, m.start())))
        return

    # execute bits on files
    for m in re.finditer(r"\b(setPermissions|chmod|fchmod|fchmodat|lchmod)\s*\(", code):
        before = re.search(r"([\w.>\-\[\]()]+?)\s*(?:\.|->)\s*$", code[max(0, m.start() - 120):m.start()])
        args, _ = balanced_args(code, m.end() - 1)
        a = [text[s:e].strip() for s, e in args]
        fn = m.group(1)
        if fn == "setPermissions":
            if len(a) == 2:
                target, mode = a
            elif len(a) == 1:
                target, mode = (before.group(1) if before else "this"), a[0]
            else:
                continue
        elif fn == "fchmodat":
            if len(a) < 3:
                continue
            target, mode = a[1], a[2]
        else:
            if len(a) < 2:
                continue
            target, mode = a[0], a[1]
        if mode_has_exec(tree, rel, mode) and not looks_like_directory(tree, rel, target, mode):
            add("RD-EXEC-MODE", m.start(), "execute permission on %s" % target)
    for m in re.finditer(r"(?<![\w.>])(?:::)?(open|open64|openat|creat)\s*\(", code):
        args, _ = balanced_args(code, m.end() - 1)
        a = [text[s:e].strip() for s, e in args]
        if len(a) >= 3 and "O_CREAT" in " ".join(a) or (m.group(1) == "creat" and len(a) >= 2):
            mode = a[-1]
            if mode_has_exec(tree, rel, mode):
                add("RD-EXEC-MODE", m.start(1), "file created with an execute bit")

    # writes of code files, writes into the install
    sinks = [
        (r"\bQSaveFile\s+[A-Za-z_]\w*\s*[({]", "all"),
        (r"\bQSaveFile\s*[({]", "all"),
        (r"\bsetFileName\s*\(", "all"),
        (r"\bQFile::(?:copy|rename|link)\s*\(", "last"),
        (r"(?:\.|->)\s*(?:copy|rename|link)\s*\(", "last"),
        (r"(?<![\w.>])(?:::|std::)?rename\s*\(", "last"),
        (r"(?:\.|->)\s*extract\s*\(", "last"),
        (r"(?<![\w.>])(?:::)?(?:open|open64|creat)\s*\(", "first-if-create"),
        (r"(?<![\w.>])fopen\s*\(", "first-if-write"),
    ]
    for pat, which in sinks:
        for m in re.finditer(pat, code):
            args, _ = balanced_args(code, m.end() - 1)
            a = [text[s:e].strip() for s, e in args]
            if not a:
                continue
            if which == "first-if-create":
                if "O_CREAT" not in " ".join(a) and "creat" not in m.group(0):
                    continue
                targets = [a[0]]
            elif which == "first-if-write":
                if len(a) < 2 or not re.search(r"[wa+]", a[1]):
                    continue
                targets = [a[0]]
            elif which == "last":
                targets = [a[-1]]
            else:
                targets = a
            for t in targets:
                if re.search(r"runtimeFile|NORDTILE_RUNTIME_ROOT", t):
                    add("RD-CODE-WRITE", m.start(), "write into the install/runtime directory")
                elif code_file_target(tree, rel, t):
                    add("RD-CODE-WRITE", m.start(), "writes a code/executable file (%s)" % t)
    for m in re.finditer(r"\b(archive_write_disk_new|archive_read_extract2?|QuaZip|JlCompress|KZip|KTar|K7Zip"
                         r"|mz_zip_reader_extract_to_file|unzOpen\w*)\b", code):
        add("RD-EXTRACT", m.start(), "archive extraction library %s" % m.group(1))

    # process launches
    qprocess_subclass = bool(re.search(r":\s*public\s+(?:QProcess|[\w:]*HelperProcess|[\w:]*ResolverProcess)\b",
                                       text + tree.files.get(rel[:-4] + ".h", {}).get("text", "")
                                       if rel.endswith(".cpp") else text))
    launches = []
    for m in re.finditer(r"\bQProcess::(?:startDetached|execute)\s*\(", code):
        launches.append((m, m.end() - 1))
    process_vars = set(re.findall(r"\b(?:QProcess|[\w:]*HelperProcess|[\w:]*ResolverProcess)\s*\*?\s*([A-Za-z_]\w*)\s*[;({=]", code))
    for m in re.finditer(r"([A-Za-z_]\w*)\s*(?:->|\.)\s*(start|startDetached|execute|startHelper)\s*\(", code):
        if m.group(2) == "startHelper" or re.search(r"(?i)proc|helper", m.group(1)) or m.group(1) in process_vars:
            launches.append((m, m.end() - 1))
    if qprocess_subclass:
        for m in re.finditer(r"(?<![\w.>:])start\s*\(", code):
            args, _ = balanced_args(code, m.end() - 1)
            if len(args) >= 2:
                launches.append((m, m.end() - 1))
    for m in re.finditer(r"(?:(?<![\w.>:])::|(?<![\w.>:]))(execv|execve|execvp|execvpe|execl|execlp|execle|fexecve|posix_spawnp?|system|popen)\s*\(", code):
        launches.append((m, m.end() - 1))
    for m, paren in launches:
        args, _ = balanced_args(code, paren)
        if not args:
            continue
        idx = 1 if "posix_spawn" in m.group(0) else 0
        if idx >= len(args):
            continue
        expr = text[args[idx][0]:args[idx][1]].strip()
        if not expr:
            continue
        origins = c_origins(tree, rel, expr, m.start())
        fname, _ = enclosing_function(text, m.start())
        if PARAM in origins and fname in LAUNCH_WRAPPERS:
            origins.discard(PARAM)    # a wrapper: its callers are launch sites of their own
        bad = origins - {INSTALL}
        if bad or not origins and fname not in LAUNCH_WRAPPERS:
            add("RD-PROC", m.start(), "program %s from %s" % (expr, ",".join(sorted(bad or {UNKNOWN}))))
        else:
            CHECKED.append("%s:%d RD-PROC %s -> %s" % (rel, line_of(text, m.start()), expr,
                                                        ",".join(sorted(origins)) or "pass-through"))

    # dynamic loading
    for m in re.finditer(r"(?<![\w.>])(dlopen|QLibrary(?:\s+[A-Za-z_]\w*)?|QPluginLoader(?:\s+[A-Za-z_]\w*)?|QLibrary::resolve)\s*\(", code):
        args, _ = balanced_args(code, m.end() - 1)
        if not args:
            continue
        expr = text[args[0][0]:args[0][1]].strip()
        lits = [c for _, c in literals(expr)]
        if lits and "/" not in lits[0] and blank_strings(expr).strip().startswith(('"', "QStringLiteral(", "QLatin1String(")) \
                and len(re.findall(r"[A-Za-z_]\w*", blank_strings(expr))) <= 1:
            CHECKED.append("%s:%d RD-DLOPEN %s -> system library name" % (rel, line_of(text, m.start()), expr))
            continue        # a bare library name: the system linker path
        origins = c_origins(tree, rel, expr, m.start())
        if origins - {INSTALL}:
            add("RD-DLOPEN", m.start(), "library %s from %s" % (expr, ",".join(sorted(origins - {INSTALL}))))
        else:
            CHECKED.append("%s:%d RD-DLOPEN %s -> install" % (rel, line_of(text, m.start()), expr))

    # code search paths
    for m in re.finditer(r"\b(addImportPath|setImportPathList|addPluginPath|setPluginPathList|addLibraryPath|setLibraryPaths)\s*\(", code):
        args, _ = balanced_args(code, m.end() - 1)
        if not args:
            continue
        expr = text[args[0][0]:args[-1][1]]
        origins = c_origins(tree, rel, expr, m.start())
        if origins - {INSTALL}:
            add("RD-LOADPATH", m.start(), "%s from %s" % (m.group(1), ",".join(sorted(origins - {INSTALL}))))
        else:
            CHECKED.append("%s:%d RD-LOADPATH %s -> install" % (rel, line_of(text, m.start()), m.group(1)))
    for m in re.finditer(r"\b(qputenv|setenv|putenv|insert)\s*\(", code):
        args, _ = balanced_args(code, m.end() - 1)
        if len(args) < 2 and m.group(1) != "putenv":
            continue
        a = [text[s:e] for s, e in args]
        names = [c for _, c in literals(a[0])]
        if not names:
            continue
        name = names[0].split("=")[0]
        if name not in LOAD_ENV:
            continue
        value = a[1] if len(a) > 1 else a[0]
        origins = c_origins(tree, rel, value, m.start())
        if origins - {INSTALL}:
            add("RD-LOADPATH", m.start(), "%s from %s" % (name, ",".join(sorted(origins - {INSTALL}))))
        else:
            CHECKED.append("%s:%d RD-LOADPATH %s -> install" % (rel, line_of(text, m.start()), name))


# ------------------------------------------------------------------ Python

def dotted(node):
    parts = []
    while isinstance(node, ast.Attribute):
        parts.append(node.attr)
        node = node.value
    if isinstance(node, ast.Name):
        parts.append(node.id)
    elif isinstance(node, ast.Call):
        parts.append(dotted(node.func) + "()")
    return ".".join(reversed(parts))


class PyFile:
    def __init__(self, rel, raw):
        self.rel, self.raw, self.lines = rel, raw, raw.split("\n")
        self.tree = ast.parse(raw, filename=rel)
        self.parents = {}
        self.visiting = set()
        for node in ast.walk(self.tree):
            for child in ast.iter_child_nodes(node):
                self.parents[child] = node

    def scope_of(self, node):
        while node in self.parents:
            node = self.parents[node]
            if isinstance(node, (ast.FunctionDef, ast.AsyncFunctionDef, ast.Lambda)):
                return node
        return self.tree

    def assignments(self, scope, name):
        out = []
        for node in ast.walk(scope):
            if isinstance(node, ast.Assign):
                for t in node.targets:
                    for n in ast.walk(t):
                        if isinstance(n, ast.Name) and n.id == name:
                            out.append(node.value)
            elif isinstance(node, (ast.AnnAssign, ast.AugAssign)) and isinstance(node.target, ast.Name) \
                    and node.target.id == name and node.value is not None:
                out.append(node.value)
            elif isinstance(node, (ast.With, ast.AsyncWith)):
                for item in node.items:
                    if isinstance(item.optional_vars, ast.Name) and item.optional_vars.id == name:
                        out.append(item.context_expr)
        return out

    def origins(self, node, scope=None, env=None, depth=0):
        scope = self.scope_of(node) if scope is None else scope
        env = {} if env is None else env
        if depth > 8:
            return {UNKNOWN}
        if isinstance(node, ast.Constant):
            if isinstance(node.value, str):
                return {classify_literal(node.value) or "literal"}
            return {"literal"}
        if isinstance(node, ast.Name):
            if node.id == "__file__":
                return {INSTALL}
            if node.id in env:
                return env[node.id]
            key = (id(scope), node.id)
            if key in self.visiting:
                return set()          # `x = x or y`: the other operands decide
            self.visiting.add(key)
            try:
                return self.name_origins(node, scope, env, depth)
            finally:
                self.visiting.discard(key)
        return self.expr_origins(node, scope, env, depth)

    def name_origins(self, node, scope, env, depth):
        if isinstance(scope, (ast.FunctionDef, ast.AsyncFunctionDef)):
            argnames = [a.arg for a in scope.args.args + scope.args.kwonlyargs]
            if node.id in argnames:
                result = {PARAM}
                for value in self.assignments(scope, node.id):
                    result |= self.origins(value, scope, env, depth + 1) - {"literal"}
                return result
        values = self.assignments(scope, node.id) or self.assignments(self.tree, node.id)
        if not values:
            return {UNKNOWN}
        result = set()
        for v in values:
            result |= self.origins(v, scope, env, depth + 1)
        return result

    def expr_origins(self, node, scope, env, depth):
        if isinstance(node, ast.Attribute):
            name = dotted(node)
            if name.startswith("os.environ"):
                return {ENV}
            return {UNKNOWN}
        if isinstance(node, ast.Subscript):
            if dotted(node.value).startswith("os.environ"):
                return {ENV}
            return self.origins(node.value, scope, env, depth + 1)
        if isinstance(node, ast.Call):
            name = dotted(node.func)
            if name in SAFE_PY_CALLS:
                return {"literal"}
            if name in ("os.getenv", "os.environ.get", "os.environ.pop", "os.environ.setdefault", "os.getenvb"):
                return {ENV}
            if name.startswith("tempfile.") or name in ("os.path.expanduser", "os.path.expandvars", "os.getcwd",
                                                        "pathlib.Path.home", "Path.home", "os.path.expanduser"):
                return {WRITABLE}
            if name.startswith("sysconfig.") or name.startswith("site.getsitepackages"):
                return {INSTALL}
            if name in ("os.path.join", "os.path.abspath", "os.path.realpath", "os.path.dirname", "os.path.normpath",
                        "str", "sorted", "list", "set", "tuple", "os.fspath", "os.fsdecode", "os.path.basename") \
                    or name.endswith((".format", ".join")):
                result = set()
                for a in node.args:
                    result |= self.origins(a, scope, env, depth + 1)
                if isinstance(node.func, ast.Attribute) and name.endswith(".format"):
                    result |= self.origins(node.func.value, scope, env, depth + 1)
                return result or {UNKNOWN}
            return {UNKNOWN}
        if isinstance(node, (ast.BinOp,)):
            left = self.origins(node.left, scope, env, depth + 1)
            if isinstance(node.op, ast.Mod):   # "%s" % x: the format string decides the shape
                return left | (self.origins(node.right, scope, env, depth + 1) - {"literal"})
            return left | self.origins(node.right, scope, env, depth + 1)
        if isinstance(node, ast.BoolOp):
            result = set()
            for v in node.values:
                result |= self.origins(v, scope, env, depth + 1)
            return result
        if isinstance(node, ast.IfExp):
            return self.origins(node.body, scope, env, depth + 1) | self.origins(node.orelse, scope, env, depth + 1)
        if isinstance(node, (ast.List, ast.Tuple, ast.Set)):
            result = set()
            for e in node.elts:
                result |= self.origins(e, scope, env, depth + 1)
            return result or {"literal"}
        if isinstance(node, (ast.ListComp, ast.SetComp, ast.GeneratorExp)):
            inner = dict(env)
            for gen in node.generators:
                it = self.origins(gen.iter, scope, inner, depth + 1)
                for n in ast.walk(gen.target):
                    if isinstance(n, ast.Name):
                        inner[n.id] = it
            return self.origins(node.elt, scope, inner, depth + 1)
        if isinstance(node, ast.JoinedStr):
            result = set()
            for v in node.values:
                if isinstance(v, ast.FormattedValue):
                    result |= self.origins(v.value, scope, env, depth + 1)
            return result or {"literal"}
        return {UNKNOWN}

    def code_file(self, node, scope=None, depth=0):
        scope = self.scope_of(node) if scope is None else scope
        for n in ast.walk(node):
            if isinstance(n, ast.Constant) and isinstance(n.value, str) and CODE_FILE_RE.search(n.value.strip()):
                return True
            if isinstance(n, ast.Name) and depth < 4:
                for v in self.assignments(scope, n.id) or self.assignments(self.tree, n.id):
                    if self.code_file(v, scope, depth + 1):
                        return True
        return False


def int_mode_exec(node):
    for n in ast.walk(node):
        if isinstance(n, ast.Constant) and isinstance(n.value, int) and not isinstance(n.value, bool) and n.value & 0o111:
            return True
        if isinstance(n, ast.Attribute) and re.match(r"S_IX(USR|GRP|OTH)|S_IRWX[UGO]", n.attr):
            return True
        if isinstance(n, ast.Name) and re.match(r"S_IX(USR|GRP|OTH)|S_IRWX[UGO]", n.id):
            return True
    return False


def check_py(tree, rel, report):
    entry = tree.files[rel]
    try:
        f = PyFile(rel, entry["raw"])
    except SyntaxError as e:
        report(rel, e.lineno or 0, "RD-PARSE", "unparsable Python: %s" % e.msg, "")
        return
    imports = set()
    for node in ast.walk(f.tree):
        if isinstance(node, ast.Import):
            imports |= {a.name.split(".")[0] for a in node.names}
        elif isinstance(node, ast.ImportFrom) and node.module:
            imports.add(node.module.split(".")[0])

    def add(rule, node, detail):
        report(rel, node.lineno, rule, detail, source_line(f.lines, node.lineno))

    def bad(origins):
        return origins - {INSTALL, "literal"}

    for node in ast.walk(f.tree):
        if isinstance(node, ast.Constant) and isinstance(node.value, str):
            if URL_RE.search(node.value):
                add("RD-URL", node, "release/asset download URL")
            v = node.value
            if v in ("--update", "-U", "--update-to") or v.startswith(("--update-to=", "--remote-components", "--plugin-dirs")):
                add("RD-FLAG", node, "extractor self-update/remote component/plugin flag %s" % v)
        # sys.path assignments
        if isinstance(node, ast.Assign):
            for t in node.targets:
                if dotted(t if not isinstance(t, ast.Subscript) else t.value) == "sys.path":
                    o = f.origins(node.value)
                    if bad(o):
                        add("RD-LOADPATH", node, "sys.path from %s" % ",".join(sorted(bad(o))))
                    else:
                        CHECKED.append("%s:%d RD-LOADPATH sys.path -> %s" % (rel, node.lineno, ",".join(sorted(o))))
                if isinstance(t, ast.Subscript) and dotted(t.value) == "os.environ":
                    key = t.slice.value if isinstance(t.slice, ast.Constant) else getattr(getattr(t.slice, "value", None), "value", None)
                    if key in LOAD_ENV:
                        o = f.origins(node.value)
                        if bad(o):
                            add("RD-LOADPATH", node, "%s from %s" % (key, ",".join(sorted(bad(o)))))
        if not isinstance(node, ast.Call):
            continue
        name = dotted(node.func)
        args = node.args
        if name in ("sys.path.insert", "sys.path.append", "sys.path.extend", "site.addsitedir"):
            value = args[-1] if args else None
            if value is not None:
                o = f.origins(value)
                if bad(o):
                    add("RD-LOADPATH", node, "%s from %s" % (name, ",".join(sorted(bad(o)))))
                else:
                    CHECKED.append("%s:%d RD-LOADPATH %s -> %s" % (rel, node.lineno, name, ",".join(sorted(o))))
        elif name in ("os.putenv", "os.environ.setdefault", "os.environ.update") and args:
            if isinstance(args[0], ast.Constant) and args[0].value in LOAD_ENV and len(args) > 1:
                o = f.origins(args[1])
                if bad(o):
                    add("RD-LOADPATH", node, "%s from %s" % (args[0].value, ",".join(sorted(bad(o)))))
        elif name in ("os.chmod", "os.fchmod", "os.lchmod") or (name.endswith(".chmod") and not name.startswith("os.")):
            mode = args[1] if len(args) > 1 else (args[0] if args and not name.startswith("os.") else None)
            target = args[0] if args else None
            tname = ast.dump(target) if target is not None else ""
            if mode is not None and int_mode_exec(mode) and not re.search(r"(?i)dir|folder", tname):
                add("RD-EXEC-MODE", node, "execute permission set by %s" % name)
        elif name == "os.open" and len(args) >= 2:
            flags = ast.dump(args[1])
            if "O_CREAT" in flags or "O_WRONLY" in flags or "O_RDWR" in flags:
                if len(args) >= 3 and int_mode_exec(args[2]):
                    add("RD-EXEC-MODE", node, "file created with an execute bit")
                if f.code_file(args[0]):
                    add("RD-CODE-WRITE", node, "writes a code/executable file")
        elif name == "open" and args:
            mode = args[1] if len(args) > 1 else next((k.value for k in node.keywords if k.arg == "mode"), None)
            writes = isinstance(mode, ast.Constant) and isinstance(mode.value, str) and re.search(r"[wax+]", mode.value)
            if writes and not (isinstance(args[0], ast.Call) and dotted(args[0].func) == "os.open") and f.code_file(args[0]):
                add("RD-CODE-WRITE", node, "writes a code/executable file")
        elif name in ("shutil.copy", "shutil.copy2", "shutil.copyfile", "shutil.move", "os.rename", "os.replace",
                      "os.link", "os.symlink", "shutil.copytree") and len(args) >= 2:
            if f.code_file(args[1]):
                add("RD-CODE-WRITE", node, "copies/moves into a code/executable file")
        elif name.endswith((".write_bytes", ".write_text")) and isinstance(node.func, ast.Attribute):
            if f.code_file(node.func.value):
                add("RD-CODE-WRITE", node, "writes a code/executable file")
        elif name in ("urllib.request.urlretrieve", "request.urlretrieve", "urlretrieve"):
            add("RD-CODE-WRITE", node, "downloads a URL straight into a file")
        if name == "shutil.unpack_archive" or (name.endswith((".extractall", ".extract")) and imports & {"zipfile", "tarfile"}):
            add("RD-EXTRACT", node, "archive extraction (%s)" % name)
        if name.split(".")[0] == "subprocess" or name in ("os.system", "os.popen", "os.startfile") \
                or re.match(r"os\.(exec|spawn|posix_spawn)", name):
            prog = args[0] if args else None
            if isinstance(prog, (ast.List, ast.Tuple)) and prog.elts:
                prog = prog.elts[0]
            o = f.origins(prog) if prog is not None else {UNKNOWN}
            if bad(o):
                add("RD-PROC", node, "%s program from %s" % (name, ",".join(sorted(bad(o)))))
        if name in ("ctypes.CDLL", "ctypes.PyDLL", "ctypes.cdll.LoadLibrary", "cdll.LoadLibrary", "CDLL") and args:
            a = args[0]
            if not (isinstance(a, ast.Constant) and isinstance(a.value, str) and "/" not in a.value):
                o = f.origins(a)
                if bad(o):
                    add("RD-DLOPEN", node, "library from %s" % ",".join(sorted(bad(o))))
        if name in ("zipimport.zipimporter", "importlib.util.spec_from_file_location", "runpy.run_path",
                    "importlib.machinery.SourceFileLoader", "importlib.machinery.ExtensionFileLoader"):
            target = args[-1] if args else None
            o = f.origins(target) if target is not None else {UNKNOWN}
            if bad(o):
                add("RD-PYLOAD", node, "%s from %s" % (name, ",".join(sorted(bad(o)))))
        if name in ("exec", "compile", "eval") and args and not isinstance(args[0], ast.Constant):
            add("RD-PYLOAD", node, "%s() of non-literal code" % name)


def check_sh(tree, rel, report):
    for i, line in enumerate(tree.files[rel]["lines"], 1):
        code = line.split("#", 1)[0]
        if re.search(r"\bchmod\s+(?:[ugoa]*\+[rw]*x|[0-7]*[1357][0-7]*\b)", code):
            report(rel, i, "RD-EXEC-MODE", "chmod adds an execute bit", line.strip())
        if re.search(r"\b(curl|wget)\b|\bpip3?\s+install\b", code):
            report(rel, i, "RD-SCRIPTLET", "downloads at run time", line.strip())
        if URL_RE.search(code):
            report(rel, i, "RD-URL", "release/asset download URL", line.strip())


def check_spec(root, report):
    rpm = os.path.join(root, "rpm")
    if not os.path.isdir(rpm):
        return
    for name in sorted(os.listdir(rpm)):
        if not name.endswith(".spec"):
            continue
        rel = "rpm/" + name
        section = None
        for i, line in enumerate(open(os.path.join(rpm, name), encoding="utf-8", errors="replace").read().split("\n"), 1):
            m = re.match(r"^%(\w+)", line)
            if m:
                section = m.group(1)
            if section in ("pre", "post", "preun", "postun", "pretrans", "posttrans", "triggerin", "triggerun"):
                if re.search(r"\b(curl|wget|git)\b|\bpip3?\s+install\b", line):
                    report(rel, i, "RD-SCRIPTLET", "scriptlet downloads or installs", line.strip())
            if not line.lstrip().startswith("#") and URL_RE.search(line):
                report(rel, i, "RD-URL", "release/asset download URL", line.strip())


def check_guard(tree, report):
    entry = tree.files.get(RUNNER)
    if entry is None:
        report(RUNNER, 0, "RD-GUARD", "the extractor runner is missing (scope broken)", "")
        return
    m = re.search(r"commonArguments\s*\([^)]*\)\s*const\s*\{", entry["text"])
    body = entry["text"][m.end():entry["text"].find("\n}", m.end())] if m else ""
    present = {c for _, c in literals(body)}
    for flag in RUNNER_GUARDS:
        if flag not in present:
            report(RUNNER, line_of(entry["text"], m.start()) if m else 0, "RD-GUARD",
                   "commonArguments() no longer passes %s" % flag, "")


# ------------------------------------------------------------------ main

def store_excluded_sources(root):
    """Files that the qmake project compiles only outside the STORE profile: listed exclusively inside
    `!sailtube_store { ... }` blocks of sailtube.pro and the .pri files it includes. A file also listed
    outside such a block counts as compiled in both builds."""
    inside, outside = set(), set()
    seen = set()

    def walk(path):
        path = os.path.normpath(path)
        if path in seen or not os.path.isfile(path):
            return
        seen.add(path)
        base = os.path.dirname(path)
        depth_store = []   # stack of booleans: is this block a !sailtube_store block
        for raw in open(path, encoding="utf-8", errors="replace"):
            line = raw.split("#", 1)[0].strip()
            if not line:
                continue
            m = re.match(r"include\s*\(\s*([^)]+?)\s*\)", line)
            if m:
                inc = m.group(1).replace("$$PWD", base)
                walk(inc if os.path.isabs(inc) else os.path.join(root if path.endswith(".pro") else base, inc))
                continue
            opens = line.count("{")
            closes = line.count("}")
            is_store = bool(re.match(r"!\s*sailtube_store\b", line)) and opens > 0
            for token in re.findall(r"[\w./$-]+\.(?:cpp|h|hpp|c|cc|qml|js|py)\b", line):
                rel = token.replace("$$PWD/", "").replace("$$PWD", "")
                (inside if any(depth_store) or is_store else outside).add(os.path.normpath(rel))
            for _ in range(opens):
                depth_store.append(is_store)
            for _ in range(closes):
                if depth_store:
                    depth_store.pop()
    walk(os.path.join(root, "sailtube.pro"))
    return {f for f in inside if f not in outside}


def main(argv):
    here = os.path.dirname(os.path.abspath(__file__))
    parser = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    parser.add_argument("--root", default=os.path.normpath(os.path.join(here, "..")))
    parser.add_argument("--exceptions", default=None)
    parser.add_argument("--variant", choices=("full", "store"), default="full")
    parser.add_argument("--verbose", action="store_true")
    args = parser.parse_args(argv)
    root = os.path.abspath(args.root)
    exceptions_path = args.exceptions or os.path.join(root, "tests", "runtime_downloads", "exceptions.json")
    try:
        doc = json.load(open(exceptions_path, encoding="utf-8"))
        exceptions = doc["exceptions"]
        for e in exceptions:
            for key in ("file", "rule", "match", "reason"):
                if not isinstance(e.get(key), str) or not e[key].strip():
                    raise ValueError("exception without %s: %r" % (key, e))
            if e.get("variant", "all") not in ("all", "full"):
                raise ValueError("exception variant must be all or full: %r" % e)
    except (OSError, ValueError, KeyError) as err:
        print("runtime download check: cannot read exceptions (%s)" % err)
        return 2
    if not all(os.path.isdir(os.path.join(root, d)) for d in SCAN_ROOTS):
        print("runtime download check: %s is not a SailTube tree" % root)
        return 2

    tree = Tree(root)
    findings = []
    full_only = store_excluded_sources(root)

    def report(rel, line, rule, detail, src):
        findings.append({"file": rel, "line": line, "rule": rule, "detail": detail, "source": src})

    for rel in sorted(tree.files):
        if args.variant == "store" and rel in full_only:
            continue   # compiled only by `!sailtube_store { }` blocks: not in the STORE binary
        if rel.endswith(".py"):
            check_py(tree, rel, report)
        elif rel.endswith(".sh"):
            check_sh(tree, rel, report)
        else:
            check_c(tree, rel, report)
    check_spec(root, report)
    check_guard(tree, report)

    seen_keys = set()
    unique = []
    for f in findings:
        key = (f["file"], f["line"], f["rule"])
        if key not in seen_keys:
            seen_keys.add(key)
            unique.append(f)

    active = [e for e in exceptions if args.variant == "full" or e.get("variant", "all") == "all"]
    used = set()
    failing, excepted = [], []
    for f in unique:
        hit = None
        for i, e in enumerate(active):
            if e["file"] == f["file"] and e["rule"] == f["rule"] and e["match"] in f["source"]:
                hit = i
                break
        if hit is None:
            failing.append(f)
        else:
            used.add(hit)
            excepted.append((f, active[hit]))
    stale = [e for i, e in enumerate(active) if i not in used]

    if args.verbose:
        for c in CHECKED:
            print("  passed %s" % c)
    for f, e in excepted:
        if args.verbose:
            print("  excepted %s:%d %s (%s): %s" % (f["file"], f["line"], f["rule"], f["detail"], e["reason"]))
    for f in failing:
        print("%s:%d: %s: %s\n    %s" % (f["file"], f["line"], f["rule"], f["detail"], f["source"]))
    for e in stale:
        print("stale exception (matches nothing): %s %s %r" % (e["file"], e["rule"], e["match"]))
    print("runtime download check (%s): %d files, %d findings, %d excepted, %d stale exceptions"
          % (args.variant, len(tree.files), len(failing), len(excepted), len(stale)))
    return 1 if failing or stale else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
