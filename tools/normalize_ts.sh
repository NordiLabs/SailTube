#!/bin/bash
# Normalize SailTube .ts catalogs the way packaging sees them.
# SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#
#   normalize_ts.sh <checkout>
#
# 1. Runs the spec's exact lupdate call (-noobsolete) on a snapshot inside sfos-m1-sailtube, so C++
#    strings are keyed by their fully qualified class context like in the RPM build.
# 2. Refills every message lupdate leaves unfinished from the previous catalogs by source text
#    (same short class name first, then any context), so translations filed under a wrong
#    context are not lost.
# 3. Verifies that every finished translation has exactly the source's %1..%9 placeholders.
# Writes the result into <checkout>/translations; prints what is still unfinished. Never commits.
set -euo pipefail
WT=${1:?usage: normalize_ts.sh <checkout>}
D="docker -H unix://$HOME/.orbstack/run/docker.sock"
# SailTube's own build container; never SailTune's sfos-m1-native.
C="${SFOS_NATIVE_CONTAINER:-sfos-m1-sailtube}"
TMP=$(mktemp -d); trap 'rm -rf "$TMP"' EXIT
git -C "$WT" archive HEAD Clients Core translations | tar -x -C "$TMP"
# Include uncommitted catalog edits, if any.
cp "$WT"/translations/*.ts "$TMP/translations/"
$D exec "$C" sh -c 'rm -rf /home/sdk/normts && mkdir -p /home/sdk/normts'
$D cp "$TMP/." "$C":/home/sdk/normts/
$D exec "$C" sh -c 'chown -R sdk /home/sdk/normts; cd /home/sdk/normts && su sdk -s /bin/sh -c "lupdate -noobsolete Clients/Sailfish Core -ts translations/harbour-sailtube.ts translations/harbour-sailtube-en.ts translations/harbour-sailtube-es.ts translations/harbour-sailtube-pt.ts translations/harbour-sailtube-fr.ts translations/harbour-sailtube-de.ts translations/harbour-sailtube-it.ts translations/harbour-sailtube-ru.ts translations/harbour-sailtube-ja.ts translations/harbour-sailtube-zh_CN.ts translations/harbour-sailtube-ar.ts" >/dev/null'
mkdir -p "$TMP/out"
$D cp "$C":/home/sdk/normts/translations/. "$TMP/out/"
python3 -I - "$WT" "$TMP/out" <<'EOF'
import copy, glob, os, re, sys
import xml.etree.ElementTree as ET
wt, out = sys.argv[1], sys.argv[2]
def finished(t):
    return t is not None and t.get('type') not in ('unfinished', 'obsolete', 'vanished') and ''.join(t.itertext()).strip()
def key(m):
    return (m.findtext('source'), m.findtext('comment') or '', m.get('numerus', ''))
bad = 0
for newf in sorted(glob.glob(os.path.join(out, 'harbour-sailtube*.ts'))):
    base = os.path.basename(newf)
    oldp = os.path.join(wt, 'translations', base)
    # The template catalog (harbour-sailtube.ts) is not tracked; a missing catalog is empty.
    old = ET.parse(oldp).getroot() if os.path.exists(oldp) else ET.Element('TS')
    new = ET.parse(newf).getroot()
    lang = new.get('language') or ''
    byctx, bysrc = {}, {}
    for c in old.findall('context'):
        short = c.findtext('name').split('::')[-1]
        for m in c.findall('message'):
            t = m.find('translation')
            if finished(t):
                byctx.setdefault((short,) + key(m), t); bysrc.setdefault(key(m), t)
    filled = left = 0
    for c in new.findall('context'):
        short = c.findtext('name').split('::')[-1]
        for m in c.findall('message'):
            t = m.find('translation')
            if t is None or t.get('type') != 'unfinished':
                continue
            src = byctx.get((short,) + key(m)) or bysrc.get(key(m))
            if src is None or lang in ('', 'en'):
                left += 1; continue
            m.remove(t); nt = copy.deepcopy(src); nt.attrib.pop('type', None); m.append(nt); filled += 1
    if lang not in ('', 'en'):
        for c in new.findall('context'):
            for m in c.findall('message'):
                t = m.find('translation')
                if not finished(t):
                    continue
                want = set(re.findall(r'%\d', m.findtext('source') or ''))
                for x in ([n.text or '' for n in t.findall('numerusform')] or [t.text or '']):
                    if set(re.findall(r'%\d', x)) != want:
                        bad += 1; print('PLACEHOLDER', base, c.findtext('name'), repr((m.findtext('source') or '')[:60]))
    ET.indent(new, space='    ')
    with open(os.path.join(wt, 'translations', base), 'w', encoding='utf-8') as f:
        f.write('<?xml version="1.0" encoding="utf-8"?>\n<!DOCTYPE TS>\n' + ET.tostring(new, encoding='unicode') + '\n')
    print(f'{base}: refilled {filled}, still unfinished {left}')
print('placeholder mismatches:', bad)
sys.exit(1 if bad else 0)
EOF
