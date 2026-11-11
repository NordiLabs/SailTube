// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "RowFilter.h"

namespace nordtile { namespace video { namespace catalog {

ShortEvidence effectiveShortEvidence(const Item &item, const ShortsLookup &lookup) {
    if (isConfirmedShort(item.shortEvidence) || item.shortEvidence == ShortEvidence::NotShort) return item.shortEvidence;
    if (lookup) {
        const ShortEvidence known = lookup(item.id);
        if (known != ShortEvidence::None) return known;
    }
    if (item.kind == ItemKind::Video && item.durationSeconds > 0 && item.durationSeconds <= kShortCandidateMaxSeconds &&
        item.liveStatus != media::LiveStatus::IsLive)
        return ShortEvidence::Candidate;
    return item.shortEvidence;
}

std::string rowSeenKey(const Item &item) {
    return std::to_string(static_cast<int>(item.kind == ItemKind::Short ? ItemKind::Video : item.kind)) + ":" + item.id;
}

RowFilterResult filterRows(const std::vector<Item> &rows, const RowFilterSettings &settings,
                           std::set<std::string> *seen, const ShortsLookup &lookup) {
    RowFilterResult out;
    out.visible.reserve(rows.size());
    for (const Item &row : rows) {
        if (seen) {
            if (!seen->insert(rowSeenKey(row)).second) { ++out.duplicates; continue; }
        }
        Item item = row;
        if (isVideoLike(item)) {
            item.shortEvidence = effectiveShortEvidence(item, lookup);
            if (isConfirmedShort(item.shortEvidence)) item.kind = ItemKind::Short;
        }
        if (settings.hideShorts && item.kind == ItemKind::Short && isConfirmedShort(item.shortEvidence)) { ++out.hiddenShorts; continue; }
        if (settings.hideMembersOnly && item.membersOnly) { ++out.hiddenMembersOnly; continue; }
        if (settings.hideRestricted && item.restricted) { ++out.hiddenRestricted; continue; }
        if (settings.hideWatched && isVideoLike(item) && settings.hideWatched(item.id)) { ++out.hiddenWatched; continue; }
        out.visible.push_back(item);
    }
    return out;
}

}}}
