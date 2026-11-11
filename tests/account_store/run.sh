#!/bin/sh
# SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
# QtCore tests of the account session store (ACC-08, ACC-10, NFR-SEC-03).
# Runs in the SDK/native container; skipped where Qt5Core is absent.
set -e
root=$(cd "$(dirname "$0")/../.." && pwd)
if ! pkg-config --exists Qt5Core 2>/dev/null; then echo "account store tests: SKIPPED (no Qt5Core)"; exit 0; fi
out=${TMPDIR:-/tmp}/sailtube-account-store-test.$$
mkdir -p "$out"
"${CXX:-c++}" -std=c++11 -fPIC -pthread -Wall -Wextra -I"$root" \
    $(pkg-config --cflags Qt5Core) -o "$out/t" \
    "$root/tests/account_store/AccountStoreTest.cpp" \
    "$root/Core/Account/Infrastructure/AccountStore.cpp" \
    "$root/Core/Account/Domain/AccountPolicy.cpp" \
    $(pkg-config --libs Qt5Core)
ACCOUNT_STORE_TMP="$out/data" "$out/t"
rm -rf "$out"
