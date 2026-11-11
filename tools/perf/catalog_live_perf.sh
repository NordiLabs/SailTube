#!/bin/bash
# SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
# OPT-IN live latency harness for the catalog component's network paths (tools/perf/CatalogLivePerf.cpp).
# It contacts LIVE services and is NEVER part of the offline gate (tools/native_build.sh --tests,
# tests/*/run.sh). Hosts contacted: www.youtube.com, suggestqueries.google.com.
#
# Like tools/catalog_test.sh it copies a private snapshot of this checkout (tracked + untracked,
# non-ignored files as they are on disk) into the sfos-m1-sailtube container, builds the
# harness there (moc + c++ -std=c++11 against Qt5Core/Qt5Network) and runs it. Extra arguments
# go to the program:
#
#   tools/perf/catalog_live_perf.sh [--runs N] [--query Q] [--prewarm] [--prefix P]
#                                   [--channel UC...] [--video ID] [--playlist ID]
set -u
HERE=$(cd "$(dirname "$0")/../.." && pwd)
DOCKER_SOCK="${DOCKER_SOCK:-unix://$HOME/.orbstack/run/docker.sock}"
CONTAINER="${SFOS_NATIVE_CONTAINER:-sfos-m1-sailtube}"
dir="/tmp/catalog-perf-$$-$RANDOM"
( cd "$HERE" && { git ls-files -z; git ls-files -z -o --exclude-standard; } | sort -zu |
  while IFS= read -r -d '' f; do [ -e "$f" ] && printf '%s\0' "$f"; done |
  COPYFILE_DISABLE=1 tar --no-xattrs --null -T - -cf - 2>/dev/null ) |
  docker -H "$DOCKER_SOCK" exec -i -u sdk "$CONTAINER" sh -c "mkdir -p $dir && tar -xf - -C $dir" || exit 2

# The build + run step, executed inside the container from the snapshot's root.
build_and_run='
set -e
root=$(pwd)
out=$root/.tmp/perf
mkdir -p "$out"
pkg-config --exists Qt5Core Qt5Network || { echo "catalog_live_perf: no Qt5Core/Qt5Network" >&2; exit 2; }
moc=$(pkg-config --variable=host_bins Qt5Core 2>/dev/null)/moc
[ -x "$moc" ] || moc=moc
MOC="Core/Catalog/Infrastructure/HttpTransport.h Core/Catalog/Infrastructure/WorkLane.h
 Core/Catalog/Infrastructure/ClientIdentity.h Core/Catalog/Infrastructure/InnerTubeClient.h
 Core/Catalog/Infrastructure/SearchService.h Core/Catalog/Infrastructure/SuggestService.h
 Core/Catalog/Infrastructure/ChannelService.h Core/Catalog/Infrastructure/ShortsStore.h
 Core/Catalog/Infrastructure/PlaylistService.h Core/Catalog/Infrastructure/WatchNextService.h
 Core/Catalog/Infrastructure/CommentsService.h"
SOURCES="tools/perf/CatalogLivePerf.cpp
 Core/Catalog/Domain/TextParse.cpp Core/Catalog/Domain/Images.cpp Core/Catalog/Domain/RowFilter.cpp
 Core/Catalog/Domain/SearchFilter.cpp Core/Catalog/Domain/SearchCache.cpp
 Core/Catalog/Domain/ShortsKnowledge.cpp Core/Catalog/Domain/ChannelReference.cpp
 Core/Catalog/Domain/Chapters.cpp Core/Catalog/Domain/Description.cpp Core/Catalog/Domain/Comments.cpp
 Core/Catalog/Infrastructure/HttpTransport.cpp Core/Catalog/Infrastructure/Json.cpp
 Core/Catalog/Infrastructure/ClientIdentity.cpp Core/Catalog/Infrastructure/InnerTubeClient.cpp
 Core/Catalog/Infrastructure/RendererParser.cpp Core/Catalog/Infrastructure/SearchService.cpp
 Core/Catalog/Infrastructure/SuggestService.cpp Core/Catalog/Infrastructure/ChannelService.cpp
 Core/Catalog/Infrastructure/ShortsStore.cpp Core/Catalog/Infrastructure/PlaylistService.cpp
 Core/Catalog/Infrastructure/WatchNextService.cpp Core/Catalog/Infrastructure/CommentsService.cpp
 Core/Catalog/Infrastructure/RssParser.cpp"
mocs=""
i=0
for h in $MOC; do
    i=$((i + 1))
    "$moc" -I"$root" "$root/$h" -o "$out/moc_$i.cpp"
    mocs="$mocs $out/moc_$i.cpp"
done
srcs=""
for s in $SOURCES; do srcs="$srcs $root/$s"; done
"${CXX:-c++}" -std=c++11 -O2 -fPIC -Wall -Wextra -I"$root" $(pkg-config --cflags Qt5Core Qt5Network | sed "s/-I/-isystem /g") \
    -o "$out/CatalogLivePerf" $srcs $mocs $(pkg-config --libs Qt5Core Qt5Network) -lpthread
echo "# built $out/CatalogLivePerf"
exec "$out/CatalogLivePerf" "$@"
'
rc=0
docker -H "$DOCKER_SOCK" exec -u sdk -w "$dir" -e TMPDIR="$dir/.tmp" "$CONTAINER" \
    sh -c "mkdir -p .tmp && $build_and_run" catalog_live_perf "$@" || rc=$?
docker -H "$DOCKER_SOCK" exec -u sdk "$CONTAINER" rm -rf "$dir"
exit $rc
