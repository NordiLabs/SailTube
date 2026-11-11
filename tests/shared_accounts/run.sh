#!/bin/sh
# SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
# SailTune's own tests of the pinned account/session pieces, from Shared/SailTune/tests/ (unchanged):
#   golden vectors   gen_golden_vectors.py --check (SHA-1 and SAPISIDHASH V1/V2/V3 from Python's hashlib)
#   ytm_session      test_pure: SHA-1, signer vectors, import parser (+ fuzz, leak scans), expiry, bundle codec, holder
#   ytm_sources      test_pure: cookie-jar row selection through the real parser, enumeration, scratch helpers
# Not run here: SailTune's test_session/test_qt (they need SailTune's account service, not pinned) and its
# check_structure.py scripts (they check SailTune's own .pri and translation files). All cookie values in
# the pinned tests are synthetic.
set -eu
root=$(cd "$(dirname "$0")/../.." && pwd)
st="$root/Shared/SailTune"
CXX=${CXX:-c++}
out=$(mktemp -d "${TMPDIR:-/tmp}/sailtube-shared-accounts.XXXXXX")
trap 'rm -rf "$out"' EXIT
python3 -I "$st/tests/ytm_session/gen_golden_vectors.py" --check
FLAGS="-std=c++11 -fPIC -pthread -Wall -Wextra -Wno-deprecated-copy -I$st"
A="$st/Core/Accounts/Infrastructure"
S="$st/Core/Ytm/Sources"
$CXX $FLAGS "$st/tests/ytm_session/test_pure.cpp" "$A/Sha1.cpp" "$A/SessionSigner.cpp" "$A/SessionExpiryDetector.cpp" \
    "$A/SessionImportParser.cpp" "$A/SessionBundleCodec.cpp" "$A/SessionHolder.cpp" -o "$out/session_pure"
"$out/session_pure"
$CXX $FLAGS "$st/tests/ytm_sources/test_pure.cpp" "$A/Sha1.cpp" "$A/SessionImportParser.cpp" \
    "$S/Domain/CookieJarSelection.cpp" "$S/Domain/AccountEnumeration.cpp" "$S/Application/BrowserCookieJarSource.cpp" \
    "$S/Infrastructure/PrivateScratch.cpp" -o "$out/sources_pure"
"$out/sources_pure"
echo "shared_accounts: PASS"
