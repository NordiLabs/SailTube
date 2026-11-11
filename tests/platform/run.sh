#!/bin/sh
# SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
# PLT-03, PLT-04 and the NFR-TST-03 invariants (docs/PLATFORM_CHECKS.md). Needs only python3 (stdlib).
#  1. Every named check of tools/platform_check.py on this tree.
#  2. The forced-sandbox FULL test desktop entry (PLT-03 c), generated on the fly and audited.
#  3. One seeded violation per check in a scratch copy of the needed directories: each must fail
#     the matching named check with the expected message (and a harmless change must still pass).
set -eu
here=$(cd "$(dirname "$0")" && pwd)
root=$(cd "$here/../.." && pwd)
tool="$root/tools/platform_check.py"
scratch=$(mktemp -d "${TMPDIR:-/tmp}/sailtube-platform.XXXXXX")
trap 'rm -rf "$scratch"' EXIT INT TERM
out="$scratch/out.txt"

check() { python3 -I "$tool" "$@"; }

die() { echo "platform: FAIL ($1)"; [ -f "$out" ] && cat "$out"; exit 1; }

# fresh <name>: a scratch tree with only what the checks read; prints its path.
fresh() {
    d="$scratch/$1"
    mkdir -p "$d/tests"
    (cd "$root" && tar -cf - harbour-sailtube.desktop sailtube.pro rpm Core Clients Shared) | tar -xf - -C "$d"
    (cd "$root/tests" && tar -cf - platform) | tar -xf - -C "$d/tests"
    echo "$d"
}

# subst <file> <old> <new>: replace the first exact occurrence ("\n" in <new> is a newline).
subst() {
    python3 -I - "$1" "$2" "$3" <<'PY'
import sys
path, old, new = sys.argv[1:4]
text = open(path, encoding="utf-8").read()
if old not in text:
    sys.exit("subst: %r not found in %s" % (old, path))
open(path, "w", encoding="utf-8").write(text.replace(old, new.replace("\\n", "\n"), 1))
PY
}

# jsonset <file> <python statement on d>: edit a JSON declaration in a scratch tree.
jsonset() {
    python3 -I - "$1" "$2" <<'PY'
import json, sys
path, stmt = sys.argv[1:3]
d = json.load(open(path, encoding="utf-8"))
exec(stmt, {"d": d})
json.dump(d, open(path, "w", encoding="utf-8"), indent=1)
PY
}

# expect_fail <label> <message> <check> <tree> [extra args]: the seeded tree must fail <check> with <message>.
expect_fail() {
    label=$1 msg=$2 name=$3 tree=$4
    shift 4
    if check --root "$tree" "$@" "$name" > "$out" 2>&1; then die "$label: seeded violation was accepted"; fi
    grep -q "platform_check: $name: FAIL" "$out" || die "$label: $name did not report the failure"
    grep -qF -- "$msg" "$out" || die "$label: failed for another reason (expected: $msg)"
    echo "platform: $label: rejected as expected"
}

expect_pass() {
    label=$1 name=$2 tree=$3
    shift 3
    check --root "$tree" "$@" "$name" > "$out" 2>&1 || die "$label: a harmless change failed $name"
    echo "platform: $label: accepted as expected"
}

echo "platform: checks on this tree"
check --root "$root" || { echo "platform: FAIL (this tree)"; exit 1; }

# ---- PLT-03 (c): forced-sandbox FULL test desktop entry --------------------------------------
forced="$scratch/harbour-sailtube.forced-sandbox.desktop"
check --root "$root" --write-forced-sandbox-desktop "$forced"
if grep -qi '^[[:space:]]*Sandboxing[[:space:]]*=[[:space:]]*Disabled' "$forced"; then die "forced-sandbox entry keeps Sandboxing=Disabled"; fi
check --root "$root" --desktop "$forced" sailjail_names permissions > "$out" 2>&1 || die "forced-sandbox entry fails the audit"
echo "platform: forced-sandbox desktop entry generated and audited (device run pending, docs/PLATFORM_CHECKS.md)"
# A FULL entry that disables the sandbox: the generated entry must drop exactly that line.
t=$(fresh forced)
subst "$t/harbour-sailtube.desktop" "[X-Sailjail]" "[X-Sailjail]\nSandboxing=Disabled"
check --root "$t" --write-forced-sandbox-desktop "$t/forced.desktop" > /dev/null
if grep -qi 'Sandboxing=Disabled' "$t/forced.desktop"; then die "Sandboxing=Disabled survived the forced-sandbox generator"; fi
grep -v '^#' "$t/forced.desktop" > "$t/a"; grep -v 'Sandboxing=Disabled' "$t/harbour-sailtube.desktop" > "$t/b"
cmp -s "$t/a" "$t/b" || die "forced-sandbox generator changed more than the Sandboxing line"
expect_fail "STORE entry with Sandboxing=Disabled" "Sailjail is mandatory on STORE" sailjail_names "$t"
echo "platform: forced-sandbox generator drops Sandboxing=Disabled only"

