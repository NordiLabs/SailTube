#!/usr/bin/env python3
# SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
"""Coverage ratchet for the engine code (NFR-TST-06).

    tools/coverage_ratchet.py check  [--root DIR]
    tools/coverage_ratchet.py update [--root DIR]
    tools/coverage_ratchet.py growth --base REF [--repo DIR]
    tools/coverage_ratchet.py list   [--root DIR]

A new engine function without a test fails CI, unless it is listed in the baseline
tests/coverage/baseline.txt, and the baseline may only shrink.

Definitions (deliberately simple; stable and deterministic rather than precise):

Engine function
    A function or method declared in a C++ header (*.h, *.hpp) under Core/** or
    Shared/Playback/**. Shared/SailTune/** (pinned upstream code, tested by its own suite) and
    Shared/Runtime/** are not scanned. Headers are tokenised after removing comments, string and
    character literals and preprocessor lines. A declaration counts when the identifier right
    before its first top-level "(" is a name, at namespace scope (free functions, inline ones
    included) or in the public section of a class/struct/union (methods, static, virtual,
    Q_INVOKABLE and inline ones, nested classes included). Q_PROPERTY accessors are ordinary
    public methods and are included. Not counted: constructors, destructors, operators,
    conversion functions, "= delete" / "= default" declarations, friend declarations, Qt
    signals (the "signals:" / "Q_SIGNALS:" sections), private and protected members (they are
    covered through the public API), variables, typedefs/using, function pointers, and Qt
    macros (Q_OBJECT, Q_PROPERTY(...), Q_ENUM(...), ...). Overloads collapse into one entry.
    Entry format: "<header path>::<Class>::<name>" for members (nested classes joined with
    "::") and "<header path>::<name>" for free functions; paths are relative to the repository
    root with "/" separators. Namespaces are not part of the entry.

Tested
    The bare function name occurs as an identifier in some file under tests/**. Not searched:
    tests/coverage/** (this suite and the baseline itself), *.json / *.jsonl data under
    tests/fixtures/, and binary files. In C-like sources (.cpp .cc .h .hpp .c .qml .js) comments
    are removed first; string contents are kept, so QML code and invokeMethod("name") in tests
    count, which is how Q_INVOKABLE / QML-facing methods are reached. Other text files (.sh,
    .py, ...) are searched as they are.

Baseline
    tests/coverage/baseline.txt: the engine functions that are not tested, one entry per line,
    sorted by code point, no duplicates. Lines starting with "#" and blank lines are ignored.

Commands
    check   Fails when an untested engine function is not in the baseline ("add a test"), when
            a baseline entry is tested now or no longer exists ("remove it from the baseline:
            it may only shrink"), or when the baseline is not sorted/unique. Prints counts.
    update  Rewrites the baseline without the entries that are tested now or gone. Never adds.
    growth  Fails when the working-tree baseline has an entry that the baseline at git REF does
            not have. Needs git history (host / release job). A REF without a baseline (the
            change that introduces the ratchet) passes with a note.
    list    Prints every engine function with "tested" or "untested" (diagnostics).

Exit status: 0 pass, 1 ratchet failure, 2 usage or environment error.
Python 3.9 standard library only; run with "python3 -I".
"""
import argparse
import os
import re
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
BASELINE_REL = "tests/coverage/baseline.txt"
BASELINE_HEADER = (
    "# NFR-TST-06 coverage ratchet: untested engine functions (tools/coverage_ratchet.py).\n"
    "# This list may only shrink: add a test and run `tools/coverage_ratchet.py update`.\n"
)

ENGINE_DIRS = ("Core", "Shared/Playback")
EXCLUDED_DIRS = ("Shared/SailTune", "Shared/Runtime")
HEADER_EXT = (".h", ".hpp")
C_LIKE_EXT = (".cpp", ".cc", ".cxx", ".c", ".h", ".hpp", ".qml", ".js")
TEST_EXCLUDED_DIRS = ("tests/coverage",)
FIXTURE_DIR = "tests/fixtures"
FIXTURE_DATA_EXT = (".json", ".jsonl")

IDENT = re.compile(r"[A-Za-z_][A-Za-z0-9_]*")
TOKEN = re.compile(r"[A-Za-z_][A-Za-z0-9_]*|\d[\w.']*|::|->|.", re.S)
QT_MACRO = re.compile(r"^(?:Q|QT)_[A-Z0-9_]+$")
ACCESS = {"public": "public", "protected": "protected", "private": "private",
          "signals": "signals", "Q_SIGNALS": "signals"}
