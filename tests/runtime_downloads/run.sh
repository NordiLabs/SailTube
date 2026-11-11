#!/bin/sh
# SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
# NFR-SEC-04: no runtime download/install/execution of helper code (python3 only).
#  1. tools/check_runtime_downloads.py on this tree, FULL and STORE: must pass with
#     only the reasoned exceptions in exceptions.json, none stale.
#  2. Scratch copies with one seeded violation each: every one must fail with the
#     expected rule and file; a few look-alikes that are fine must pass.
#  3. Exception hygiene: a stale exception fails; a FULL-only exception passes FULL
#     and fails STORE.
# The FULL fake-release-host tests of NFR-SEC-04 are tests/extractor_update (NFR-MNT-01)
# (see docs/SECURITY_CHECKS.md).
set -eu
here=$(cd "$(dirname "$0")" && pwd)
root=$(cd "$here/../.." && pwd)
check="$root/tools/check_runtime_downloads.py"
exceptions="$here/exceptions.json"
if ! command -v python3 >/dev/null 2>&1; then echo "runtime_downloads: SKIPPED (no python3)"; exit 0; fi

python3 -I "$check" --root "$root" --exceptions "$exceptions" --variant full
python3 -I "$check" --root "$root" --exceptions "$exceptions" --variant store

scratch=$(mktemp -d "${TMPDIR:-/tmp}/sailtube-runtime-downloads.XXXXXX")
trap 'rm -rf "$scratch"' EXIT
tree="$scratch/tree"
mkdir -p "$tree"
(cd "$root" && tar -cf - --exclude='tests' Core Clients Shared rpm) | tar -xf - -C "$tree"
python3 -I "$check" --root "$tree" --exceptions "$exceptions" > "$scratch/out.txt" 2>&1 \
    || { echo "runtime_downloads: FAIL (the scratch copy does not pass unchanged)"; cat "$scratch/out.txt"; exit 1; }

seeds=0
# expect_fail RULE FILE: the seeded tree must fail and name RULE in FILE.
expect_fail() {
    seeds=$((seeds + 1))
    if python3 -I "$check" --root "$tree" --exceptions "${3:-$exceptions}" ${4:-} > "$scratch/out.txt" 2>&1; then
        echo "runtime_downloads: FAIL (seed $seeds, $1 in $2, was accepted)"; exit 1
    fi
    grep -q "^$2:[0-9]*: $1:" "$scratch/out.txt" \
        || { echo "runtime_downloads: FAIL (seed $seeds: expected $1 in $2)"; cat "$scratch/out.txt"; exit 1; }
    echo "runtime_downloads: seed $seeds rejected as expected ($1, $2)"
}
expect_pass() {
    python3 -I "$check" --root "$tree" --exceptions "${2:-$exceptions}" ${3:-} > "$scratch/out.txt" 2>&1 \
        || { echo "runtime_downloads: FAIL (look-alike '$1' was rejected)"; cat "$scratch/out.txt"; exit 1; }
    echo "runtime_downloads: look-alike accepted as expected ($1)"
}
seed() { mkdir -p "$(dirname "$tree/$1")"; cat > "$tree/$1"; }
unseed() { rm -f "$tree/$1"; }

# --- downloaded body written to a .pyz (C++)
seed Core/Scratch/Update.cpp <<'EOF'
#include <QNetworkReply>
#include <QSaveFile>
void seededUpdate(QNetworkReply *reply, const QString &dir) {
    QSaveFile out(dir + QStringLiteral("/yt-dlp.pyz"));
    out.open(QIODevice::WriteOnly);
    out.write(reply->readAll());
    out.commit();
}
EOF
expect_fail RD-CODE-WRITE Core/Scratch/Update.cpp; unseed Core/Scratch/Update.cpp

