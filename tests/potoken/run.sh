#!/bin/sh
# SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
# RES-14 [FULL] optional PO-token provider. Run from anywhere.
#  1. Qt-free policy tests (C++11 only): Deno command line, pinning, state machine, bind check
#     against tests/potoken/fixtures (/proc/net/tcp{,6} samples), yt-dlp arguments.
#  2. STORE has none of it: PoTokenProvider.h refuses to compile with SAILTUBE_STORE, and every
#     provider line of the shared files sits under #ifndef SAILTUBE_STORE.
#  3. QtCore/QtNetwork tests (skipped without Qt5Core/Qt5Network): the service's argument
#     plumbing in FULL and STORE builds, and on Linux the process-level checks with a fake Deno
#     (fake_deno.c) bound to 127.0.0.1 or 0.0.0.0, verified through the real /proc tables.
set -e
root=$(cd "$(dirname "$0")/../.." && pwd)
here="$root/tests/potoken"
out=${TMPDIR:-/tmp}/sailtube-potoken-test.$$
rm -rf "$out"
mkdir -p "$out"
trap 'rm -rf "$out"' EXIT INT TERM

# 1. Domain
"${CXX:-c++}" -std=c++11 -Wall -Wextra -Werror -I"$root" -o "$out/domain" \
    "$here/PoTokenTest.cpp" "$root/Core/Resolver/Domain/PoTokenPolicy.cpp"
"$out/domain" "$here/fixtures"

# 2. STORE guards
printf '#include "Core/Resolver/Infrastructure/PoTokenProvider.h"\n' > "$out/store_guard.cpp"
if "${CXX:-c++}" -std=c++11 -DSAILTUBE_STORE -fsyntax-only -I"$root" "$out/store_guard.cpp" > "$out/store_guard.txt" 2>&1; then
    echo "FAIL potoken: PoTokenProvider.h compiles in the STORE build"; exit 1
fi
grep -q "FULL only" "$out/store_guard.txt" || { echo "FAIL potoken: unexpected STORE guard error"; cat "$out/store_guard.txt"; exit 1; }
guards=0
for f in Clients/Sailfish/Composition/main.cpp Clients/Sailfish/Presentation/AppSettings.h \
         Clients/Sailfish/Presentation/AppSettings.cpp Core/Resolver/Infrastructure/YtDlpVideoService.h \
         Core/Resolver/Infrastructure/YtDlpVideoService.cpp; do
    awk -v file="$f" '
        /^[ \t]*#[ \t]*if/ {
            depth++
            kind[depth] = ($0 ~ /^[ \t]*#[ \t]*ifndef[ \t]+SAILTUBE_STORE/) ? 1 : ($0 ~ /^[ \t]*#[ \t]*ifdef[ \t]+SAILTUBE_STORE/) ? 2 : 0
            full[depth] = (kind[depth] == 1)
            next
        }
        /^[ \t]*#[ \t]*else/ { full[depth] = (kind[depth] == 2); next }
        /^[ \t]*#[ \t]*endif/ { depth--; next }
        /PoTokenProvider|PoTokenPolicy|potoken::|[sS]etPoTokenArguments|poTokenArguments|poTokenHeapMb|poTokenProvider|youtubepot|fetch_pot/ {
            ok = 0
            for (i = 1; i <= depth; i++) if (full[i]) ok = 1
            if (!ok) { printf("FAIL potoken: %s:%d: provider code outside #ifndef SAILTUBE_STORE: %s\n", file, NR, $0); bad = 1 }
            else hits++
        }
        END { exit bad ? 1 : 0 }' "$root/$f" || exit 1
    guards=$((guards + 1))
done
echo "potoken store guards: $guards files checked, PoTokenProvider.h refused with SAILTUBE_STORE"

# 3. Qt
for m in Qt5Core Qt5Network; do
    if ! pkg-config --exists $m 2>/dev/null; then echo "potoken qt tests: SKIPPED (no $m)"; exit 0; fi
done
moc=$(pkg-config --variable=host_bins Qt5Core 2>/dev/null)/moc
[ -x "$moc" ] || moc=moc
qtflags=$(pkg-config --cflags Qt5Core Qt5Network | sed 's/-I/-isystem /g')
qtlibs=$(pkg-config --libs Qt5Core Qt5Network)
"${CC:-cc}" -std=c99 -Wall -Wextra -o "$out/fake_deno" "$here/fake_deno.c"
printf '#!/bin/sh\nprintf "%%s\\n" "$@" > "$SAILTUBE_TEST_RECORD"\n' > "$out/recorder.sh"
chmod 700 "$out/recorder.sh"
common="$root/Core/Resolver/Infrastructure/YtDlpJson.cpp $root/Core/Resolver/Infrastructure/YtDlpRunner.cpp \
    $root/Core/Resolver/Infrastructure/YtDlpVideoService.cpp $root/Core/Platform/Infrastructure/GstCapabilityProbe.cpp \
    $root/Core/Media/Domain/StreamSelection.cpp $root/Core/Media/Domain/AudioTrackPolicy.cpp \
    $root/Core/Media/Domain/LivePolicy.cpp"
for variant in full store; do
    d="$out/$variant"
    mkdir -p "$d"
    defines=""
    [ "$variant" = store ] && defines="-DSAILTUBE_STORE"
    "$moc" $defines -I"$root" "$root/Core/Resolver/Infrastructure/YtDlpRunner.h" -o "$d/moc_runner.cpp"
    "$moc" $defines -I"$root" "$root/Core/Resolver/Infrastructure/YtDlpVideoService.h" -o "$d/moc_service.cpp"
    extra=""
    if [ "$variant" = full ]; then
        "$moc" -I"$root" "$root/Core/Resolver/Infrastructure/PoTokenProvider.h" -o "$d/moc_provider.cpp"
        extra="$root/Core/Resolver/Infrastructure/PoTokenProvider.cpp $root/Core/Resolver/Domain/PoTokenPolicy.cpp $d/moc_provider.cpp"
    fi
    # shellcheck disable=SC2086
    "${CXX:-c++}" -std=c++11 -fPIC -Wall -Wextra $defines -I"$root" $qtflags -o "$d/t" \
        "$here/PoTokenQtTest.cpp" $common $extra "$d/moc_runner.cpp" "$d/moc_service.cpp" $qtlibs
    TMPDIR="$d" "$d/t" "$out/fake_deno" "$out/recorder.sh"
done
# The STORE build of the service carries none of the provider's strings (the test binary
# itself names them, so the service object is checked on its own).
"${CXX:-c++}" -std=c++11 -fPIC -Wall -Wextra -DSAILTUBE_STORE -I"$root" $qtflags -c \
    "$root/Core/Resolver/Infrastructure/YtDlpVideoService.cpp" -o "$out/store/service.o"
for o in "$out/store/service.o" "$out/full/t"; do [ -s "$o" ] || { echo "FAIL potoken: missing $o"; exit 1; }; done
if grep -a -q -e "youtubepot" -e "fetch_pot" -e "plugin-dirs" -e "deny-ffi" "$out/store/service.o"; then
    echo "FAIL potoken: provider strings in the STORE build of YtDlpVideoService"; exit 1
fi
grep -a -q "fetch_pot" "$out/full/t" || { echo "FAIL potoken: FULL build lacks the provider strings (check is blind)"; exit 1; }
echo "potoken: STORE service object has no provider strings"
echo "potoken: PASS"
