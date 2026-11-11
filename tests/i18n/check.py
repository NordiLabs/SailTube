# SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
"""Translation build check (I18N-03).

* The .pro TRANSLATIONS, translations/*.ts and the spec's lrelease loop name the same 10 locales.
* Every catalog is well formed, has every message translated (no unfinished, no empty), and
  keeps the source's %1..%9 placeholders.
* Every qsTr()/qsTranslate() literal in the QML sources is in the catalogs (catalogs are current).
* QT_LAYOUT_DIRECTION is RTL exactly for Arabic, LTR elsewhere (RTL mirroring).
* With lrelease available, each catalog compiles with -nounfinished to a non-empty .qm, so the
  .ts and .qm sets match.
* No //: translator comment in the C++ sources directly follows a ')' (an unbraced if / for /
  while, or the second branch of a conditional expression): Qt 5.6's lupdate drops such a comment
  without a word. Brace the statement, or put the comment before it. (packaged.sh also checks the
  result of the real lupdate run, tests/i18n/lost_comments.py.)
"""
import glob
import os
import re
import shutil
import subprocess
import sys
import tempfile
import xml.etree.ElementTree as ET

ROOT = os.path.abspath(sys.argv[1] if len(sys.argv) > 1 else os.path.join(os.path.dirname(__file__), "..", ".."))
LOCALES = ["en", "es", "pt", "fr", "de", "it", "ru", "ja", "zh_CN", "ar"]
RTL = {"ar"}
problems = []


def bad(msg):
    problems.append(msg)


pro = open(os.path.join(ROOT, "sailtube.pro"), encoding="utf-8").read()
pro_locales = re.findall(r"translations/harbour-sailtube-([A-Za-z_]+)\.ts", pro)
ts_locales = sorted(re.sub(r".*harbour-sailtube-(.*)\.ts$", r"\1", p)
                    for p in glob.glob(os.path.join(ROOT, "translations", "harbour-sailtube-*.ts")))
spec = open(os.path.join(ROOT, "rpm", "harbour-sailtube.spec"), encoding="utf-8").read()
loop = re.search(r"for locale in ([^;]+);", spec)
spec_locales = loop.group(1).split() if loop else []
for name, got in (("sailtube.pro", pro_locales), ("translations/", ts_locales), ("spec lrelease loop", spec_locales)):
    if sorted(got) != sorted(LOCALES):
        bad("%s locales %s differ from %s" % (name, sorted(got), sorted(LOCALES)))

TRANSLATOR_COMMENT = re.compile(r"//[:~=]")


def dropped_translator_comments(text):
    """Line numbers of //: comment blocks that directly follow a ')' in C++ source text."""
    out = []
    lines = text.split("\n")
    for i, line in enumerate(lines):
        m = TRANSLATOR_COMMENT.search(line)
        if not m:
            continue
        before = line[:m.start()].strip()
        if before.startswith("//"):
            continue                      # an ordinary comment that mentions the marker
        if before:
            if before.endswith(")"):      # if (x) //: note
                out.append(i + 1)
            continue
        j = i - 1
        while j >= 0 and (not lines[j].strip() or lines[j].strip().startswith("//")):
            if TRANSLATOR_COMMENT.match(lines[j].strip()):
                break                     # a later line of the same comment block
            j -= 1
        if j < 0 or TRANSLATOR_COMMENT.match(lines[j].strip()):
            continue
        code = lines[j].split("//")[0].rstrip() if "//" in lines[j] and '"' not in lines[j] else lines[j].rstrip()
        if code.endswith(")"):
            out.append(i + 1)
    return out


# The check knows the forms lupdate drops and the ones it keeps.
_sample = """if (a)
    //: lost
    return tr("x");
if (b) {
    //: kept
    return tr("y");
}
//: kept
if (c) return tr("z");
v = cond()
    //: lost
    ? tr("p") : q;
if (d) //: lost
    return tr("w");
else
    //: kept
    return tr("u");
"""
if dropped_translator_comments(_sample) != [2, 11, 13]:
    bad("dropped_translator_comments self-check: %r" % dropped_translator_comments(_sample))