SLOT_WORDS = {"slots", "Q_SLOTS"}
CLASS_KEYS = {"class", "struct", "union"}
# Names that cannot be the name of a declared function even though "(" follows them.
NOT_NAMES = {
    "alignas", "alignof", "decltype", "sizeof", "static_assert", "noexcept", "throw",
    "return", "if", "while", "for", "switch", "void", "bool", "char", "short", "int", "long",
    "float", "double", "signed", "unsigned", "auto", "const", "volatile", "operator",
    "__attribute__", "__declspec",
}


def rel(path, root):
    return os.path.relpath(path, root).replace(os.sep, "/")


def strip_comments(text, keep_strings):
    """Removes // and /* */ comments; string and char literals are kept or blanked."""
    out = []
    i, n = 0, len(text)
    while i < n:
        c = text[i]
        if c == "/" and i + 1 < n and text[i + 1] == "/":
            j = text.find("\n", i)
            i = n if j < 0 else j
        elif c == "/" and i + 1 < n and text[i + 1] == "*":
            j = text.find("*/", i + 2)
            out.append(" ")
            i = n if j < 0 else j + 2
        elif c in "\"'":
            j = i + 1
            while j < n and text[j] != c and text[j] != "\n":
                j += 2 if text[j] == "\\" else 1
            out.append(text[i:j + 1] if keep_strings else c + c)
            i = j + 1
        else:
            out.append(c)
            i += 1
    return "".join(out)


def strip_preprocessor(text):
    lines, out, cont = text.split("\n"), [], False
    for line in lines:
        directive = cont or line.lstrip().startswith("#")
        cont = directive and line.endswith("\\")
        out.append("" if directive else line)
    return "\n".join(out)


def tokens_of(text):
    return [t for t in TOKEN.findall(text) if not t.isspace()]


def drop_qt_macros(toks):
    """Removes Q_*/QT_* macros (with their argument lists), keeping Q_SIGNALS/Q_SLOTS."""
    out, i = [], 0
    while i < len(toks):
        t = toks[i]
        if QT_MACRO.match(t) and t not in ACCESS and t not in SLOT_WORDS:
            i += 1
            if i < len(toks) and toks[i] == "(":
                depth = 0
                while i < len(toks):
                    depth += {"(": 1, ")": -1}.get(toks[i], 0)
                    i += 1
                    if depth == 0:
                        break
            continue
        out.append(t)
        i += 1
    return out


def skip_braces(toks, i):
    """toks[i] == "{": returns the index after the matching "}"."""
    depth = 0
    while i < len(toks):
        if toks[i] == "{":
            depth += 1
        elif toks[i] == "}":
            depth -= 1
            if depth == 0:
                return i + 1
        i += 1
    return i


def strip_template_prefix(stmt):
    while stmt and stmt[0] == "template" and len(stmt) > 1 and stmt[1] == "<":
        depth, j = 0, 1
        while j < len(stmt):
            if stmt[j] == "<":
                depth += 1
            elif stmt[j] == ">":
                depth -= 1
                if depth == 0:
                    break
            j += 1
        stmt = stmt[j + 1:]
    return stmt


def class_name(stmt):
    """stmt starts with class/struct/union and ends before "{"."""
    names = []
    for k, t in enumerate(stmt[1:], 1):
        if t == ":" or t == "final":
            break
        if IDENT.fullmatch(t) and t not in ("alignas",):
            names.append(t)
    return names[-1] if names else "anonymous"


def function_name(stmt, cls):
    """Returns the declared function name of a statement, or None."""
    stmt = strip_template_prefix(stmt)
    if not stmt or stmt[0] in ("using", "typedef", "friend", "enum", "namespace", "extern"):
        return None
    angle = 0
    for k, t in enumerate(stmt):
        if t == "<":
            angle += 1
        elif t == ">":
            angle -= 1
        elif t == "=" and angle <= 0:
            return None  # a variable with an initialiser
        elif t == "(" and angle <= 0:
            if k == 0:
                return None
            name = stmt[k - 1]
            if not IDENT.fullmatch(name) or name in NOT_NAMES:
                return None
            if k >= 2 and stmt[k - 2] in ("~", "operator"):
                return None
            if cls is not None and name == cls:
                return None  # constructor
            tail = stmt[k:]
            for a in range(len(tail) - 1):
                if tail[a] == "=" and tail[a + 1] in ("delete", "default"):
                    return None
            return name
    return None


