#!/bin/sh
# SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
# NFR-SEC-03 / ACC-10: secrets stay out of logs, diagnostics, backups and shared
# locations; secret-bearing temp files are private, 0600 and short-lived.
#  1. Static (python3): check_secret_logging.py on this tree must pass; scratch
#     copies with one seeded logging leak each must fail.
#  2. Qt (Qt5Core, Qt5Network, Qt5Sql): SecretsTest drives sign-in, rotations,
#     extractor calls (success, failure, timeout, cancel, concurrency), backup,
#     diagnostics and sign-out, scanning logs, report, backup and the file system
#     after every step. On Linux the test runs under fs_trace.so (LD_PRELOAD) so
#     short-lived files and transient modes are seen too. Skipped without Qt.
set -eu
here=$(cd "$(dirname "$0")" && pwd)
root=$(cd "$here/../.." && pwd)
work=$(mktemp -d "${TMPDIR:-/tmp}/sailtube-secrets.XXXXXX")
trap 'rm -rf "$work"' EXIT

# ---- 1. static check
if command -v python3 >/dev/null 2>&1; then
    python3 -I "$here/check_secret_logging.py" --root "$root"
    tree="$work/tree"
    mkdir -p "$tree"
    (cd "$root" && tar -cf - Core/Account Core/Resolver/Infrastructure Core/Worker \
        Clients/Sailfish/Presentation Shared/SailTune/Core/Accounts) | tar -xf - -C "$tree"
    python3 -I "$here/check_secret_logging.py" --root "$tree" > "$work/out.txt" 2>&1 \
        || { echo "secrets: FAIL (the unchanged scratch copy is rejected)"; cat "$work/out.txt"; exit 1; }
    seeds=0
    # seed FILE EXPECTED-TEXT: append stdin to FILE, expect a finding naming EXPECTED-TEXT, restore.
    seed() {
        seeds=$((seeds + 1))
        cp "$tree/$1" "$work/orig"
        cat >> "$tree/$1"
        if python3 -I "$here/check_secret_logging.py" --root "$tree" > "$work/out.txt" 2>&1; then
            echo "secrets: FAIL (seeded leak $seeds in $1 was accepted)"; exit 1
        fi
        grep -q "^$1:[0-9]*: .*$2" "$work/out.txt" \
            || { echo "secrets: FAIL (seed $seeds: expected '$2' in $1)"; cat "$work/out.txt"; exit 1; }
        cp "$work/orig" "$tree/$1"
        echo "secrets: seeded leak $seeds rejected as expected ($1: $2)"
    }
    seed Core/Account/Infrastructure/AccountStore.cpp "'jarPath'" <<'EOF'
static void seededJar(const QString &jarPath) { qWarning("account: jar at %s", qPrintable(jarPath)); }
EOF
    seed Core/Account/Infrastructure/AccountService.cpp "'cookie'" <<'EOF'
static void seededCookie(const nordtile::video::account::Cookie &cookie) {
    qDebug() << "rotated" << QString::fromStdString(cookie.value);
}
EOF
    seed Core/Resolver/Infrastructure/YtDlpRunner.cpp "'arguments'" <<'EOF'
static void seededArgs(const QStringList &arguments) { qInfo() << "yt-dlp" << arguments; }
EOF
    seed Core/Resolver/Infrastructure/YtDlpRunner.cpp "'--cookies'" <<'EOF'
static void seededFlag(const char *p) { std::fprintf(stderr, "running with --cookies %s\n", p); }
EOF
    seed Clients/Sailfish/Presentation/AccountController.cpp "'token'" <<'EOF'
static void seededToken(const std::string &token) { std::fprintf(stderr, "%s\n", token.c_str()); }
EOF
    seed Core/Worker/Runtime/worker.py "'cookie_value'" <<'EOF'


def _seeded(cookie_value):
    print("cookie", cookie_value, file=sys.stderr)
EOF
    seed Core/Worker/Runtime/Worker.qml "'authorization'" <<'EOF'
