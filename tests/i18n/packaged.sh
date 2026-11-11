#!/bin/sh
# SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
# I18N-01/02: run the packaging's lupdate step on a copy of the sources and fail when any
# non-English catalog would leave a source text untranslated (lrelease -nounfinished drops
# those, so they show in English on the device). Needs lupdate (the build container has it).
set -e
root=$(cd "$(dirname "$0")/../.." && pwd)
command -v lupdate >/dev/null 2>&1 || { echo "i18n packaged: lupdate not found, skipped"; exit 0; }
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT
cp -R "$root/Clients" "$root/Core" "$root/translations" "$work/"
cd "$work"
ts=""
for f in translations/harbour-sailtube-*.ts; do ts="$ts $f"; done
lupdate -noobsolete Clients/Sailfish Core -ts $ts >"$work/lupdate.log" 2>&1 || { cat "$work/lupdate.log"; exit 1; }
python3 -I - "$work/translations" <<'PY'
import glob, os, sys
import xml.etree.ElementTree as ET
bad = 0
for path in sorted(glob.glob(os.path.join(sys.argv[1], "harbour-sailtube-*.ts"))):
    loc = os.path.basename(path)[len("harbour-sailtube-"):-3]
    missing = []
    for ctx in ET.parse(path).getroot().iter("context"):
        for m in ctx.iter("message"):
            t = m.find("translation")
            kind = t.get("type") if t is not None else "unfinished"
            if kind in ("obsolete", "vanished"):
                continue
            text = "".join(t.itertext()).strip() if t is not None else ""
            if kind == "unfinished" or not text:
                missing.append("%s: %s" % (ctx.findtext("name"), m.findtext("source")))
    if missing:
        bad += 1
        print("FAIL %s: %d untranslated after lupdate (run tools/normalize_ts.sh and translate)" % (loc, len(missing)))
        for s in missing[:10]:
            print("  " + s)
print("i18n packaged: %d catalogs, %d incomplete" % (len(glob.glob(os.path.join(sys.argv[1], "harbour-sailtube-*.ts"))), bad))
sys.exit(1 if bad else 0)
PY
# Every //: translator comment in the sources reached a message (lupdate drops some silently).
python3 -I "$root/tests/i18n/lost_comments.py" "$work" "$work/translations/harbour-sailtube-en.ts"
