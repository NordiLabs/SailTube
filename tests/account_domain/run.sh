#!/bin/sh
# SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
# Account policy tests (ACC-02, 03, 06..09). The policy is std-only; the test
# links Qt5Core only for an independent SHA-1 (QCryptographicHash).
set -e
root=$(cd "$(dirname "$0")/../.." && pwd)
if ! pkg-config --exists Qt5Core 2>/dev/null; then echo "account domain tests: SKIPPED (no Qt5Core)"; exit 0; fi
out=${TMPDIR:-/tmp}/sailtube-account-domain-test.$$
"${CXX:-c++}" -std=c++11 -fPIC -Wall -Wextra -I"$root" $(pkg-config --cflags Qt5Core) -o "$out" \
    "$root/tests/account_domain/AccountDomainTest.cpp" \
    "$root/Core/Account/Domain/AccountPolicy.cpp" \
    $(pkg-config --libs Qt5Core)
"$out"
rm -f "$out"