for base in (os.path.join(ROOT, "Clients", "Sailfish"), os.path.join(ROOT, "Core")):
    for folder, _, names in os.walk(base):
        for n in sorted(names):
            if n.endswith((".cpp", ".h")):
                path = os.path.join(folder, n)
                for ln in dropped_translator_comments(open(path, encoding="utf-8", errors="replace").read()):
                    bad("%s:%d: translator comment directly after ')' is dropped by lupdate: "
                        "brace the statement or move the comment before it" % (os.path.relpath(path, ROOT), ln))


def unescape(text):
    # Only the QML escapes; unicode_escape would mangle non-ASCII text such as an ellipsis.
    return re.sub(r'\\(.)', lambda m: {"n": "\n", "t": "\t"}.get(m.group(1), m.group(1)), text)


sources = set()
for base, _, names in os.walk(os.path.join(ROOT, "Clients", "Sailfish", "qml")):
    for n in names:
        if n.endswith((".qml", ".js")):
            src = open(os.path.join(base, n), encoding="utf-8").read()
            for m in re.finditer(r'qsTr\(\s*"((?:[^"\\]|\\.)*)"', src):
                sources.add(unescape(m.group(1)))
            for m in re.finditer(r'qsTranslate\(\s*"[^"]*"\s*,\s*"((?:[^"\\]|\\.)*)"', src):
                sources.add(unescape(m.group(1)))

for loc in LOCALES:
    path = os.path.join(ROOT, "translations", "harbour-sailtube-%s.ts" % loc)
    if not os.path.exists(path):
        continue
    try:
        root = ET.parse(path).getroot()
    except ET.ParseError as e:
        bad("%s: malformed: %s" % (loc, e))
        continue
    seen = set()
    count = 0
    for ctx in root.iter("context"):
        for m in ctx.iter("message"):
            count += 1
            src = m.findtext("source") or ""
            seen.add(src)
            t = m.find("translation")
            kind = t.get("type") if t is not None else "missing"
            if kind in ("obsolete", "vanished"):
                continue
            forms = [f.text or "" for f in t.findall("numerusform")] if t is not None else []
            texts = forms or [(t.text or "") if t is not None else ""]
            if kind in ("unfinished", "missing") or not all(x.strip() for x in texts):
                bad("%s: untranslated %r (%s)" % (loc, src[:50], ctx.findtext("name")))
                continue
            want = sorted(re.findall(r"%\d", src))
            for x in texts:
                if sorted(re.findall(r"%\d", x)) != want:
                    bad("%s: placeholders differ in %r" % (loc, src[:50]))
            if src == "QT_LAYOUT_DIRECTION" and texts[0] != ("RTL" if loc in RTL else "LTR"):
                bad("%s: QT_LAYOUT_DIRECTION is %r" % (loc, texts[0]))
    missing = sorted(s for s in sources if s not in seen)
    for s in missing[:5]:
        bad("%s: QML string not in catalog (run tools/normalize_ts.sh): %r" % (loc, s[:60]))
    if len(missing) > 5:
        bad("%s: %d more QML strings missing" % (loc, len(missing) - 5))
    if "QT_LAYOUT_DIRECTION" not in seen:
        bad("%s: no QT_LAYOUT_DIRECTION entry" % loc)

lrelease = shutil.which("lrelease") or ("/usr/lib64/qt5/bin/lrelease" if os.path.exists("/usr/lib64/qt5/bin/lrelease") else None)
compiled = 0
if lrelease:
    out = tempfile.mkdtemp()
    for loc in LOCALES:
        ts = os.path.join(ROOT, "translations", "harbour-sailtube-%s.ts" % loc)
        qm = os.path.join(out, "harbour-sailtube-%s.qm" % loc)
        p = subprocess.run([lrelease, "-nounfinished", ts, "-qm", qm], capture_output=True, text=True)
        if p.returncode != 0 or not os.path.exists(qm) or os.path.getsize(qm) < 64:
            bad("%s: lrelease failed or produced an empty .qm" % loc)
        else:
            compiled += 1
    shutil.rmtree(out, ignore_errors=True)

for p in problems:
    print("FAIL " + p)
print("i18n: %d locales, %d QML strings, %s, %d problems" % (
    len(LOCALES), len(sources), "%d .qm compiled" % compiled if lrelease else "lrelease not found (compile step skipped)", len(problems)))
sys.exit(1 if problems else 0)
