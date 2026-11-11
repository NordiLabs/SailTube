#!/bin/bash
# SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
# Fast loop for host test suites: runs tests/<suite>/run.sh inside the sfos-m1-sailtube
# container against a private snapshot of this checkout (tracked + untracked, non-ignored
# files as they are on disk). It does not build the app and takes no build lock, so several
# suites can be iterated in parallel. The gate remains tools/native_build.sh --tests.
#
#   tools/catalog_test.sh catalog_core [catalog_search ...]
set -u
HERE=$(cd "$(dirname "$0")/.." && pwd)
DOCKER_SOCK="${DOCKER_SOCK:-unix://$HOME/.orbstack/run/docker.sock}"
CONTAINER="${SFOS_NATIVE_CONTAINER:-sfos-m1-sailtube}"
[ $# -ge 1 ] || { echo "usage: $0 <suite>..." >&2; exit 2; }
for s in "$@"; do [ -f "$HERE/tests/$s/run.sh" ] || { echo "no tests/$s/run.sh" >&2; exit 2; }; done
dir="/tmp/catalog-test-$$-$RANDOM"
( cd "$HERE" && { git ls-files -z; git ls-files -z -o --exclude-standard; } | sort -zu |
  while IFS= read -r -d '' f; do [ -e "$f" ] && printf '%s\0' "$f"; done |
  COPYFILE_DISABLE=1 tar --null -T - -cf - 2>/dev/null ) |
  docker -H "$DOCKER_SOCK" exec -i -u sdk "$CONTAINER" sh -c "mkdir -p $dir && tar -xf - -C $dir" || exit 2
rc=0
for s in "$@"; do
  docker -H "$DOCKER_SOCK" exec -u sdk -w "$dir" -e TMPDIR="$dir/.tmp" "$CONTAINER" sh -c "mkdir -p .tmp && sh tests/$s/run.sh" || rc=1
done
docker -H "$DOCKER_SOCK" exec -u sdk "$CONTAINER" rm -rf "$dir"
exit $rc