// seeded: console.log(authorization) inside a comment is ignored, the call below is not
function seededLog(request) { console.log("auth", request.authorization) }
EOF
    seed Shared/SailTune/Core/Accounts/Infrastructure/SessionSigner.cpp "'secret'" <<'EOF'
static void seededSecret(const QByteArray &secret) { qCritical("signing with %s", secret.constData()); }
EOF
    echo "secrets: $seeds seeded logging leaks rejected"
else
    echo "secrets: static check SKIPPED (no python3)"
fi

# ---- 2. Qt test
qt=yes
for m in Qt5Core Qt5Network Qt5Sql; do
    pkg-config --exists $m 2>/dev/null || qt=no
done
if [ "$qt" = no ]; then
    echo "secrets: Qt test SKIPPED (no Qt5Core/Qt5Network/Qt5Sql)"
    echo "secrets: PASS"
    exit 0
fi
out="$work/build"
mkdir -p "$out"
moc=$(pkg-config --variable=host_bins Qt5Core 2>/dev/null)/moc
[ -x "$moc" ] || moc=moc
"$moc" -I"$root" "$root/Core/Account/Infrastructure/AccountService.h" -o "$out/moc_service.cpp"
"$moc" -I"$root" "$root/Core/Account/Infrastructure/AuthTransport.h" -o "$out/moc_transport.cpp"
"$moc" -I"$root" "$root/Clients/Sailfish/Presentation/AccountController.h" -o "$out/moc_controller.cpp"
"$moc" -I"$root" "$root/Core/Resolver/Infrastructure/YtDlpRunner.h" -o "$out/moc_runner.cpp"
qtflags=$(pkg-config --cflags Qt5Core Qt5Network Qt5Sql | sed 's/-I/-isystem /g')
"${CXX:-c++}" -std=c++11 -fPIC -Wall -Wextra -I"$root" $qtflags -o "$out/t" \
    "$here/SecretsTest.cpp" \
    "$root/Core/Account/Domain/AccountPolicy.cpp" \
    "$root/Core/Account/Infrastructure/AccountStore.cpp" \
    "$root/Core/Account/Infrastructure/AuthTransport.cpp" \
    "$root/Core/Account/Infrastructure/AccountService.cpp" \
    "$root/Core/Account/Infrastructure/BrowserCookieImport.cpp" \
    "$root/Core/Account/Infrastructure/PinnedSessionParser.cpp" \
    "$root/Shared/SailTune/Core/Accounts/Infrastructure/SessionImportParser.cpp" \
    "$root/Shared/SailTune/Core/Accounts/Infrastructure/Sha1.cpp" \
    "$root/Clients/Sailfish/Presentation/AccountController.cpp" \
    "$root/Core/Resolver/Infrastructure/YtDlpRunner.cpp" \
    "$root/Core/Library/Infrastructure/SailTubeBackup.cpp" \
    "$root/Core/Library/Infrastructure/ZipReader.cpp" \
    "$root/Core/Library/Domain/LibraryPolicy.cpp" \
    "$out/moc_service.cpp" "$out/moc_transport.cpp" "$out/moc_controller.cpp" "$out/moc_runner.cpp" \
    $(pkg-config --libs Qt5Core Qt5Network Qt5Sql) -lz

sec_root="$work/root"
mkdir -p "$sec_root/tmp"
preload=""
if [ "$(uname -s)" = Linux ] && [ "${SEC_NO_TRACE:-0}" != 1 ]; then
    "${CC:-cc}" -shared -fPIC -O1 -Wall -o "$out/fs_trace.so" "$here/fs_trace.c" -ldl
    preload="$out/fs_trace.so"
    : > "$work/trace.log"
fi
rc=0
if [ -n "$preload" ]; then
    SEC_ROOT="$sec_root" TMPDIR="$sec_root/tmp" SEC_TRACE="$work/trace.log" SEC_TRACE_ROOT="$sec_root" \
        LD_PRELOAD="$preload" "$out/t" || rc=$?
else
    SEC_ROOT="$sec_root" TMPDIR="$sec_root/tmp" "$out/t" || rc=$?
fi
if [ "$rc" != 0 ]; then echo "secrets: FAIL"; exit 1; fi
echo "secrets: PASS"
