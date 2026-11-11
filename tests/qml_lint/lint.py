# SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
"""Static checks over the Sailfish QML (PLT-05, PLT-06, SET-02, I18N, theme colours).

Runs on Python 3 with the standard library only (host and build container). Each check
prints "FAIL <file>:<line>: <message>"; the exit code is 1 when anything fails.

Exemptions are explicit and local: a line comment `// theme-exempt: <reason>` allows a colour
literal on that line, `// i18n-exempt: <reason>` allows an untranslated literal (brand and codec
names), and `// pulley-dynamic: <max>` declares the most items a Repeater inside a pulley adds.
"""
import os
import re
import sys

ROOT = os.path.abspath(sys.argv[1] if len(sys.argv) > 1 else os.path.join(os.path.dirname(__file__), "..", ".."))
QML_ROOT = os.path.join(ROOT, "Clients", "Sailfish", "qml")
PULLEY_LIMIT = 5
failures = []


def fail(path, line, message):
    failures.append("%s:%d: %s" % (os.path.relpath(path, ROOT), line, message))


def files(ext):
    out = []
    for base, _, names in os.walk(QML_ROOT):
        out += [os.path.join(base, n) for n in sorted(names) if n.endswith(ext)]
    return sorted(out)


def strip(src, keep_strings=False):
    """Blank out comments (and strings unless keep_strings), keeping offsets and newlines."""
    out = list(src)
    i, n = 0, len(src)

    def blank(a, b):
        for k in range(a, b):
            if out[k] != "\n":
                out[k] = " "
    while i < n:
        c = src[i]
        if src.startswith("//", i):
            j = src.find("\n", i)
            j = n if j < 0 else j
            blank(i, j)
            i = j
        elif src.startswith("/*", i):
            j = src.find("*/", i + 2)
            j = n if j < 0 else j + 2
            blank(i, j)
            i = j
        elif c in "'\"`":
            j = i + 1
            while j < n and src[j] != c:
                j += 2 if src[j] == "\\" else 1
            if not keep_strings:
                blank(i + 1, j)
                if c == "`":
                    out[i] = "`"
            i = j + 1
        else:
            i += 1
    return "".join(out)


def line_of(src, offset):
    return src.count("\n", 0, offset) + 1


def block_end(code, open_brace):
    depth = 0
    for k in range(open_brace, len(code)):
        if code[k] == "{":
            depth += 1
        elif code[k] == "}":
            depth -= 1
            if depth == 0:
                return k
    return len(code) - 1


ES6 = [
    (re.compile(r"(?<![\w.$])let\s+[A-Za-z_$]"), "let"),
    (re.compile(r"(?<![\w.$])const\s+[A-Za-z_$]"), "const"),
    (re.compile(r"=>"), "arrow function"),
    (re.compile(r"\.\.\."), "spread or rest"),
    (re.compile(r"`"), "template literal"),
    (re.compile(r"(?<![\w.$])class\s+[A-Za-z_$][\w$]*\s*(extends|\{)"), "class"),
    (re.compile(r"for\s*\(\s*(var\s+)?[A-Za-z_$][\w$]*\s+of\s"), "for...of"),
    (re.compile(r"function\s*\*"), "generator"),
    (re.compile(r"\bQt\.callLater\b"), "Qt.callLater needs Qt 5.8 (use a zero-interval Timer)"),
]


def check_es5(path, src):
    code = strip(src)
    for rx, what in ES6:
        for m in rx.finditer(code):
            fail(path, line_of(code, m.start()), "ES5 only (PLT-05): %s" % what)


def check_pulleys(path, src):
    code = strip(src)
    for m in re.finditer(r"\b(PullDownMenu|PushUpMenu)\s*\{", code):
        start = m.end() - 1
        end = block_end(code, start)
        body = code[start + 1:end]
        # Direct children only: MenuItem blocks at depth 0 of the menu body.
        depth, count, k = 0, 0, 0
        while k < len(body):
            ch = body[k]
            if ch == "{":
                depth += 1
            elif ch == "}":
                depth -= 1
            elif depth == 0 and body.startswith("MenuItem", k) and re.match(r"MenuItem\s*\{", body[k:]):
                count += 1
            elif depth == 0 and body.startswith("Repeater", k) and re.match(r"Repeater\s*\{", body[k:]):
                dyn = re.search(r"//\s*pulley-dynamic:\s*(\d+)", src[start:start + 1 + end - start])
                if not dyn:
                    fail(path, line_of(code, start + 1 + k), "Repeater in a pulley needs a // pulley-dynamic: <max> note (PLT-06)")
                else:
                    count += int(dyn.group(1))
            k += 1
        if count > PULLEY_LIMIT:
            fail(path, line_of(code, m.start()), "%s has %d items, at most %d (PLT-06)" % (m.group(1), count, PULLEY_LIMIT))
        if count == 0:
            fail(path, line_of(code, m.start()), "%s without items" % m.group(1))