# --- a shared object staged through a variable, then renamed into place
seed Core/Scratch/Stage.cpp <<'EOF'
#include <QFile>
void seededStage(const QString &cache, const QByteArray &body) {
    const QString staged = cache + QStringLiteral("/libhelper.so.1");
    QFile f;
    f.setFileName(staged);
    f.open(QIODevice::WriteOnly);
    f.write(body);
}
EOF
expect_fail RD-CODE-WRITE Core/Scratch/Stage.cpp; unseed Core/Scratch/Stage.cpp

# --- chmod +x on a file (C++ and Python)
seed Core/Scratch/Mode.cpp <<'EOF'
#include <QFile>
void seededMode(const QString &helper) {
    QFile::setPermissions(helper, QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner);
}
EOF
expect_fail RD-EXEC-MODE Core/Scratch/Mode.cpp; unseed Core/Scratch/Mode.cpp
seed Shared/Scratch/mode.py <<'EOF'
import os
def seeded(target):
    os.chmod(target, 0o755)
EOF
expect_fail RD-EXEC-MODE Shared/Scratch/mode.py; unseed Shared/Scratch/mode.py

# --- running a program from a writable location (C++ and Python)
seed Core/Scratch/Run.cpp <<'EOF'
#include <QProcess>
#include <QStandardPaths>
void seededRun() {
    QProcess p;
    p.start(QStandardPaths::writableLocation(QStandardPaths::CacheLocation) + QStringLiteral("/yt-dlp"), QStringList());
}
EOF
expect_fail RD-PROC Core/Scratch/Run.cpp; unseed Core/Scratch/Run.cpp
seed Core/Scratch/run.py <<'EOF'
import os
import subprocess
def seeded():
    helper = os.path.expanduser("~/.cache/harbour-sailtube/yt-dlp")
    subprocess.run([helper, "--version"])
EOF
expect_fail RD-PROC Core/Scratch/run.py; unseed Core/Scratch/run.py

# --- Python code from a writable location
seed Core/Scratch/path.py <<'EOF'
import os
import sys
def seeded():
    sys.path.insert(0, os.path.join(os.environ["HOME"], ".cache", "harbour-sailtube", "yt-dlp.pyz"))
EOF
expect_fail RD-LOADPATH Core/Scratch/path.py; unseed Core/Scratch/path.py

# --- a library from a writable location
seed Clients/Scratch/Load.cpp <<'EOF'
#include <QDir>
#include <QLibrary>
void seededLoad() {
    QLibrary lib(QDir::homePath() + QStringLiteral("/.local/share/harbour-sailtube/libplugin.so"));
    lib.load();
}
EOF
expect_fail RD-DLOPEN Clients/Scratch/Load.cpp; unseed Clients/Scratch/Load.cpp

# --- PYTHONPATH pointed at the data dir
seed Core/Scratch/Env.cpp <<'EOF'
#include <QFile>
#include <QStandardPaths>
void seededEnv() {
    qputenv("PYTHONPATH", QFile::encodeName(QStandardPaths::writableLocation(QStandardPaths::AppDataLocation)));
}
EOF
expect_fail RD-LOADPATH Core/Scratch/Env.cpp; unseed Core/Scratch/Env.cpp

# --- a GitHub release URL and yt-dlp's own updater / remote components
seed Core/Scratch/Url.cpp <<'EOF'
static const char kLatest[] = "https://github.com/yt-dlp/yt-dlp/releases/latest/download/yt-dlp";
EOF
expect_fail RD-URL Core/Scratch/Url.cpp; unseed Core/Scratch/Url.cpp
seed Core/Scratch/Flags.cpp <<'EOF'
#include <QStringList>
QStringList seededFlags() { return QStringList() << QStringLiteral("--update-to") << QStringLiteral("nightly"); }
QStringList seededEjs() { return QStringList() << QStringLiteral("--remote-components") << QStringLiteral("ejs:github"); }
EOF
expect_fail RD-FLAG Core/Scratch/Flags.cpp; unseed Core/Scratch/Flags.cpp

