#!/bin/sh
# SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
# NFR-TST-01: recorder scrub rules, refusal to save, and every committed fixture scrubbed. Offline.
set -e
command -v python3 >/dev/null 2>&1 || { echo "catalog_fixture_scrub: SKIPPED (no python3)"; exit 0; }
python3 -I -B "$(dirname "$0")/scrub_test.py"
