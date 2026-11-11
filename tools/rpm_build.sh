#!/bin/bash
# SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
# Packaging check: rpmbuild in the native aarch64 container with the arguments the shared
# parity tool uses for SailTune (sailtune-internal-notes/tools/parity_check.sh).
#
#   tools/rpm_build.sh [--store | --beta] [--debug-hooks] [--without-runtime] [--release] [--jobs N]
#
# --store builds the STORE (Harbour) variant into RPMS/store/ (spec `--with store`).
# --beta builds the time-limited public beta into RPMS/beta/ (spec `--with beta`, FULL only, docs/BETA.md);
# --debug-hooks adds the device-only test hooks (spec `--with debug_hooks`).
# Every RPM is then checked by tools/rpm_audit.py in the container (PLT-02 glibc ceiling of the
# 5.1 target, NFR-MNT-02 component manifest, NFR-SEC-06 licences and helper ban); --release makes
# an incomplete licence review fatal. A failed audit exits 1 (the RPM is still copied for inspection).
#
# Snapshots what is on disk (HEAD + tracked changes + untracked, not ignored), adds the prepared
# Python-archive runtime stage (vendor/python-runtime, same layout as SailTune's; path inside the
# container from SAILTUBE_PYTHON_RUNTIME), builds, and copies the RPM to RPMS/. Not a delivery:
# mb2 stays authoritative (docs/BUILDING.md). Exit: 0 ok, 1 build failed, 2 infrastructure error.
set -u
DOCKER_SOCK="${DOCKER_SOCK:-unix://$HOME/.orbstack/run/docker.sock}"
CONTAINER="${SFOS_NATIVE_CONTAINER:-sfos-m1-native}"
RUNTIME="${SAILTUBE_PYTHON_RUNTIME:-/home/sdk/parity-real-std2/vendor/python-runtime}"
dk() { docker -H "$DOCKER_SOCK" "$@"; }
WITH_RUNTIME=1; JOBS=8; STORE=0; BETA=0; HOOKS=0; RELEASE=
while [ $# -gt 0 ]; do
  case "$1" in
    --store) STORE=1; shift;;
    --beta) BETA=1; shift;;
    --debug-hooks) HOOKS=1; shift;;
    --without-runtime) WITH_RUNTIME=0; shift;;
    --release) RELEASE=--release; shift;;
    --jobs) JOBS="$2"; shift 2;;
    *) echo "unknown option $1" >&2; exit 2;;
  esac
done
WT=$(cd "$(dirname "$0")/.." && git rev-parse --show-toplevel) || exit 2
SHA=$(git -C "$WT" rev-parse --short HEAD) || exit 2
[ "$STORE" = 1 ] && [ "$BETA" = 1 ] && { echo "--beta is FULL only (not for Harbour)" >&2; exit 2; }
DIR="/builds/sailtube-rpm-$SHA"; [ "$STORE" = 1 ] && DIR="$DIR-store"; [ "$BETA" = 1 ] && DIR="$DIR-beta"; [ "$HOOKS" = 1 ] && DIR="$DIR-hooks"
TMP=$(mktemp -d "${TMPDIR:-/tmp}/sailtube_rpm.XXXXXX"); trap 'rm -rf "$TMP"' EXIT
mkdir -p "$TMP/stage"
git -C "$WT" archive --format=tar HEAD | tar -x -C "$TMP/stage" || exit 2
while IFS= read -r -d '' f; do
  if [ -e "$WT/$f" ]; then mkdir -p "$TMP/stage/$(dirname "$f")"; cp -p "$WT/$f" "$TMP/stage/$f"; else rm -f "$TMP/stage/$f"; fi
done < <({ git -C "$WT" diff --name-only -z --no-renames HEAD --; git -C "$WT" ls-files -z -o --exclude-standard; } | sort -zu)
find "$TMP/stage" -name '._*' -type f -delete
COPYFILE_DISABLE=1 tar --no-xattrs --no-mac-metadata -C "$TMP/stage" -cf "$TMP/snapshot.tar" . 2>/dev/null \
  || COPYFILE_DISABLE=1 tar -C "$TMP/stage" -cf "$TMP/snapshot.tar" . || exit 2

dk exec "$CONTAINER" sh -c "rm -rf $DIR $DIR.log && mkdir -p $DIR && chown 1000:100000 $DIR" || exit 2
dk exec -i -u sdk "$CONTAINER" tar -x -C "$DIR" < "$TMP/snapshot.tar" || exit 2
OPTS=""
if [ "$WITH_RUNTIME" = 1 ]; then
  dk exec -u sdk "$CONTAINER" sh -c "test -s '$RUNTIME/SHA256SUMS' && mkdir -p $DIR/vendor && cp -a '$RUNTIME' $DIR/vendor/python-runtime" \
    || { echo "python runtime stage not found in the container: $RUNTIME" >&2; exit 2; }
else
  OPTS="--without python_runtime"
fi
[ "$STORE" = 1 ] && OPTS="$OPTS --with store"
[ "$BETA" = 1 ] && OPTS="$OPTS --with beta"
[ "$HOOKS" = 1 ] && OPTS="$OPTS --with debug_hooks"
dk exec -i -u sdk "$CONTAINER" sh <<EOF
cd $DIR
rpmbuild --build-in-place --target=aarch64-meego-linux-gnu --define "debug_package %{nil}" --define "_smp_mflags -j$JOBS" \
  --define "_rpmdir \$PWD/RPMS" --define "_sourcedir \$PWD/rpm" --define "_rpmfilename %%{name}-%%{version}-%%{release}.%%{arch}.rpm" \
  --define "__patch patch" --define "_buildhost localhost" --buildroot="\$PWD/installroot" -bb rpm/harbour-sailtube.spec $OPTS > $DIR.log 2>&1
echo EXIT \$? >> $DIR.log
EOF
if ! dk exec "$CONTAINER" grep -q '^EXIT 0' "$DIR.log"; then
  echo "rpmbuild FAILED ($DIR.log):"; dk exec "$CONTAINER" tail -30 "$DIR.log"; exit 1
fi
RPM=$(dk exec "$CONTAINER" sh -c "ls $DIR/RPMS/*.rpm" | head -1)
OUT="$WT/RPMS"; [ "$STORE" = 1 ] && OUT="$WT/RPMS/store"; [ "$BETA" = 1 ] && OUT="$WT/RPMS/beta"
mkdir -p "$OUT"
dk cp "$CONTAINER:$RPM" "$OUT/" || exit 2
echo "rpm: $OUT/$(basename "$RPM")"
dk exec "$CONTAINER" sh -c "rpm -qp --requires '$RPM' | sort -u | tr '\n' ' '; echo; rpm -qpl '$RPM'" | sed 's/^/  /'
# PLT-02 / NFR-MNT-02 / NFR-SEC-06 gates on the built RPM, then the floor file against this target.
dk exec "$CONTAINER" sh -c "python3 -I $DIR/tools/rpm_audit.py --rpm '$RPM' $RELEASE && \
  python3 -I $DIR/tools/rpm_audit.py --probe-floor /lib64/libc.so.6 /lib64/libm.so.6 \$(readlink -f /usr/lib64/libstdc++.so.6)" \
  | sed 's/^/  /' || { echo "rpm audit FAILED"; exit 1; }
