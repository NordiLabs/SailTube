#!/bin/sh
# SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
# RPM content gates on recorded `rpm -qp --requires/--dump` output of real FULL and STORE RPMs
# (fixtures/): PLT-02 glibc ceiling, NFR-MNT-02 component manifest,
# NFR-SEC-06 licence review and helper ban. Each check also fails on a seeded violation.
# The same tool runs on every freshly built RPM in tools/rpm_build.sh. Needs only python3.
set -eu
here=$(cd "$(dirname "$0")" && pwd)
root=$(cd "$here/../.." && pwd)
tool="$root/tools/rpm_audit.py"
fx="$here/fixtures"
scratch=$(mktemp -d "${TMPDIR:-/tmp}/sailtube-rpm-audit.XXXXXX")
trap 'rm -rf "$scratch"' EXIT
pass() { python3 -I "$tool" "$@" > "$scratch/out" 2>&1 || { cat "$scratch/out"; echo "rpm_audit: FAIL (expected pass: $label)"; exit 1; }; }
fail() { if python3 -I "$tool" "$@" > "$scratch/out" 2>&1; then cat "$scratch/out"; echo "rpm_audit: FAIL (expected failure: $label)"; exit 1; fi
         grep -q "$expect" "$scratch/out" || { cat "$scratch/out"; echo "rpm_audit: FAIL (no '$expect': $label)"; exit 1; }; echo "  rejected: $label"; }

label="real FULL RPM"; pass --requires "$fx/full.requires" --dump "$fx/full.dump"
label="real STORE RPM"; pass --requires "$fx/store.requires" --dump "$fx/store.dump"

# PLT-02: a deliberately newer target (a requirement above the ceiling), and a lower floor.
cp "$fx/full.requires" "$scratch/newer.requires"; echo 'libc.so.6(GLIBC_2.42)(64bit)' >> "$scratch/newer.requires"
label="GLIBC_2.42 above the 5.1 ceiling"; expect="GLIBC_2.42"; fail --requires "$scratch/newer.requires" --dump "$fx/full.dump"
cp "$fx/full.requires" "$scratch/cxx.requires"; echo 'libstdc++.so.6(GLIBCXX_3.4.33)(64bit)' >> "$scratch/cxx.requires"
label="GLIBCXX_3.4.33 above the ceiling"; expect="GLIBCXX_3.4.33"; fail --requires "$scratch/cxx.requires" --dump "$fx/full.dump"
python3 -I -c 'import json,sys; d=json.load(open(sys.argv[1])); d["ceilings"]["GLIBC"]="2.34"; json.dump(d,open(sys.argv[2],"w"))' \
    "$root/packaging/platform_floor.json" "$scratch/floor.json"
label="build on a newer target than a 2.34 floor"; expect="GLIBC_2.38"; fail --requires "$fx/full.requires" --dump "$fx/full.dump" --floor "$scratch/floor.json"

# NFR-MNT-02: changed bytes, an unlisted shipped file, a listed file missing from the RPM.
sed 's#^\(/usr/share/harbour-sailtube/runtime/yt-dlp.pyz [0-9]* [0-9]* \)e51e#\1ffff#' "$fx/full.dump" > "$scratch/hash.dump"
label="yt-dlp.pyz bytes differ from the manifest"; expect="yt-dlp.pyz"; fail --requires "$fx/full.requires" --dump "$scratch/hash.dump"
cp "$fx/full.dump" "$scratch/extra.dump"
echo '/usr/share/harbour-sailtube/lib/libextra.so.1 1000 1700000000 aaaa 0100644 root root 0 0 0 X' >> "$scratch/extra.dump"
label="unlisted bundled library"; expect="libextra.so.1"; fail --requires "$fx/full.requires" --dump "$scratch/extra.dump"
grep -v 'libsailtune-quickjs' "$fx/full.dump" > "$scratch/missing.dump"
label="manifest component missing from the RPM"; expect="not in the RPM"; fail --requires "$fx/full.requires" --dump "$scratch/missing.dump"

# NFR-SEC-06: a component without a licence; incomplete review fails a release build; helpers.
python3 -I -c 'import json,sys; d=json.load(open(sys.argv[1])); d["components"][0]["licence"]=""; json.dump(d,open(sys.argv[2],"w"))' \
    "$root/packaging/components.json" "$scratch/nolicence.json"
label="component without a licence"; expect="has no licence"; fail --requires "$fx/full.requires" --dump "$fx/full.dump" --components "$scratch/nolicence.json"
label="incomplete review on a release build"; expect="review is not complete"; fail --requires "$fx/full.requires" --dump "$fx/full.dump" --release
cp "$fx/full.dump" "$scratch/helper.dump"
echo '/usr/share/harbour-sailtube/bin/ffmpeg 1000 1700000000 bbbb 0100755 root root 0 0 0 X' >> "$scratch/helper.dump"
label="ffmpeg inside the RPM"; expect="runtime helper"; fail --requires "$fx/full.requires" --dump "$scratch/helper.dump"
echo "rpm_audit: PASS"