# ---- PLT-03 (a): Sailjail names --------------------------------------------------------------
t=$(fresh names-main)
subst "$t/Clients/Sailfish/Composition/main.cpp" 'QCoreApplication::setOrganizationName(QStringLiteral("org.nordilabs.sailtube"))' \
    'QCoreApplication::setOrganizationName(QStringLiteral("harbour-sailtube"))'
expect_fail "organization name differs in main.cpp" 'expected "org.nordilabs.sailtube"' sailjail_names "$t"
t=$(fresh names-desktop)
subst "$t/harbour-sailtube.desktop" "ApplicationName=SailTube" "ApplicationName=harbour-sailtube"
expect_fail "application name differs in the desktop entry" "ApplicationName=harbour-sailtube, expected SailTube" sailjail_names "$t"
t=$(fresh names-order)
subst "$t/Clients/Sailfish/Composition/main.cpp" '    QCoreApplication::setApplicationName(QStringLiteral("SailTube"));' ''
subst "$t/Clients/Sailfish/Composition/main.cpp" '    app->setApplicationName(QStringLiteral("SailTube"));' \
    '    app->setApplicationName(QStringLiteral("SailTube"));\n    QCoreApplication::setApplicationName(QStringLiteral("SailTube"));'
expect_fail "application name set after the application object" "setApplicationName runs after the application object" sailjail_names "$t"

# ---- PLT-03 (b): paths -------------------------------------------------------------------------
t=$(fresh paths-home)
printf '%s\n' '#include <QDir>' 'QString scratchRoot() { return QDir::homePath() + QStringLiteral("/.sailtube"); }' > "$t/Core/ScratchPaths.cpp"
expect_fail "home directory path" "Core/ScratchPaths.cpp:2: home directory access" paths "$t"
t=$(fresh paths-generic)
printf '%s\n' '#include <QStandardPaths>' 'QString scratchRoot() { return QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation) + QStringLiteral("/sailtube"); }' > "$t/Core/ScratchPaths.cpp"
expect_fail "GenericDataLocation without the app subdirectory" "QStandardPaths::GenericDataLocation without the /org.nordilabs.sailtube/SailTube subdirectory" paths "$t"
t=$(fresh paths-tmp)
printf '%s\n' 'static const char kScratch[] = "/tmp/sailtube-scratch";' > "$t/Core/ScratchPaths.cpp"
expect_fail "hard-coded /tmp path" 'hard-coded path "/tmp/sailtube-scratch"' paths "$t"
t=$(fresh paths-qml)
subst "$t/Clients/Sailfish/qml/pages/AboutPage.qml" "allowedOrientations: Orientation.All" \
    'allowedOrientations: Orientation.All\n    readonly property string scratchFolder: "/home/defaultuser/Videos"'
expect_fail "hard-coded home path in QML" 'hard-coded path "/home/defaultuser/Videos"' paths "$t"
t=$(fresh paths-tied)
printf '%s\n' '#include <QStandardPaths>' 'QString scratchRoot() { return QStandardPaths::writableLocation(QStandardPaths::MoviesLocation); }' > "$t/Core/ScratchPaths.cpp"
expect_fail "Videos location outside the Videos feature code" "is a Videos location but this file is not in that permission's code list" paths "$t"
t=$(fresh paths-redirect)
subst "$t/Clients/Sailfish/Composition/main.cpp" 'qputenv("TMPDIR"' 'qputenv("SCRATCH_UNUSED"'
expect_fail "QDir::tempPath without the TMPDIR redirect" "QDir::tempPath( while main.cpp does not set TMPDIR" paths "$t"
t=$(fresh paths-stale)
jsonset "$t/tests/platform/path_exceptions.json" 'd["exceptions"].append({"id": "scratch-stale", "file": "Core/Nowhere.cpp", "rule": "home", "match": "x", "reason": "seed"})'
expect_fail "stale path exception" "stale exception scratch-stale" paths "$t"
t=$(fresh paths-comment)
printf '%s\n' '// Never QDir::homePath() or "/tmp/x" here; see PLT-03.' '/* QStandardPaths::HomeLocation */' 'static const char kPath[] = "/library/library.db";' > "$t/Core/ScratchPaths.cpp"
expect_pass "paths in comments and relative suffixes" paths "$t"

