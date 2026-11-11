#!/bin/bash
# SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
# Harbour gate (G6): run the SDK's rpmvalidation on an RPM, plus SailTube's own static STORE
# checks (PLT-11: no Nemo.KeepAlive import or requirement).
#
#   tools/harbour_validate.sh [RPM]      (default: RPMS/store/harbour-sailtube-*.rpm)
#
# rpmvalidation runs in the official SDK container (SFOS_OFFICIAL_CONTAINER, default
# sailfish-mac-probe-20261106) in a private directory; nothing else in it is touched.
# Exit: 0 all checks pass, 1 a check failed, 2 infrastructure error.
set -u
DOCKER_SOCK="${DOCKER_SOCK:-unix://$HOME/.orbstack/run/docker.sock}"
OFF="${SFOS_OFFICIAL_CONTAINER:-sailfish-mac-probe-20261106}"
TARGET="${SFOS_VALIDATION_TARGET:-SailfishOS-5.1.0.11-aarch64}"
dk() { docker -H "$DOCKER_SOCK" "$@"; }
WT=$(cd "$(dirname "$0")/.." && pwd)
RPM="${1:-$(ls "$WT"/RPMS/store/harbour-sailtube-*.rpm 2>/dev/null | head -1)}"
[ -s "$RPM" ] || { echo "no RPM (build one with tools/rpm_build.sh --store)" >&2; exit 2; }
NAME=$(basename "$RPM")
VAL=/home/sdk/sailtube-val
TMP=$(mktemp -d "${TMPDIR:-/tmp}/sailtube_val.XXXXXX"); trap 'rm -rf "$TMP"' EXIT

dk exec -u root "$OFF" sh -c "rm -rf $VAL && mkdir -p $VAL && chown sdk $VAL" || exit 2
dk cp "$RPM" "$OFF:$VAL/$NAME" || exit 2
dk exec -u root "$OFF" chown sdk "$VAL/$NAME" || exit 2
dk exec -u sdk "$OFF" sh -c "cd $VAL && rpmvalidation -t $TARGET $NAME 2>&1" | sed 's/\x1b\[[0-9;]*m//g' > "$TMP/validator.txt"
passed=$(grep -c PASSED "$TMP/validator.txt"); failed=$(grep -c FAILED "$TMP/validator.txt")
rc=0
echo "rpmvalidation ($TARGET): $passed PASSED, $failed FAILED"
if [ "$failed" != 0 ] || [ "$passed" = 0 ]; then
  grep -E "ERROR|FAILED" "$TMP/validator.txt" | sed 's/^/  /'; rc=1
fi

# Static STORE checks: requirements and shipped QML.
dk exec -u sdk "$OFF" sh -c "cd $VAL && rpm -qp --requires $NAME" > "$TMP/requires.txt" 2>/dev/null || exit 2
dk exec -u sdk "$OFF" sh -c "cd $VAL && rm -rf x && mkdir x && cd x && rpm2cpio ../$NAME | cpio -idm --quiet && grep -rl 'Nemo.KeepAlive' . ; true" > "$TMP/keepalive.txt"
if grep -q 'Nemo.KeepAlive' "$TMP/requires.txt"; then echo "STORE check: requires qml(Nemo.KeepAlive)"; rc=1; fi
if [ -s "$TMP/keepalive.txt" ]; then echo "STORE check: Nemo.KeepAlive in"; sed 's/^/  /' "$TMP/keepalive.txt"; rc=1; fi
[ "$rc" = 0 ] && echo "STORE checks: no Nemo.KeepAlive requirement or import"
echo "RESULT: $([ "$rc" = 0 ] && echo OK || echo FAILED)"
exit $rc