def check_images(path, src):
    code = strip(src, keep_strings=True)
    for m in re.finditer(r"(?<![\w.])Image\s*\{", code):
        start = m.end() - 1
        end = block_end(code, start)
        body = code[start:end]
        src_m = re.search(r"\bsource\s*:\s*(.+)", body)
        if not src_m:
            continue
        value = src_m.group(1)
        if "image://theme" in value or value.strip().startswith('"/usr/share') or "Qt.resolvedUrl" in value:
            continue
        if "sourceSize" not in body:
            fail(path, line_of(code, m.start()), "network image without sourceSize (PLT-06)")
        if "asynchronous: true" not in body:
            fail(path, line_of(code, m.start()), "network image must decode asynchronously (NFR-PERF-07)")


def check_installed():
    """Every QML directory is installed by sailtube.pro (qml.files)."""
    pro = open(os.path.join(ROOT, "sailtube.pro"), encoding="utf-8").read()
    for name in sorted(os.listdir(QML_ROOT)):
        if os.path.isdir(os.path.join(QML_ROOT, name)) and ("Clients/Sailfish/qml/" + name) not in pro:
            fail(os.path.join(ROOT, "sailtube.pro"), 1, "qml/%s is not in qml.files, so it is not installed" % name)


COLOUR = re.compile(r"\"#[0-9A-Fa-f]{3,8}\"|\"(white|black|red|green|blue|gray|grey|yellow|transparent)\"|Qt\.rgba\(\s*[\d.]")


def check_colours(path, src):
    lines = src.split("\n")
    code = strip(src, keep_strings=True).split("\n")
    for i, line in enumerate(code):
        if COLOUR.search(line) and "theme-exempt:" not in lines[i]:
            if re.search(r"\"transparent\"", line) and "GradientStop" in line:
                continue
            fail(path, i + 1, "colour literal; use Theme colours or add // theme-exempt: <reason>")


TEXT_PROPS = r"(text|label|title|description|placeholderText|hintText|value)"


def check_i18n(path, src):
    lines = src.split("\n")
    code = strip(src, keep_strings=True).split("\n")
    rx = re.compile(r"\b%s\s*:\s*\"([^\"]*[A-Za-z][^\"]*)\"" % TEXT_PROPS)
    for i, line in enumerate(code):
        m = rx.search(line)
        if m and "i18n-exempt:" not in lines[i]:
            fail(path, i + 1, "untranslated literal %r; wrap it in qsTr or add // i18n-exempt: <reason>" % m.group(2))
    # Every qsTr carries a translator comment within the three lines above it.
    for i, line in enumerate(code):
        for m in re.finditer(r"\bqsTr\(\s*\"", line):
            window = lines[max(0, i - 3):i + 1]
            if not any("//:" in w for w in window):
                fail(path, i + 1, "qsTr without a //: translator comment")


def check_connections(path, src):
    code = strip(src)
    for m in re.finditer(r"\bConnections\s*\{", code):
        body = code[m.end() - 1:block_end(code, m.end() - 1)]
        if re.search(r"^\s*enabled\s*:", body, re.M):
            fail(path, line_of(code, m.start()), "Connections.enabled does not exist on Qt 5.6; retarget instead (PLT-05)")


def check_accessible_names(path, src):
    # Accessibility: an icon-only button (IconButton, or a MouseArea with an id ending in
    # "Button") has no visible text, so a screen reader needs Accessible.name.
    code = strip(src, keep_strings=True)
    for m in re.finditer(r"\b(IconButton|MouseArea)\s*\{", code):
        end = block_end(code, m.end() - 1)
        body = code[m.end() - 1:end]
        if m.group(1) == "MouseArea" and not re.search(r"^\s*id\s*:\s*\w*Button\b", body, re.M):
            continue
        if not re.search(r"\bAccessible\.name\s*:", body):
            fail(path, line_of(code, m.start()), "%s without Accessible.name (icon-only buttons need a screen-reader label)" % m.group(1))