# ---- PLT-04 / NFR-TST-03 (4): every permission maps to a feature -------------------------------
t=$(fresh perm-nofeature)
subst "$t/harbour-sailtube.desktop" "Permissions=Internet;Audio" "Permissions=Internet;Audio;Secrets"
expect_fail "declared permission with no feature (Secrets)" "declares Secrets but the mapping says declared:false" permissions "$t"
t=$(fresh perm-outside)
subst "$t/harbour-sailtube.desktop" "Permissions=Internet;Audio" "Permissions=Internet;Audio;Bluetooth"
expect_fail "permission outside PLT-04" "declares Bluetooth, outside the PLT-04 list" permissions "$t"
t=$(fresh perm-missing)
subst "$t/harbour-sailtube.desktop" "Permissions=Internet;Audio" "Permissions=Internet"
expect_fail "mapped permission not declared" "mapping marks Audio declared but harbour-sailtube.desktop does not declare it" permissions "$t"
t=$(fresh perm-code)
jsonset "$t/tests/platform/permissions.json" 'd["permissions"]["Audio"]["code"] = ["Clients/Sailfish/qml/pages/NoSuchPlayer.qml"]'
expect_fail "permission whose feature code is gone" "Audio: code glob Clients/Sailfish/qml/pages/NoSuchPlayer.qml matches no file" permissions "$t"
t=$(fresh perm-planned)
jsonset "$t/tests/platform/permissions.json" 'd["permissions"]["Secrets"]["declared"] = True'
subst "$t/harbour-sailtube.desktop" "Permissions=Internet;Audio" "Permissions=Internet;Audio;Secrets"
expect_fail "permission tied to a planned feature" "Secrets is declared but its feature flag account.secretsVault is only planned" permissions "$t"
# Seeds work on either tree shape: with or without D-Bus link activation on main.
strip_lines() {  # strip_lines <file> <substring>: drop every line containing it
    python3 -I - "$1" "$2" <<'PY'
import sys
path, needle = sys.argv[1:3]
lines = open(path, encoding="utf-8").read().splitlines(True)
open(path, "w", encoding="utf-8").write("".join(l for l in lines if needle not in l))
PY
}
t=$(fresh perm-execdbus)
rm -rf "$t/dbus"; strip_lines "$t/harbour-sailtube.desktop" "X-Maemo-"; strip_lines "$t/rpm/harbour-sailtube.spec" "dbus-1/services"
strip_lines "$t/harbour-sailtube.desktop" "ExecDBus="
subst "$t/harbour-sailtube.desktop" "[X-Sailjail]" "[X-Sailjail]\nExecDBus=/usr/bin/harbour-sailtube"
expect_fail "ExecDBus without a D-Bus service" "has ExecDBus but nothing is D-Bus activated" permissions "$t"
t=$(fresh perm-dbus)
strip_lines "$t/harbour-sailtube.desktop" "ExecDBus="
printf '%s\n' '[D-BUS Service]' 'Name=org.nordilabs.sailtube.SailTube' 'Exec=/usr/bin/harbour-sailtube' > "$t/org.nordilabs.sailtube.SailTube.service"
expect_fail "D-Bus service without ExecDBus" "without ExecDBus in [X-Sailjail]" permissions "$t"

# ---- NFR-TST-03 (1): the cache key covers every output-affecting setting ------------------------
seed_setting() {  # seed_setting <tree> <key>: a new setting in the inventory (shared table if present)
    if [ -f "$1/Core/Settings/Domain/SettingsDefaults.cpp" ]; then
        subst "$1/Core/Settings/Domain/SettingsDefaults.cpp" "const std::vector<SettingSpec> table = {" \
            "const std::vector<SettingSpec> table = {\n    { \"$2\", SettingType::Bool, \"false\", 0, 0, 0, \"\", Variant::All },"
    else
        printf '%s\n' "bool AppSettings::scratchSeed() const { return settings_.value(QStringLiteral(\"$2\"), false).toBool(); }" \
            >> "$1/Clients/Sailfish/Presentation/AppSettings.cpp"
    fi
}
# The seeded key must never be a real setting (playback/stableVolume became one on 2026-11-10).
t=$(fresh key-unclassified)
seed_setting "$t" "playback/seededProbe"
expect_fail "new setting not classified" "playback/seededProbe" cache_key "$t"
grep -qF "is not classified in output_settings.json" "$out" || die "unclassified setting: unexpected message"
t=$(fresh key-outside)
seed_setting "$t" "playback/seededProbe"
subst "$t/Shared/Playback/StreamSource.h" "    bool hardwareDecode = false;" "    bool hardwareDecode = false;\n    bool stableVolume = false;"
jsonset "$t/tests/platform/output_settings.json" 'd["settings"]["playback/seededProbe"] = {"output": True, "request_field": "stableVolume"}; d["request"]["fields"]["stableVolume"] = {"role": "output"}'
expect_fail "new output-affecting setting outside the key" "output field StreamRequest::stableVolume (setting playback/seededProbe) is not in StreamRequest::cacheKey()" cache_key "$t"
t=$(fresh key-component)
subst "$t/Shared/SailTune/Core/SharedPlayback/Domain/StreamTypes.h" "    bool hardwareDecode = false;" "    bool hardwareDecode = false;\n    bool stableVolume = false;"
expect_fail "shared request field missing from cacheKey()" "StreamRequest::stableVolume is not part of cacheKey()" cache_key "$t"
t=$(fresh key-contract)
subst "$t/Shared/Playback/StreamSource.h" "    bool speculative = false;" "    bool speculative = false;\n    int maxFrameRate = 0;"
expect_fail "unclassified StreamRequest field" "StreamRequest::maxFrameRate (Shared/Playback/StreamSource.h) is not classified" cache_key "$t"

