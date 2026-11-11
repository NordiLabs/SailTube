#!/bin/sh
# SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
# The optional account session (ACC-01..04, 06..10, IMP-02 transport) over a fake
# transport and fake vault; browser import over a generated WAL database.
set -e
root=$(cd "$(dirname "$0")/../.." && pwd)
for m in Qt5Core Qt5Network Qt5Sql; do
    if ! pkg-config --exists $m 2>/dev/null; then echo "account service tests: SKIPPED (no $m)"; exit 0; fi
done
out=${TMPDIR:-/tmp}/sailtube-account-service-test.$$
rm -rf "$out"; mkdir -p "$out"
moc=$(pkg-config --variable=host_bins Qt5Core 2>/dev/null)/moc
[ -x "$moc" ] || moc=moc
"$moc" -I"$root" "$root/Core/Account/Infrastructure/AccountService.h" -o "$out/moc_service.cpp"
"$moc" -I"$root" "$root/Core/Account/Infrastructure/AuthTransport.h" -o "$out/moc_transport.cpp"
"$moc" -I"$root" "$root/Clients/Sailfish/Presentation/AccountController.h" -o "$out/moc_controller.cpp"
"$moc" -I"$root" "$root/Core/Account/Infrastructure/SignedCatalogTransport.h" -o "$out/moc_signed.cpp"
qtflags=$(pkg-config --cflags Qt5Core Qt5Network Qt5Sql | sed 's/-I/-isystem /g')
"${CXX:-c++}" -std=c++11 -fPIC -Wall -Wextra -I"$root" $qtflags -o "$out/t" \
    "$root/tests/account_service/AccountServiceTest.cpp" \
    "$root/Core/Account/Domain/AccountPolicy.cpp" \
    "$root/Core/Account/Infrastructure/AccountStore.cpp" \
    "$root/Core/Account/Infrastructure/AuthTransport.cpp" \
    "$root/Core/Account/Infrastructure/AccountService.cpp" \
    "$root/Core/Account/Infrastructure/BrowserCookieImport.cpp" \
    "$root/Clients/Sailfish/Presentation/AccountController.cpp" \
    "$root/Core/Account/Infrastructure/SignedCatalogTransport.cpp" "$out/moc_signed.cpp" \
    "$out/moc_service.cpp" "$out/moc_transport.cpp" "$out/moc_controller.cpp" \
    $(pkg-config --libs Qt5Core Qt5Network Qt5Sql)
TEST_DIR="$out" "$out/t"
rm -rf "$out"
