// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#pragma once

// The subscription list as the feed sees it (DSC-05). The library component owns the stored list
// (LIB-01) and feeds it here as plain {id, name} pairs. Only UC channel ids feed the feed.
// Qt-free.

#include <set>
#include <string>
#include <vector>

namespace nordtile { namespace video { namespace catalog {

struct ChannelRef {
    ChannelRef() {}
    ChannelRef(const std::string &channelId, const std::string &channelName) : id(channelId), name(channelName) {}
    std::string id;     // "UC" + 22 characters
    std::string name;   // display label only
};

using ChannelList = std::vector<ChannelRef>;

// "UC" followed by 22 characters of [A-Za-z0-9_-].
inline bool isFeedChannelId(const std::string &id) {
    if (id.size() != 24 || id[0] != 'U' || id[1] != 'C') return false;
    for (std::size_t i = 2; i < id.size(); ++i) {
        const char c = id[i];
        const bool ok = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' || c == '-';
        if (!ok) return false;
    }
    return true;
}

// The channels that take part in the feed: valid UC ids, first occurrence wins, order kept.
inline ChannelList feedChannels(const ChannelList &all) {
    ChannelList out;
    std::set<std::string> seen;
    for (const ChannelRef &c : all) {
        if (!isFeedChannelId(c.id) || !seen.insert(c.id).second) continue;
        out.push_back(c);
    }
    return out;
}

}}}
