// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "ShortsKnowledge.h"

#include <algorithm>
#include <chrono>
#include <set>
#include <sstream>

namespace nordtile { namespace video { namespace catalog {

const std::size_t ShortsKnowledge::kDurationCapacity;
const std::size_t ShortsKnowledge::kShortsCapacity;
const std::size_t ShortsKnowledge::kNotShortCapacity;
const std::size_t ShortsKnowledge::kChannelMemory;
const std::int64_t ShortsKnowledge::kRecheckIntervalMs;
const std::size_t ShortsKnowledge::kMaxChecksPerPlan;

namespace {

char evidenceCode(ShortEvidence e) {
    switch (e) {
    case ShortEvidence::Renderer: return 'r';
    case ShortEvidence::RssLink: return 'l';
    case ShortEvidence::ShortsTab: return 't';
    default: return 0;
    }
}

ShortEvidence evidenceFromCode(char c) {
    switch (c) {
    case 'r': return ShortEvidence::Renderer;
    case 'l': return ShortEvidence::RssLink;
    case 't': return ShortEvidence::ShortsTab;
    default: return ShortEvidence::None;
    }
}

// Lists the entries oldest first (the store iterates most recent first).
template <typename V>
std::vector<std::pair<std::string, V>> oldestFirst(const LruStore<std::string, V> &store) {
    std::vector<std::pair<std::string, V>> out(store.begin(), store.end());
    std::reverse(out.begin(), out.end());
    return out;
}

}  // namespace

ShortsKnowledge::ShortsKnowledge(std::function<std::int64_t()> nowMs)
    : nowMs_(std::move(nowMs)), durations_(kDurationCapacity), shorts_(kShortsCapacity),
      notShorts_(kNotShortCapacity), checkedAt_(kChannelMemory) {}

std::int64_t ShortsKnowledge::now() const {
    if (nowMs_) return nowMs_();
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

bool ShortsKnowledge::isVideoId(const std::string &id) {
    if (id.size() != 11) return false;
    for (char c : id) {
        const bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' || c == '_';
        if (!ok) return false;
    }
    return true;
}

void ShortsKnowledge::recordDuration(const std::string &videoId, std::int64_t seconds) {
    if (seconds <= 0 || !isVideoId(videoId)) return;
    durations_.put(videoId, seconds);
}

std::int64_t ShortsKnowledge::duration(const std::string &videoId) const {
    const std::int64_t *v = durations_.peek(videoId);
    return v ? *v : -1;
}

void ShortsKnowledge::markShort(const std::string &videoId, ShortEvidence evidence) {
    if (!isConfirmedShort(evidence) || !isVideoId(videoId)) return;
    notShorts_.erase(videoId);
    // Keep the first (strongest recorded) evidence kind, refresh recency.
    const int *known = shorts_.peek(videoId);
    shorts_.put(videoId, known ? *known : static_cast<int>(evidence));
}

void ShortsKnowledge::markNotShort(const std::string &videoId) {
    if (!isVideoId(videoId) || shorts_.contains(videoId)) return;
    notShorts_.put(videoId, true);
}

ShortEvidence ShortsKnowledge::evidence(const std::string &videoId) const {
    if (const int *e = shorts_.peek(videoId)) return static_cast<ShortEvidence>(*e);
    if (notShorts_.contains(videoId)) return ShortEvidence::NotShort;
    return ShortEvidence::None;
}

ShortsLookup ShortsKnowledge::lookup() const {
    const ShortsKnowledge *self = this;
    return [self](const std::string &id) { return self->evidence(id); };
}

bool ShortsKnowledge::observe(const std::vector<Item> &rows, bool fromShortsTab) {
    bool changed = false;
    for (const Item &row : rows) {
        if (!isVideoLike(row) || !isVideoId(row.id)) continue;
        if (row.durationSeconds > 0 && row.liveStatus != media::LiveStatus::IsLive && duration(row.id) != row.durationSeconds) {
            recordDuration(row.id, row.durationSeconds);
            changed = true;
        }
        ShortEvidence e = row.shortEvidence;
        if (fromShortsTab && !isConfirmedShort(e)) e = ShortEvidence::ShortsTab;
        if (isConfirmedShort(e) && !shorts_.contains(row.id)) {
            markShort(row.id, e);
            changed = true;
        } else if (isConfirmedShort(e)) {
            markShort(row.id, e);   // refresh recency
        }
    }
    return changed;
}

void ShortsKnowledge::fillDurations(std::vector<Item> &rows) const {
    for (Item &row : rows) {
        if (!isVideoLike(row) || row.durationSeconds > 0) continue;
        const std::int64_t d = duration(row.id);
        if (d > 0) row.durationSeconds = d;
    }
}

ShortEvidence ShortsKnowledge::classify(const Item &row) const {
    if (!isVideoLike(row)) return row.shortEvidence;
    Item copy = row;
    if (copy.durationSeconds <= 0) {
        const std::int64_t d = duration(copy.id);
        if (d > 0) copy.durationSeconds = d;
    }
    return effectiveShortEvidence(copy, lookup());
}

std::vector<std::string> ShortsKnowledge::channelsNeedingCheck(const std::vector<Item> &rows, std::size_t limit) const {
    std::vector<std::string> out;
    std::set<std::string> seen;
    for (const Item &row : rows) {
        if (out.size() >= limit) break;
        if (row.channelId.empty() || seen.count(row.channelId)) continue;
        if (classify(row) != ShortEvidence::Candidate) continue;
        seen.insert(row.channelId);
        if (recentlyChecked(row.channelId)) continue;
        out.push_back(row.channelId);
    }
    return out;
}

std::vector<std::string> ShortsKnowledge::candidatesOf(const std::string &channelId, const std::vector<Item> &rows) const {
    std::vector<std::string> out;
    std::set<std::string> seen;
    for (const Item &row : rows) {
        if (row.channelId != channelId || seen.count(row.id)) continue;
        if (classify(row) != ShortEvidence::Candidate) continue;
        seen.insert(row.id);
        out.push_back(row.id);
    }
    return out;
}

void ShortsKnowledge::noteChecked(const std::string &channelId) {
    if (!channelId.empty()) checkedAt_.put(channelId, now());
}

bool ShortsKnowledge::recentlyChecked(const std::string &channelId) const {
    const std::int64_t *at = checkedAt_.peek(channelId);
    return at && now() - *at < kRecheckIntervalMs;
}

int ShortsKnowledge::applyShortsTab(const std::string &channelId, const std::vector<std::string> &shortsTabIds,
                                    const std::vector<std::string> &candidateIds) {
    const std::set<std::string> listed(shortsTabIds.begin(), shortsTabIds.end());
    for (const std::string &id : shortsTabIds) markShort(id, ShortEvidence::ShortsTab);
    int settled = 0;
    for (const std::string &id : candidateIds) {
        if (listed.count(id)) { ++settled; continue; }
        if (shorts_.contains(id)) continue;
        markNotShort(id);
        ++settled;
    }
    noteChecked(channelId);
    return settled;
}

std::string ShortsKnowledge::serialize() const {
    std::ostringstream out;
    out << "sailtube-shorts 1\n";
    for (const auto &e : oldestFirst(durations_)) out << "d " << e.first << ' ' << e.second << '\n';
    for (const auto &e : oldestFirst(shorts_)) {
        const char code = evidenceCode(static_cast<ShortEvidence>(e.second));
        if (code) out << "s " << e.first << ' ' << code << '\n';
    }
    for (const auto &e : oldestFirst(notShorts_)) out << "n " << e.first << '\n';
    return out.str();
}

bool ShortsKnowledge::deserialize(const std::string &data) {
    std::istringstream in(data);
    std::string line;
    if (!std::getline(in, line) || line != "sailtube-shorts 1") return false;
    durations_.clear();
    shorts_.clear();
    notShorts_.clear();
    while (std::getline(in, line)) {
        std::istringstream fields(line);
        std::string kind, id;
        if (!(fields >> kind >> id) || !isVideoId(id)) continue;
        if (kind == "d") {
            long long seconds = 0;
            if (fields >> seconds) recordDuration(id, seconds);
        } else if (kind == "s") {
            std::string code;
            if (fields >> code && code.size() == 1) markShort(id, evidenceFromCode(code[0]));
        } else if (kind == "n") {
            markNotShort(id);
        }
    }
    return true;
}

}}}
