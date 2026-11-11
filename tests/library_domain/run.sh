#!/bin/sh
# SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
# Qt-free library policy tests; needs only a C++11 compiler. Run from anywhere.
set -e
root=$(cd "$(dirname "$0")/../.." && pwd)
out=${TMPDIR:-/tmp}/sailtube-library-domain-test.$$
"${CXX:-c++}" -std=c++11 -Wall -Wextra -I"$root" -o "$out" "$root/tests/library_domain/LibraryDomainTest.cpp" \
    "$root/Core/Library/Domain/LibraryPolicy.cpp"
"$out"
rm -f "$out"