def check_hideable_labels(path, src):
    # Accessibility: a button that shows an Icon and a Label that can be hidden (a navigation
    # tab in its icon-only mode, say) is icon-only in that state, so it needs Accessible.name.
    # A label that is always shown names the button, so only all-hideable labels count.
    code = strip(src, keep_strings=True)
    for m in re.finditer(r"\b(BackgroundItem|ListItem|MouseArea)\s*\{", code):
        body = code[m.end() - 1:block_end(code, m.end() - 1)]
        if not re.search(r"\bIcon\s*\{", body):
            continue
        labels = [body[lm.end() - 1:block_end(body, lm.end() - 1)] for lm in re.finditer(r"\bLabel\s*\{", body)]
        # Icon-only in some state: every label can be hidden (one always shown names it).
        hideable = labels and all(re.search(r"^\s*visible\s*:", lb, re.M) for lb in labels)
        if hideable and not re.search(r"\bAccessible\.name\s*:", body):
            fail(path, line_of(code, m.start()), "%s shows an Icon and a hideable Label without Accessible.name" % m.group(1))


def check_mirroring(path, src):
    # I18N-03: LayoutMirroring flips anchors, not x. An item placed with an asymmetric x that
    # other items anchor to stays on the left in right-to-left layouts while they move.
    code = strip(src)
    for m in re.finditer(r"^\s*id\s*:\s*(\w+)", code, re.M):
        start = code.rfind("{", 0, m.start())
        end = block_end(code, start) if start >= 0 else -1
        body = code[start:end] if end > start else ""
        x = re.search(r"^\s*x\s*:\s*([^\n]+)", body, re.M)
        if not x or "2 * x" in body:
            continue
        if re.search(r"\b%s\.(right|left)\b" % m.group(1), code):
            fail(path, line_of(code, m.start()), "%s is placed with x but anchored to; use anchors.left so RTL mirrors it" % m.group(1))


_ROLES = None


def model_roles():
    """Role names the C++ list models expose ({ XRole, "name" } in roleNames())."""
    global _ROLES
    if _ROLES is None:
        _ROLES = set()
        for top in ("Clients", "Core"):
            for base, _, names in os.walk(os.path.join(ROOT, top)):
                for n in names:
                    if n.endswith(".cpp"):
                        text = open(os.path.join(base, n), encoding="utf-8", errors="replace").read()
                        _ROLES |= set(re.findall(r"\{\s*\w+Role\s*,\s*\"(\w+)\"\s*\}", text))
    return _ROLES


def check_role_shadowing(path, src):
    # Inside a delegate a model role wins over an id of the same name: a ChannelModel with
    # id "channel" whose rows have a "channel" role made channel.get() a string method call in
    # every row (device run 2: tapping a channel row did nothing). Flag ids that the file also
    # reads as a role (model.<id>) and dereferences (<id>.something).
    # Only ids declared outside the delegates matter: an id declared in the delegate itself wins
    # over the role of that delegate.
    code = strip(src)
    first = code.find("delegate:")
    if first < 0:
        return
    for m in re.finditer(r"^\s*id\s*:\s*(\w+)", code, re.M):
        name = m.group(1)
        if m.start() > first:
            continue
        inside = code[first:]
        is_role = name in model_roles() or re.search(r"\bmodel\.%s\b" % name, code)
        if is_role and re.search(r"(?<![\w.])%s\.\w" % name, inside):
            fail(path, line_of(code, m.start()), "id %s is also a model role here; inside delegates %s means the role (rename the id)" % (name, name))


def check_shadowing(path, src):
    # An id wins over a same-named property in scripts and bindings of the file, so a property
    # named like an id silently reads or writes the other object.
    code = strip(src)
    ids = set(re.findall(r"^\s*id\s*:\s*(\w+)", code, re.M))
    for m in re.finditer(r"^\s*(?:readonly\s+|default\s+)*property\s+[\w.<>]+\s+(\w+)", code, re.M):
        if m.group(1) in ids:
            fail(path, line_of(code, m.start()), "property %s has the same name as an id in this file" % m.group(1))


# Context objects and the C++ headers that declare their signals.
CONTEXT_HEADERS = {
    "playback": "Clients/Sailfish/Presentation/PlaybackSession.h",
    "searchModel": "Clients/Sailfish/Presentation/SearchModel.h",
    "appSettings": "Clients/Sailfish/Presentation/AppSettings.h",
    "startupTiming": "Clients/Sailfish/Presentation/StartupTiming.h",
}


