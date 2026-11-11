#!/bin/sh
# SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
# Static QML checks: ES5 (PLT-05), pulley size and image decode size (PLT-06), one control per
# setting (SET-02), translatable strings, theme colours, RTL mirroring.
set -e
root=$(cd "$(dirname "$0")/../.." && pwd)
python3 -I "$root/tests/qml_lint/lint.py" "$root"
