// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#pragma once

// The one list filter every discovery list uses (search, home, feed, channel, related).
//  * Shorts are hidden only when confirmed by structural evidence (DSC-17). A candidate
//    (180 s or less, unchecked) stays visible.
//  * Members-only and restricted rows are badged, or hidden by setting, and never dropped
//    silently: every hidden row is counted by reason (DSC-18).
//  * Duplicates (by kind + id) are removed across pages when the caller passes a seen set.
//  * Watched videos are hidden when the library's hide-watched predicate says so (LIB-07), and
//    counted like the other reasons.
// Qt-free.

#include "Item.h"

#include <functional>
#include <set>
#include <string>
#include <vector>

namespace nordtile { namespace video { namespace catalog {

struct RowFilterSettings {
    bool hideShorts = false;
    bool hideMembersOnly = false;
    bool hideRestricted = false;
    // LIB-07: true when a video id is to be hidden as watched in this list (library component's
    // watch index); empty = show all. Called on the caller's thread.
    std::function<bool(const std::string &videoId)> hideWatched;
};

struct RowFilterResult {
    std::vector<Item> visible;
    int hiddenShorts = 0;
    int hiddenMembersOnly = 0;
    int hiddenRestricted = 0;
    int hiddenWatched = 0;
    int duplicates = 0;
    int hiddenTotal() const { return hiddenShorts + hiddenMembersOnly + hiddenRestricted + hiddenWatched; }
};

// Optional lookup into the persistent Shorts knowledge (DSC-17 stores): returns the best
// evidence known for a video id, or ShortEvidence::None.
using ShortsLookup = std::function<ShortEvidence(const std::string &videoId)>;

// The key a row is deduplicated under across pages (the "seen" set of filterRows): its kind and
// id, with a Short counted as a video, so the same video never shows twice.
std::string rowSeenKey(const Item &item);

// The evidence to use for a row: structural evidence on the row wins, then the store, then the
// 180 s candidate rule.
ShortEvidence effectiveShortEvidence(const Item &item, const ShortsLookup &lookup);

RowFilterResult filterRows(const std::vector<Item> &rows, const RowFilterSettings &settings,
                           std::set<std::string> *seen = nullptr, const ShortsLookup &lookup = ShortsLookup());

const std::int64_t kShortCandidateMaxSeconds = 180;

}}}
