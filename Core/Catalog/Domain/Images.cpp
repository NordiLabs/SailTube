// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "Images.h"

#include <cstdlib>

namespace nordtile { namespace video { namespace catalog {

std::string listThumbnailUrl(const std::string &videoId) {
    return videoId.empty() ? std::string() : "https://i.ytimg.com/vi/" + videoId + "/mqdefault.jpg";
}

std::string largeThumbnailUrl(const std::string &videoId) {
    return videoId.empty() ? std::string() : "https://i.ytimg.com/vi/" + videoId + "/hq720.jpg";
}

std::string thumbnailVideoId(const std::string &thumbnailUrl) {
    static const std::string prefix = "https://i.ytimg.com/vi/";
    if (thumbnailUrl.compare(0, prefix.size(), prefix) != 0) return std::string();
    const std::size_t slash = thumbnailUrl.find('/', prefix.size());
    if (slash == std::string::npos || slash - prefix.size() != 11) return std::string();
    const std::string name = thumbnailUrl.substr(slash + 1);
    if (name != "mqdefault.jpg" && name != "hqdefault.jpg" && name != "hq720.jpg") return std::string();
    return thumbnailUrl.substr(prefix.size(), 11);
}

std::string shortsThumbnailUrl(const std::string &videoId) {
    return videoId.empty() ? std::string() : "https://i.ytimg.com/vi/" + videoId + "/frame0.jpg";
}

std::string detailThumbnailUrl(const std::string &videoId) {
    return videoId.empty() ? std::string() : "https://i.ytimg.com/vi/" + videoId + "/hqdefault.jpg";
}

std::string absoluteUrl(const std::string &url) {
    if (url.compare(0, 2, "//") == 0) return "https:" + url;
    return url;
}

std::string rewriteAvatarUrl(const std::string &input, int size) {
    std::string url = absoluteUrl(input);
    const std::string sz = std::to_string(size);
    // "...=s68-c-k-c0x00ffffff-no-rj" or "...=s88"
    size_t p = url.rfind("=s");
    if (p != std::string::npos && p + 2 < url.size() && url[p + 2] >= '0' && url[p + 2] <= '9') {
        size_t e = p + 2;
        while (e < url.size() && url[e] >= '0' && url[e] <= '9') ++e;
        return url.substr(0, p + 2) + sz + url.substr(e);
    }
    // Older path form ".../s88-c-k-no/photo.jpg".
    p = url.find("/s");
    while (p != std::string::npos) {
        size_t e = p + 2;
        while (e < url.size() && url[e] >= '0' && url[e] <= '9') ++e;
        if (e > p + 2 && e < url.size() && (url[e] == '-' || url[e] == '/'))
            return url.substr(0, p + 2) + sz + url.substr(e);
        p = url.find("/s", p + 1);
    }
    return url;
}

std::string chooseAvatar(const std::vector<ImageCandidate> &candidates, int size) {
    const ImageCandidate *best = nullptr;     // smallest >= size
    const ImageCandidate *largest = nullptr;
    for (const ImageCandidate &c : candidates) {
        if (c.url.empty() || std::abs(c.width - c.height) > 2) continue;
        if (!largest || c.width > largest->width) largest = &c;
        if (c.width >= size && (!best || c.width < best->width)) best = &c;
    }
    const ImageCandidate *pick = best ? best : largest;
    return pick ? rewriteAvatarUrl(pick->url, size) : std::string();
}

}}}