# --- the runner loses its guard flag
cp "$tree/Core/Resolver/Infrastructure/YtDlpRunner.cpp" "$scratch/runner.orig"
sed 's/<< QStringLiteral("--no-remote-components")//' "$scratch/runner.orig" > "$tree/Core/Resolver/Infrastructure/YtDlpRunner.cpp"
expect_fail RD-GUARD Core/Resolver/Infrastructure/YtDlpRunner.cpp
cp "$scratch/runner.orig" "$tree/Core/Resolver/Infrastructure/YtDlpRunner.cpp"

# --- archive extraction
seed Shared/Scratch/unpack.py <<'EOF'
import zipfile
def seeded(archive, target):
    with zipfile.ZipFile(archive) as z:
        z.extractall(target)
EOF
expect_fail RD-EXTRACT Shared/Scratch/unpack.py; unseed Shared/Scratch/unpack.py

# --- QML loading code from the network / evaluating text
seed Clients/Scratch/Remote.qml <<'EOF'
import QtQuick 2.0
Item {
    Component.onCompleted: Qt.createComponent("https://updates.example.org/Update.qml")
}
EOF
expect_fail RD-QMLREMOTE Clients/Scratch/Remote.qml; unseed Clients/Scratch/Remote.qml
seed Clients/Scratch/eval.js <<'EOF'
function seeded(text) { return eval(text) }
EOF
expect_fail RD-QMLREMOTE Clients/Scratch/eval.js; unseed Clients/Scratch/eval.js

# --- look-alikes that are fine: a URL in a comment, a 0700 directory, a .db temp copy
seed Core/Scratch/Fine.cpp <<'EOF'
// Upstream notes: https://github.com/yt-dlp/yt-dlp/releases (comment only)
#include <QDir>
#include <QFile>
#include <QSaveFile>
void fine(const QString &cacheDir, const QByteArray &data) {
    QDir().mkpath(cacheDir);
    QFile::setPermissions(cacheDir, QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner);
    QSaveFile f(cacheDir + QStringLiteral("/import.db"));
    f.open(QIODevice::WriteOnly);
    f.write(data);
    f.commit();
}
EOF
expect_pass "comment URL, 0700 directory, data file"; unseed Core/Scratch/Fine.cpp

# --- exception hygiene
python3 -I - "$exceptions" "$scratch/stale.json" <<'PY'
import json, sys
d = json.load(open(sys.argv[1]))
d["exceptions"].append({"file": "Core/Nowhere.cpp", "rule": "RD-PROC", "match": "nothing", "reason": "stale on purpose"})
json.dump(d, open(sys.argv[2], "w"))
PY
if python3 -I "$check" --root "$tree" --exceptions "$scratch/stale.json" > "$scratch/out.txt" 2>&1; then
    echo "runtime_downloads: FAIL (a stale exception was accepted)"; exit 1
fi
grep -q "stale exception" "$scratch/out.txt" || { echo "runtime_downloads: FAIL (no stale report)"; cat "$scratch/out.txt"; exit 1; }
echo "runtime_downloads: stale exception rejected as expected"

seed Core/Scratch/Updater.cpp <<'EOF'
static const char kAssets[] = "https://api.github.com/repos/yt-dlp/yt-dlp/releases/latest";
EOF
python3 -I - "$exceptions" "$scratch/full.json" <<'PY'
import json, sys
d = json.load(open(sys.argv[1]))
d["exceptions"].append({"file": "Core/Scratch/Updater.cpp", "rule": "RD-URL", "match": "kAssets", "variant": "full",
                        "reason": "scratch: a FULL-only updater (NFR-MNT-01) declared for FULL"})
json.dump(d, open(sys.argv[2], "w"))
PY
expect_pass "FULL-only exception on FULL" "$scratch/full.json" "--variant full"
expect_fail RD-URL Core/Scratch/Updater.cpp "$scratch/full.json" "--variant store"
unseed Core/Scratch/Updater.cpp

echo "runtime_downloads: $seeds seeded violations rejected"
echo "runtime_downloads: PASS"
