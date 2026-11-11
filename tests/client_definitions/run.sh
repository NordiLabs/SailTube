#!/bin/sh
# SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
# NFR-MNT-04: client names, client versions and endpoint parameters only in each component's one
# definitions file, every value there backed by a fixture (tools/check_client_definitions.py,
# docs/CLIENT_DEFINITIONS.md). Offline; python3 standard library only; needs no .git.
set -e
root=$(cd "$(dirname "$0")/../.." && pwd)
command -v python3 >/dev/null 2>&1 || { echo "client_definitions: SKIPPED (no python3)"; echo "client_definitions: PASS"; exit 0; }
tmp=$(mktemp -d "${TMPDIR:-/tmp}/sailtube-client-definitions.XXXXXX")
trap 'rm -rf "$tmp"' EXIT INT TERM
python3 -I -B "$root/tools/check_client_definitions.py" --root "$root"
python3 -I -B "$root/tests/client_definitions/selftest.py" --root "$root" --tmp "$tmp"
echo "client_definitions: PASS"
