#!/bin/sh
# SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
# App icon: an original PNG at each Sailfish size (86, 108, 128, 172), square and matching its
# directory, listed in SAILFISHAPP_ICONS, and installed by the spec's %files glob.
set -e
root=$(cd "$(dirname "$0")/../.." && pwd)
python3 -I - "$root" <<'PY'
import os, re, struct, sys
root = sys.argv[1]
sizes = [86, 108, 128, 172]
bad = []
for s in sizes:
    p = os.path.join(root, "icons", "%dx%d" % (s, s), "harbour-sailtube.png")
    if not os.path.exists(p):
        bad.append("missing %s" % p); continue
    head = open(p, "rb").read(24)
    if head[:8] != b"\x89PNG\r\n\x1a\n":
        bad.append("not a PNG: %s" % p); continue
    w, h = struct.unpack(">II", head[16:24])
    if (w, h) != (s, s):
        bad.append("%s is %dx%d" % (p, w, h))
pro = open(os.path.join(root, "sailtube.pro"), encoding="utf-8").read()
m = re.search(r"SAILFISHAPP_ICONS\s*=\s*(.+)", pro)
if not m or sorted(m.group(1).split()) != sorted("%dx%d" % (s, s) for s in sizes):
    bad.append("SAILFISHAPP_ICONS does not list the four sizes")
spec = open(os.path.join(root, "rpm", "harbour-sailtube.spec"), encoding="utf-8").read()
if "%{_datadir}/icons/hicolor/*/apps/%{name}.png" not in spec:
    bad.append("spec does not install the icons")
for b in bad: print("FAIL " + b)
print("icons: %d sizes, %d problems" % (len(sizes), len(bad)))
sys.exit(1 if bad else 0)
PY
