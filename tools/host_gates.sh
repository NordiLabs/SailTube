#!/bin/sh
# SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
# Gates that need git history or the SailTune checkout, so they cannot run in the build container's
# .git-less snapshot. Run on the host before a merge and in the release job.
#
#   tools/host_gates.sh [--base REF] [--sailtune DIR] [--tag TAG]
#
#   --base REF     coverage baseline may only shrink relative to REF (default main)
#   --sailtune DIR check every Shared/ pin against its recorded SailTune commit (default: $SAILTUNE_DIR,
#                  else skipped)
#   --tag TAG      release job: TAG matches the spec version and is on main (NFR-MNT-03)
# Exit: 0 all gates pass, 1 a gate failed, 2 usage error.
set -u
root=$(cd "$(dirname "$0")/.." && pwd)
base=main
sailtune=${SAILTUNE_DIR:-}
tag=
while [ $# -gt 0 ]; do
  case "$1" in
    --base) base=$2; shift 2;;
    --sailtune) sailtune=$2; shift 2;;
    --tag) tag=$2; shift 2;;
    *) echo "unknown option $1" >&2; exit 2;;
  esac
done
rc=0
run() { echo "== $*"; "$@" || rc=1; }
run python3 -I "$root/tools/release_check.py" --root "$root" version
run python3 -I "$root/tools/coverage_ratchet.py" growth --base "$base" --repo "$root"
if [ -n "$sailtune" ] && { [ -d "$sailtune/.git" ] || [ -f "$sailtune/.git" ]; }; then
  run python3 -I "$root/tools/shared_sync.py" --check --upstream "$sailtune" --pinned
else
  echo "== shared_sync --pinned: skipped (no SailTune checkout; set SAILTUNE_DIR or --sailtune)"; run python3 -I "$root/tools/shared_sync.py" --check
fi
[ -n "$tag" ] && run python3 -I "$root/tools/release_check.py" tag "$tag" --repo "$root"
echo "host gates: $([ $rc = 0 ] && echo PASS || echo FAIL)"
exit $rc
