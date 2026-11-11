# SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
"""Translator comments that lupdate did not attach to any message.

    lost_comments.py <source root> <catalog.ts written by lupdate>

Every //: comment in Clients/Sailfish and Core must be the <extracomment> of a message. Qt 5.6's
lupdate silently drops one that directly follows a ')' (an unbraced if / for / while, the second
branch of a conditional expression): the translator then gets the string without its note.
tests/i18n/packaged.sh runs this after the packaging's lupdate step.
"""
import os, re, sys
import xml.etree.ElementTree as ET
root, ts = sys.argv[1], sys.argv[2]
norm = lambda s: ' '.join(s.split())
have = set()
for m in ET.parse(ts).getroot().iter('message'):
    for part in (m.findtext('extracomment') or '').split('----------'):
        have.add(norm(part))
joined = ' \x00 '.join(sorted(have))
lost = []
for base in ('Clients/Sailfish', 'Core'):
    for d, _, names in os.walk(os.path.join(root, base)):
        for n in sorted(names):
            if not n.endswith(('.cpp', '.h', '.qml', '.js')):
                continue
            p = os.path.join(d, n)
            lines = open(p, encoding='utf-8', errors='replace').read().split('\n')
            i = 0
            while i < len(lines):
                m = re.match(r'\s*(?:.*?\s)?//:\s?(.*)$', lines[i]) if '//:' in lines[i] else None
                if not m or re.match(r'\s*//\s', lines[i]):
                    i += 1
                    continue
                start = i
                block = [m.group(1)]
                while i + 1 < len(lines) and re.match(r'\s*//:', lines[i + 1]):
                    i += 1
                    block.append(re.match(r'\s*//:\s?(.*)$', lines[i]).group(1))
                text = norm(' '.join(block))
                if text and text not in have and text not in joined:
                    lost.append((os.path.relpath(p, root), start + 1, text))
                i += 1
for f, ln, text in lost:
    print('%s:%d: %s' % (f, ln, text[:90]))
print('lost translator comments: %d' % len(lost))
sys.exit(1 if lost else 0)
