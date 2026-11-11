#!/usr/bin/env python3
# SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
"""PLT-03: every QSettings in shipped code (Core/, Clients/) names its file explicitly. A default
QSettings() or QSettings(org, app) writes <GenericConfigLocation>/<org>/<app>.conf, outside the
Sailjail config directory, and splits the settings over two files."""
import os, re, sys

root = sys.argv[1]
pattern = re.compile(r"\bQSettings\s*(?:\(\s*\)|\s+[A-Za-z_]\w*\s*;|\(\s*QStringLiteral\(\"[^\"]*\"\)\s*,\s*QStringLiteral|\(\s*\"[^\"]*\"\s*,\s*\")")
bad = []
for top in ("Core", "Clients"):
    for d, _, files in os.walk(os.path.join(root, top)):
        for f in files:
            if not f.endswith((".cpp", ".h")):
                continue
            path = os.path.join(d, f)
            rel = os.path.relpath(path, root)
            text = open(path, encoding="utf-8", errors="replace").read()
            text = re.sub(r"//[^\n]*", "", text)
            text = re.sub(r"/\*.*?\*/", "", text, flags=re.S)
            for m in pattern.finditer(text):
                line = text.count("\n", 0, m.start()) + 1
                # A class member ("QSettings settings_;" in a header) is initialised by its
                # constructor; only a local variable of that form is a default QSettings.
                if f.endswith(".h") and re.match(r"QSettings\s+[A-Za-z_]\w*\s*;", m.group(0)):
                    continue
                # The one sanctioned probe: the legacy path for the migration.
                if rel.endswith("Presentation/SettingsFile.cpp") and "probe" in m.group(0):
                    continue
                bad.append("%s:%d: %s" % (rel, line, m.group(0).strip()))
for b in bad:
    print("catalog_settings: QSettings without an explicit file: " + b)
print("catalog_settings scan: %s (%d finding(s))" % ("FAIL" if bad else "PASS", len(bad)))
sys.exit(1 if bad else 0)