def scan_header(text):
    """Returns the qualified names ("Class::name" or "name") declared by one header."""
    toks = drop_qt_macros(tokens_of(strip_preprocessor(strip_comments(text, keep_strings=False))))
    found = set()
    # Scope stack entries: ("ns", None, None) or ("class", name, access).
    stack = []
    stmt = []
    i = 0

    def scope_class():
        return stack[-1] if stack and stack[-1][0] == "class" else None

    def class_path():
        return [s[1] for s in stack if s[0] == "class"]

    def visible():
        return all(s[2] == "public" for s in stack if s[0] == "class")

    def record(name):
        if name and visible():
            found.add("::".join(class_path() + [name]))

    while i < len(toks):
        t = toks[i]
        top = scope_class()
        if not stmt and top is not None and t in ACCESS:
            j = i + 1
            while j < len(toks) and toks[j] in SLOT_WORDS:
                j += 1
            if j < len(toks) and toks[j] == ":":
                stack[-1] = ("class", top[1], ACCESS[t])
                i = j + 1
                continue
        if t == ";":
            if stmt:
                record(function_name(stmt, top[1] if top else None))
            stmt = []
            i += 1
        elif t == "{":
            head = strip_template_prefix(stmt)
            if head and (head[0] in ("namespace", "inline") and "namespace" in head
                         or head[0] == "extern" and '"' in head):
                stack.append(("ns", None, None))
                i += 1
            elif head and head[0] in CLASS_KEYS:
                default = "private" if head[0] == "class" else "public"
                stack.append(("class", class_name(head), default))
                i += 1
            else:
                if head and "(" in head:
                    record(function_name(stmt, top[1] if top else None))
                i = skip_braces(toks, i)
            stmt = []
        elif t == "}":
            if stack:
                stack.pop()
            stmt = []
            i += 1
        else:
            stmt.append(t)
            i += 1
    return found


def walk_files(base):
    for dirpath, dirnames, filenames in os.walk(base):
        dirnames.sort()
        for f in sorted(filenames):
            yield os.path.join(dirpath, f)


def engine_functions(root):
    result = set()
    for d in ENGINE_DIRS:
        base = os.path.join(root, d)
        if not os.path.isdir(base):
            continue
        for path in walk_files(base):
            r = rel(path, root)
            if any(r == x or r.startswith(x + "/") for x in EXCLUDED_DIRS):
                continue
            if not path.endswith(HEADER_EXT):
                continue
            with open(path, encoding="utf-8", errors="replace") as fh:
                names = scan_header(fh.read())
            result.update(r + "::" + n for n in names)
    return result


def test_identifiers(root):
    idents = set()
    base = os.path.join(root, "tests")
    if not os.path.isdir(base):
        return idents
    for path in walk_files(base):
        r = rel(path, root)
        if any(r.startswith(x + "/") for x in TEST_EXCLUDED_DIRS):
            continue
        if r.startswith(FIXTURE_DIR + "/") and r.endswith(FIXTURE_DATA_EXT):
            continue
        with open(path, "rb") as fh:
            data = fh.read()
        if b"\0" in data:
            continue
        text = data.decode("utf-8", errors="replace")
        if path.endswith(C_LIKE_EXT):
            text = strip_comments(text, keep_strings=True)
        idents.update(IDENT.findall(text))
    return idents


def bare_name(entry):
    return entry.rsplit("::", 1)[-1]


def parse_baseline(text):
    return [l.strip() for l in text.splitlines() if l.strip() and not l.lstrip().startswith("#")]


def read_baseline(root):
    path = os.path.join(root, BASELINE_REL)
    if not os.path.exists(path):
        return None
    with open(path, encoding="utf-8") as fh:
        return parse_baseline(fh.read())


def write_baseline(root, entries):
    path = os.path.join(root, BASELINE_REL)
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, "w", encoding="utf-8", newline="\n") as fh:
        fh.write(BASELINE_HEADER)
        for e in sorted(set(entries)):
            fh.write(e + "\n")


def analyse(root):
    functions = engine_functions(root)
    idents = test_identifiers(root)
    tested = {f for f in functions if bare_name(f) in idents}
    return functions, tested


def cmd_list(args):
    functions, tested = analyse(args.root)
    for f in sorted(functions):
        print(("tested   " if f in tested else "untested ") + f)
    return 0


