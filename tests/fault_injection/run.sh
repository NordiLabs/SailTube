#!/bin/sh
# SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
# NFR-ROB-01 fault injection (docs/FAULT_INJECTION.md): named cases that inject malformed JSON/XML,
# a missing renderer, HTTP 404/429/5xx, network errors, timeouts and one bad item among healthy
# ones into the real discovery services (search, channel tabs, feed refresh, comments, related)
# and the real resolve path (fast path -> yt-dlp extractor fallback, fixed error classes, one bad
# format in a list). Qt5Core + Qt5Network (moc), POSIX sh, run from anywhere; skipped where the Qt
# development files are absent. Prints one "CASE <name>: PASS|FAIL|KNOWN-BUG|GAP" line per case.
set -e
here=$(cd "$(dirname "$0")" && pwd)
root=$(cd "$here/../.." && pwd)
if ! command -v pkg-config >/dev/null 2>&1 || ! pkg-config --exists Qt5Core Qt5Network 2>/dev/null; then
    echo "fault_injection: SKIPPED (no Qt5Core/Qt5Network)"; echo "fault_injection: PASS"; exit 0
fi
out=$(mktemp -d "${TMPDIR:-/tmp}/sailtube-fault.XXXXXX")
trap 'rm -rf "$out"' EXIT
moc=$(pkg-config --variable=host_bins Qt5Core 2>/dev/null)/moc
[ -x "$moc" ] || moc=$(command -v moc || command -v moc-qt5)
cxx=${CXX:-c++}
qtflags=$(pkg-config --cflags Qt5Core Qt5Network | sed "s/-I/-isystem /g")
qtlibs=$(pkg-config --libs Qt5Core Qt5Network)
flags="-std=c++11 -fPIC -Wall -Wextra -Wno-deprecated-copy -I$root $qtflags"

mocs() { # tag header... -> generated sources on stdout
    tag=$1; shift
    n=0
    for h in "$@"; do
        n=$((n + 1))
        "$moc" -I"$root" "$root/$h" -o "$out/moc_${tag}_$n.cpp" || exit 1
        printf '%s ' "$out/moc_${tag}_$n.cpp"
    done
}

# ---- 1. discovery (catalog services, presentation comments model) ---------------------------
catalog_mocs=$(mocs cat Core/Catalog/Infrastructure/HttpTransport.h Core/Catalog/Infrastructure/WorkLane.h \
    Core/Catalog/Infrastructure/ClientIdentity.h Core/Catalog/Infrastructure/InnerTubeClient.h \
    Core/Catalog/Infrastructure/SearchService.h Core/Catalog/Infrastructure/FeedService.h \
    Core/Catalog/Infrastructure/ChannelService.h Core/Catalog/Infrastructure/ShortsStore.h \
    Core/Catalog/Infrastructure/CommentsService.h Core/Catalog/Infrastructure/WatchNextService.h \
    Clients/Sailfish/Presentation/CommentsModel.h)
catalog_sources=""
for s in Core/Catalog/Domain/TextParse.cpp Core/Catalog/Domain/Images.cpp Core/Catalog/Domain/RowFilter.cpp \
    Core/Catalog/Domain/SearchFilter.cpp Core/Catalog/Domain/FeedSchedule.cpp Core/Catalog/Domain/Comments.cpp \
    Core/Catalog/Domain/Chapters.cpp Core/Catalog/Domain/Description.cpp Core/Catalog/Domain/ShortsKnowledge.cpp \
    Core/Catalog/Domain/ChannelReference.cpp \
    Core/Catalog/Infrastructure/HttpTransport.cpp Core/Catalog/Infrastructure/Json.cpp \
    Core/Catalog/Infrastructure/ClientIdentity.cpp Core/Catalog/Infrastructure/InnerTubeClient.cpp \
    Core/Catalog/Infrastructure/RendererParser.cpp Core/Catalog/Infrastructure/SearchService.cpp \
    Core/Catalog/Infrastructure/RssParser.cpp Core/Catalog/Infrastructure/FeedCacheStore.cpp \
    Core/Catalog/Infrastructure/FeedService.cpp Core/Catalog/Infrastructure/ChannelService.cpp \
    Core/Catalog/Infrastructure/ShortsStore.cpp Core/Catalog/Infrastructure/CommentsService.cpp \
    Core/Catalog/Infrastructure/WatchNextService.cpp Clients/Sailfish/Presentation/CommentsModel.cpp; do
    catalog_sources="$catalog_sources $root/$s"
done
# shellcheck disable=SC2086
"$cxx" $flags -o "$out/catalog" "$here/CatalogFaultTest.cpp" $catalog_sources $catalog_mocs $qtlibs -lpthread
SAILTUBE_FAULT_FIXTURES="$here/fixtures" QT_QPA_PLATFORM=minimal timeout -s KILL 240 "$out/catalog"

# ---- 2. resolve (SharedStreamSource -> YtDlpStreamResolver fallback, pinned component parser) --
st="Shared/SailTune/Core/SharedPlayback/Infrastructure"
resolve_mocs=$(mocs res "$st/HttpsConnection.h" "$st/FastStreamResolver.h" \
    Core/Resolver/Infrastructure/YtDlpRunner.h Core/Resolver/Infrastructure/YtDlpVideoService.h)
resolve_sources=""
for s in Core/Player/Infrastructure/SharedStreamSource.cpp Core/Player/Infrastructure/SharedStreamSourceLadder.cpp \
    Core/Player/Infrastructure/SharedStreamSourceThreaded.cpp \
    Core/Player/Infrastructure/YtDlpStreamResolver.cpp Core/Player/Infrastructure/YtDlpStreamMapping.cpp \
    Core/Player/Domain/ResolveCache.cpp \
    Core/Resolver/Infrastructure/YtDlpJson.cpp Core/Resolver/Infrastructure/YtDlpRunner.cpp \
    Core/Resolver/Infrastructure/YtDlpVideoService.cpp \
    "$st/FastStreamResolver.cpp" "$st/HttpsConnection.cpp" "$st/MediaProxy.cpp" "$st/PlayerResponseParser.cpp"; do
    resolve_sources="$resolve_sources $root/$s"
done
# shellcheck disable=SC2086
"$cxx" $flags -o "$out/resolve" "$here/ResolveFaultTest.cpp" $resolve_sources $resolve_mocs $qtlibs -lpthread
SAILTUBE_FAULT_FIXTURES="$here/fixtures" QT_QPA_PLATFORM=minimal timeout -s KILL 240 "$out/resolve"

echo "fault_injection: PASS"
