#!/bin/sh
# SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
# Translation completeness and build check (I18N-03).
set -e
root=$(cd "$(dirname "$0")/../.." && pwd)
python3 -I "$root/tests/i18n/check.py" "$root"
sh "$root/tests/i18n/packaged.sh"
