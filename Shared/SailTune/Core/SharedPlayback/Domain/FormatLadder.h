// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#pragma once
// Which format first, and what comes after it. Pure functions over StreamFormat lists.
//
// Audio ladder (best first):
//  1. language: the original / default track, then the requested dub, then others, descriptive last;
//  2. codec: the request's preference (default Opus, then AAC). Opus in WebM can be push-seeked
//     through a local fetcher; AAC in MP4 needs a demuxer that cannot on the target stack;
//  3. the regular variant before the "stable volume" (DRC) one;
//  4. higher bitrate first;
// then duplicates (same codec family and kbps rounded to 8) are dropped. A bitrate ceiling removes
// the formats above it unless that would leave nothing. A muxed last resort follows separately.
#include "StreamTypes.h"
#include <algorithm>
#include <cstddef>
#include <string>
#include <vector>

namespace nordtile { namespace music { namespace sharedplayback { namespace domain {

namespace detail {
inline int languageRank(const StreamFormat &f, const std::string &dub) {
    if (f.isDescriptive) return 3;
    if (dub.empty()) return f.isDefaultAudio ? 0 : 2;
    if (!f.audioLanguage.empty() && f.audioLanguage.compare(0, dub.size(), dub) == 0) return 0;
    return f.isDefaultAudio ? 1 : 2;
}
inline int codecRank(const StreamFormat &f, const std::vector<std::string> &preference) {
    const std::string family = f.codecFamily();
    static const char *const defaults[] = {"opus", "aac"};
    if (preference.empty()) {
        for (int i = 0; i < 2; ++i) if (family == defaults[i]) return i;
        return 2;
    }
    for (std::size_t i = 0; i < preference.size(); ++i) if (family == preference[i]) return int(i);
    return int(preference.size());
}
inline bool betterAudio(const StreamFormat &a, const StreamFormat &b, const StreamRequest &r) {
    const int la = languageRank(a, r.dubLanguage), lb = languageRank(b, r.dubLanguage);
    if (la != lb) return la < lb;
    const int ca = codecRank(a, r.codecPreference), cb = codecRank(b, r.codecPreference);
    if (ca != cb) return ca < cb;
    if (a.isDrc != b.isDrc) return !a.isDrc;
    if (a.bitrate != b.bitrate) return a.bitrate > b.bitrate;
    return a.itag < b.itag;
}
}  // namespace detail

// Audio-only formats with a direct address, ranked and deduplicated.
inline std::vector<StreamFormat> rankAudio(const std::vector<StreamFormat> &formats, const StreamRequest &request) {
    std::vector<StreamFormat> usable;
    for (std::size_t i = 0; i < formats.size(); ++i) {
        const StreamFormat &f = formats[i];
        if (f.url.empty() || !f.hasAudio || f.hasVideo || f.isHls) continue;
        usable.push_back(f);
    }
    if (request.maxAudioKbps > 0) {
        std::vector<StreamFormat> within;
        for (std::size_t i = 0; i < usable.size(); ++i)
            if (usable[i].bitrate / 1000 <= request.maxAudioKbps) within.push_back(usable[i]);
        if (!within.empty()) usable.swap(within);
    }
    std::stable_sort(usable.begin(), usable.end(), [&request](const StreamFormat &a, const StreamFormat &b) {
        return detail::betterAudio(a, b, request);
    });
    std::vector<StreamFormat> ladder;
    for (std::size_t i = 0; i < usable.size(); ++i) {
        bool duplicate = false;
        for (std::size_t j = 0; j < ladder.size() && !duplicate; ++j)
            duplicate = ladder[j].codecFamily() == usable[i].codecFamily() &&
                        (ladder[j].bitrate + 4000) / 8000 == (usable[i].bitrate + 4000) / 8000 &&
                        ladder[j].isDrc == usable[i].isDrc && ladder[j].audioLanguage == usable[i].audioLanguage &&
                        ladder[j].isDescriptive == usable[i].isDescriptive && ladder[j].isDefaultAudio == usable[i].isDefaultAudio;
        if (!duplicate) ladder.push_back(usable[i]);
    }
    return ladder;
}

// Video-only formats at most `maxVideoHeight` tall; AV1 is excluded; with `hardwareDecode`
// H.264 comes before VP9. Larger first within a codec rank.
inline std::vector<StreamFormat> rankVideo(const std::vector<StreamFormat> &formats, const StreamRequest &request) {
    std::vector<StreamFormat> usable;
    for (std::size_t i = 0; i < formats.size(); ++i) {
        const StreamFormat &f = formats[i];
        if (f.url.empty() || !f.hasVideo || f.hasAudio || f.isHls) continue;
        const std::string family = f.codecFamily();
        if (family == "av1") continue;
        if (request.maxVideoHeight > 0 && f.height > request.maxVideoHeight) continue;
        usable.push_back(f);
    }
    const bool hw = request.hardwareDecode;
    std::stable_sort(usable.begin(), usable.end(), [hw](const StreamFormat &a, const StreamFormat &b) {
        const int ra = hw && a.codecFamily() == "h264" ? 0 : 1, rb = hw && b.codecFamily() == "h264" ? 0 : 1;
        if (ra != rb) return ra < rb;
        if (a.height != b.height) return a.height > b.height;
        if (a.fps != b.fps) return a.fps > b.fps;
        return a.bitrate > b.bitrate;
    });
    return usable;
}

// Progressive audio+video streams (itag 18 and the like), best first: the muxed last resort.
inline std::vector<StreamFormat> rankMuxed(const std::vector<StreamFormat> &formats) {
    std::vector<StreamFormat> usable;
    for (std::size_t i = 0; i < formats.size(); ++i)
        if (!formats[i].url.empty() && formats[i].hasAudio && formats[i].hasVideo && !formats[i].isHls) usable.push_back(formats[i]);
    std::stable_sort(usable.begin(), usable.end(), [](const StreamFormat &a, const StreamFormat &b) { return a.height > b.height; });
    return usable;
}

}}}}