def header_signals(rel):
    src = open(os.path.join(ROOT, rel), encoding="utf-8").read()
    m = re.search(r"\bsignals:(.*?)(?:\n\s*(?:public|private|protected)\b|\n};)", src, re.S)
    names = set(re.findall(r"void\s+(\w+)\s*\(", m.group(1))) if m else set()
    # QAbstractItemModel signals a list model inherits.
    if "QAbstractListModel" in src or "QAbstractItemModel" in src:
        names |= {"rowsInserted", "rowsRemoved", "modelReset", "dataChanged", "layoutChanged", "countChanged"}
    return names


def check_context_connections(path, src):
    """A Connections handler on a C++ context object must name a signal its header declares;
    a mock would accept onFooChanged for any property, the device would not."""
    code = strip(src)
    for m in re.finditer(r"\bConnections\s*\{", code):
        body = code[m.end() - 1:block_end(code, m.end() - 1)]
        t = re.search(r"\btarget\s*:\s*(\w+)\s*$", body, re.M)
        if not t or t.group(1) not in CONTEXT_HEADERS:
            continue
        signals = header_signals(CONTEXT_HEADERS[t.group(1)])
        for h in re.finditer(r"^\s*on([A-Z]\w*)\s*:", body, re.M):
            name = h.group(1)[0].lower() + h.group(1)[1:]
            if name not in signals:
                fail(path, line_of(code, m.end() - 1 + h.start()), "%s has no signal %s (see %s)" % (t.group(1), name, CONTEXT_HEADERS[t.group(1)]))


def check_logic(path, src):
    lines = [l for l in src.split("\n") if l.strip() and not l.strip().startswith("//")]
    if not lines or lines[0].strip() != ".pragma library":
        fail(path, 1, "decision library must start with .pragma library (NFR-TST-04)")
    code = strip(src)
    for rx, what in [(r"\bQt\.", "Qt global"), (r"\bTheme\.", "Theme"), (r"\bqsTr\b", "qsTr"),
                     (r"^\s*import\s", "QML import")]:
        m = re.search(rx, code, re.M)
        if m:
            fail(path, line_of(code, m.start()), "decision library uses %s; keep it free of QML types (NFR-TST-04)" % what)


def check_setting_keys():
    """SET-02: every inventory key with a settings row is bound by exactly one control."""
    inv = open(os.path.join(QML_ROOT, "logic", "SettingsInventory.js"), encoding="utf-8").read()
    entries = re.findall(r'key:\s*"([^"]+)",\s*section:\s*"([^"]*)",\s*kind:\s*"(\w+)"', inv)
    uses = {}
    for path in files(".qml"):
        src = open(path, encoding="utf-8").read()
        for m in re.finditer(r'settingKey\s*:\s*"([^"]+)"', strip(src, keep_strings=True)):
            uses.setdefault(m.group(1), []).append((path, line_of(src, m.start())))
    keys = set()
    for key, section, kind in entries:
        keys.add(key)
        n = len(uses.get(key, []))
        if kind == "remembered":
            if n:
                fail(uses[key][0][0], uses[key][0][1], "%s is remembered by the player and has no settings row" % key)
        elif n != 1:
            where = uses.get(key, [(os.path.join(QML_ROOT, "logic", "SettingsInventory.js"), 1)])[0]
            fail(where[0], where[1], "setting %s has %d controls, needs exactly 1 (SET-02)" % (key, n))
    for key, places in uses.items():
        if key not in keys:
            fail(places[0][0], places[0][1], "settingKey %s is not in SettingsInventory.js" % key)


def check_root():
    path = os.path.join(QML_ROOT, "harbour-sailtube.qml")
    src = open(path, encoding="utf-8").read()
    if "LayoutMirroring.enabled" not in src or "LayoutMirroring.childrenInherit: true" not in src:
        fail(path, 1, "root window must enable LayoutMirroring for right-to-left locales")


def main():
    for path in files(".qml"):
        src = open(path, encoding="utf-8").read()
        if "SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0" not in src:
            fail(path, 1, "missing SPDX header")
        check_es5(path, src)
        check_pulleys(path, src)
        check_images(path, src)
        check_role_shadowing(path, src)
        check_colours(path, src)
        check_i18n(path, src)
        check_connections(path, src)
        check_context_connections(path, src)
        check_shadowing(path, src)
        check_mirroring(path, src)
        check_accessible_names(path, src)
        check_hideable_labels(path, src)
    for path in files(".js"):
        src = open(path, encoding="utf-8").read()
        check_es5(path, src)
        if os.sep + "logic" + os.sep in path:
            check_logic(path, src)
    check_setting_keys()
    check_root()
    check_installed()
    for f in failures:
        print("FAIL " + f)
    print("qml lint: %d qml, %d js, %d problems" % (len(files(".qml")), len(files(".js")), len(failures)))
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
