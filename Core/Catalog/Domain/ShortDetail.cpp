// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "ShortDetail.h"
#include "Images.h"

#include <atomic>

namespace nordtile { namespace video { namespace catalog {

namespace {
bool isBlank(char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n'; }
}

std::string ShortDetail::descriptionShort(std::size_t limit) const {
    // First non-empty line, trimmed.
    std::size_t start = 0;
    while (start < description.size() && isBlank(description[start])) ++start;
    std::size_t end = start;
    while (end < description.size() && description[end] != '\n' && description[end] != '\r') ++end;
    while (end > start && isBlank(description[end - 1])) --end;
    const std::string line = description.substr(start, end - start);
    const bool moreLines = [&] {
        for (std::size_t i = end; i < description.size(); ++i) if (!isBlank(description[i])) return true;
        return false;
    }();
    // Cut at `limit` code points (a UTF-8 lead byte starts a new one).
    std::size_t points = 0, cut = line.size();
    for (std::size_t i = 0; i < line.size(); ++i) {
        if ((static_cast<unsigned char>(line[i]) & 0xC0) == 0x80) continue;
        if (points == limit) { cut = i; break; }
        ++points;
    }
    if (cut == line.size() && !moreLines) return line;
    std::string out = line.substr(0, cut);
    while (!out.empty() && isBlank(out[out.size() - 1])) out.erase(out.size() - 1);
    return out + "\xE2\x80\xA6";   // "…"
}

std::string ShortDetail::relatedVideoThumbnail() const {
    if (relatedVideoId.empty()) return std::string();
    return relatedVideoIsShort ? shortsThumbnailUrl(relatedVideoId) : listThumbnailUrl(relatedVideoId);
}

const bool kShortsDislikeEstimate = false;

namespace {
std::atomic<bool> &dislikeSwitch() {
    static std::atomic<bool> on(kShortsDislikeEstimate);
    return on;
}
}  // namespace

bool shortsDislikeEstimate() { return dislikeSwitch().load(); }

void setShortsDislikeEstimate(bool on) { dislikeSwitch().store(on); }

}}}