def cmd_check(args):
    root = args.root
    baseline = read_baseline(root)
    if baseline is None:
        print("coverage: no baseline at %s (create it with `update --init`)" % BASELINE_REL)
        return 2
    functions, tested = analyse(root)
    untested = functions - tested
    base = set(baseline)
    errors = []
    if baseline != sorted(base):
        errors.append("%s is not sorted or has duplicate entries; run `tools/coverage_ratchet.py "
                      "update`" % BASELINE_REL)
    for f in sorted(untested - base):
        errors.append("untested engine function %s: add a test that calls %s()" % (f, bare_name(f)))
    for e in sorted(base & tested):
        errors.append("baseline entry %s is tested now: remove it from the baseline: it may only "
                      "shrink (tools/coverage_ratchet.py update)" % e)
    for e in sorted(base - functions):
        errors.append("baseline entry %s no longer exists: remove it from the baseline: it may "
                      "only shrink (tools/coverage_ratchet.py update)" % e)
    print("coverage: %d engine functions, %d tested, %d untested, %d in baseline"
          % (len(functions), len(tested), len(untested), len(base)))
    for e in errors:
        print("coverage: FAIL " + e)
    return 1 if errors else 0


def cmd_update(args):
    root = args.root
    baseline = read_baseline(root)
    functions, tested = analyse(root)
    if baseline is None:
        if not args.init:
            print("coverage: no baseline at %s (pass --init to create it)" % BASELINE_REL)
            return 2
        keep = sorted(functions - tested)
    else:
        keep = sorted(e for e in set(baseline) if e in functions and e not in tested)
        dropped = len(set(baseline)) - len(keep)
        print("coverage: removed %d baseline entr%s" % (dropped, "y" if dropped == 1 else "ies"))
    write_baseline(root, keep)
    print("coverage: baseline has %d entries" % len(keep))
    return 0


def cmd_growth(args):
    repo = args.repo
    try:
        top = subprocess.run(["git", "-C", repo, "rev-parse", "--show-toplevel"],
                             stdout=subprocess.PIPE, stderr=subprocess.PIPE, check=False)
    except OSError:
        print("coverage growth: git is not available")
        return 2
    if top.returncode != 0:
        print("coverage growth: %s is not a git work tree" % repo)
        return 2
    verify = subprocess.run(["git", "-C", repo, "rev-parse", "--verify", "--quiet",
                             args.base + "^{commit}"],
                            stdout=subprocess.PIPE, stderr=subprocess.PIPE, check=False)
    if verify.returncode != 0:
        print("coverage growth: unknown ref %s" % args.base)
        return 2
    toplevel = top.stdout.decode().strip()
    shown = subprocess.run(["git", "-C", toplevel, "show", "%s:%s" % (args.base, BASELINE_REL)],
                           stdout=subprocess.PIPE, stderr=subprocess.PIPE, check=False)
    current = read_baseline(toplevel)
    if current is None:
        print("coverage growth: FAIL no baseline at %s in the working tree" % BASELINE_REL)
        return 1
    if shown.returncode != 0:
        print("coverage growth: %s has no %s; the ratchet starts with this change"
              % (args.base, BASELINE_REL))
        return 0
    before = set(parse_baseline(shown.stdout.decode("utf-8", errors="replace")))
    added = sorted(set(current) - before)
    print("coverage growth: %d entries at %s, %d now" % (len(before), args.base, len(set(current))))
    for e in added:
        print("coverage growth: FAIL new baseline entry %s: the baseline may only shrink; add a "
              "test instead" % e)
    return 1 if added else 0


def main(argv):
    p = argparse.ArgumentParser(description="NFR-TST-06 coverage ratchet")
    sub = p.add_subparsers(dest="cmd")
    for name in ("check", "update", "list"):
        s = sub.add_parser(name)
        s.add_argument("--root", default=ROOT, help="tree to scan (default: this repository)")
        if name == "update":
            s.add_argument("--init", action="store_true",
                           help="create a missing baseline from the current tree")
    g = sub.add_parser("growth")
    g.add_argument("--base", required=True, help="git ref holding the previous baseline")
    g.add_argument("--repo", default=ROOT, help="git work tree (default: this repository)")
    args = p.parse_args(argv)
    if args.cmd is None:
        p.print_usage()
        return 2
    if hasattr(args, "root"):
        args.root = os.path.abspath(args.root)
    return {"check": cmd_check, "update": cmd_update, "growth": cmd_growth,
            "list": cmd_list}[args.cmd](args)


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