# ---- NFR-TST-03 (2): UI default literals match the defaults table -------------------------------
t=$(fresh ui-literal)
subst "$t/Clients/Sailfish/qml/pages/SettingsPage.qml" "allowedOrientations: Orientation.All" \
    'allowedOrientations: Orientation.All\n    readonly property int seededCeiling: appSettings.value("playback/maxHeight", 12345)'
expect_fail "changed UI default literal" "playback/maxHeight: UI literal 12345, table" ui_defaults "$t"
t=$(fresh ui-unknown)
subst "$t/Clients/Sailfish/qml/pages/SettingsPage.qml" "allowedOrientations: Orientation.All" \
    'allowedOrientations: Orientation.All\n    readonly property bool seeded: appSettings.value("playback/noSuchKey", true)'
expect_fail "UI default for a key outside the table" "playback/noSuchKey, which is not in the defaults table" ui_defaults "$t"
if [ -f "$root/Clients/Sailfish/qml/logic/SettingsInventory.js" ]; then
    t=$(fresh ui-inventory)
    subst "$t/Clients/Sailfish/qml/logic/SettingsInventory.js" "def: 500" "def: 600"
    expect_fail "changed SettingsInventory.js default" "history/cap: UI literal 600" ui_defaults "$t"
fi

# ---- NFR-TST-03 (3): every SponsorBlock category the engine fetches has a UI row ----------------
# Scratch categories that no waiver (tests/platform/pending.json) names, so the seeds hold on any tree.
t=$(fresh sb-missing)
mkdir -p "$t/Core/Catalog/Domain"
printf '%s\n' '#include <string>' '#include <vector>' \
    'const std::vector<std::string> &sponsorScratchCategories() {' \
    '    static const std::vector<std::string> list = { "scratch_one", "scratch_two" };' \
    '    return list;' '}' > "$t/Core/Catalog/Domain/ScratchSponsor.cpp"
printf '%s\n' 'import QtQuick 2.6' '// SponsorBlock categories' 'ListModel {' '    id: sponsorCategoryRows' '    ListElement { category: "scratch_one" }' '}' \
    > "$t/Clients/Sailfish/qml/pages/ScratchSponsorBlockPage.qml"
expect_fail "SponsorBlock category with no UI row" "SponsorBlock category scratch_two is fetched by the engine" sponsorblock "$t"
grep -q "category scratch_one is fetched" "$out" && die "scratch_one has a UI row but was reported"
printf '%s\n' 'import QtQuick 2.6' '// SponsorBlock categories' 'ListModel {' '    id: sponsorCategoryRows' '    ListElement { category: "scratch_one" }' \
    '    ListElement { category: "scratch_two" }' '}' > "$t/Clients/Sailfish/qml/pages/ScratchSponsorBlockPage.qml"
expect_pass "SponsorBlock categories all with UI rows" sponsorblock "$t"
if [ -f "$root/tests/platform/pending.json" ] && grep -q '"sponsor"' "$root/tests/platform/pending.json"; then
    t=$(fresh sb-stale-waiver)
    printf '%s\n' 'import QtQuick 2.6' '// SponsorBlock categories' 'ListModel {' '    ListElement { category: "sponsor" }' '}' \
        > "$t/Clients/Sailfish/qml/pages/ScratchSponsorBlockPage.qml"
    expect_fail "waiver kept after the UI row landed" "waives sponsor, which has a UI row now" sponsorblock "$t"
fi

echo "platform: PASS"
