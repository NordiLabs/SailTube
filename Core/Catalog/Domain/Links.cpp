// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "Links.h"

namespace nordtile { namespace video { namespace catalog {

const std::size_t LinkQueue::kCapacity;

namespace {

bool idChar(char c) {
    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' || c == '-';
}

bool allIdChars(const std::string &s) {
    for (char c : s) if (!idChar(c)) return false;
    return true;
}

std::string lower(std::string s) {
    for (char &c : s) if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    return s;
}

std::string trim(const std::string &s) {
    const char *ws = " \t\r\n\f\v";
    const std::size_t b = s.find_first_not_of(ws);
    if (b == std::string::npos) return std::string();
    const std::size_t e = s.find_last_not_of(ws);
    return s.substr(b, e - b + 1);
}

int hexValue(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

std::string percentDecode(const std::string &s, bool plusIsSpace) {
    std::string out;
    out.reserve(s.size());
    for (std::size_t i = 0; i < s.size(); ++i) {
        const char c = s[i];
        if (c == '%' && i + 2 < s.size()) {
            const int h = hexValue(s[i + 1]);
            const int l = hexValue(s[i + 2]);
            if (h >= 0 && l >= 0) { out += static_cast<char>(h * 16 + l); i += 2; continue; }
        }
        out += (plusIsSpace && c == '+') ? ' ' : c;
    }
    return out;
}

struct Param {
    std::string key;
    std::string value;
};

std::vector<Param> parseParams(const std::string &query) {
    std::vector<Param> params;
    std::size_t start = 0;
    while (start <= query.size()) {
        std::size_t end = query.find('&', start);
        if (end == std::string::npos) end = query.size();
        const std::string part = query.substr(start, end - start);
        if (!part.empty()) {
            Param p;
            const std::size_t eq = part.find('=');
            p.key = percentDecode(eq == std::string::npos ? part : part.substr(0, eq), true);
            p.value = eq == std::string::npos ? std::string() : percentDecode(part.substr(eq + 1), true);
            params.push_back(p);
        }
        start = end + 1;
    }
    return params;
}

// First occurrence wins; `found` tells an empty value from an absent key.
std::string param(const std::vector<Param> &params, const char *key, bool *found = nullptr) {
    for (const Param &p : params) {
        if (p.key == key) { if (found) *found = true; return p.value; }
    }
    if (found) *found = false;
    return std::string();
}

struct ParsedUrl {
    ParsedUrl() : ok(false) {}
    bool ok;
    std::string host;
    std::vector<std::string> segments; // raw (still percent-encoded), empty segments skipped
    std::string query;
    std::string fragment;
};

ParsedUrl parseUrl(const std::string &input) {
    ParsedUrl u;
    std::string rest = input;
    const std::size_t sep = rest.find("://");
    if (sep != std::string::npos) {
        const std::string scheme = lower(rest.substr(0, sep));
        if (scheme != "http" && scheme != "https") return u;
        rest = rest.substr(sep + 3);
    } else if (rest.compare(0, 2, "//") == 0) {
        rest = rest.substr(2);
    } else {
        // No scheme: accept "youtu.be/..." style input, but nothing with another scheme ("mailto:").
        const std::size_t colon = rest.find(':');
        const std::size_t slash = rest.find_first_of("/?#");
        if (colon != std::string::npos && (slash == std::string::npos || colon < slash)) {
            // "host:port/..." is fine; "scheme:..." is not. A port is all digits.
            std::size_t p = colon + 1;
            const std::size_t portEnd = slash == std::string::npos ? rest.size() : slash;
            if (p == portEnd) return u;
            for (; p < portEnd; ++p) if (rest[p] < '0' || rest[p] > '9') return u;
        }
    }
    const std::size_t authorityEnd = rest.find_first_of("/?#");
    std::string authority = rest.substr(0, authorityEnd);
    const std::size_t at = authority.rfind('@');
    if (at != std::string::npos) authority = authority.substr(at + 1);
    const std::size_t colon = authority.find(':');
    if (colon != std::string::npos) authority = authority.substr(0, colon);
    while (!authority.empty() && authority[authority.size() - 1] == '.') authority.erase(authority.size() - 1);
    u.host = lower(authority);
    if (u.host.empty()) return u;

    std::string tail = authorityEnd == std::string::npos ? std::string() : rest.substr(authorityEnd);
    const std::size_t hash = tail.find('#');
    if (hash != std::string::npos) { u.fragment = tail.substr(hash + 1); tail = tail.substr(0, hash); }
    const std::size_t question = tail.find('?');
    if (question != std::string::npos) { u.query = tail.substr(question + 1); tail = tail.substr(0, question); }
    std::size_t start = 0;
    while (start < tail.size()) {
        std::size_t end = tail.find('/', start);
        if (end == std::string::npos) end = tail.size();
        if (end > start) u.segments.push_back(tail.substr(start, end - start));
        start = end + 1;
    }
    u.ok = true;
    return u;
}

int parseIndex(const std::string &value) {
    if (value.empty() || value.size() > 6) return -1;
    int n = 0;
    for (char c : value) {
        if (c < '0' || c > '9') return -1;
        n = n * 10 + (c - '0');
    }
    return n;
}

bool isValidClipId(const std::string &id) {
    return !id.empty() && id.size() <= 64 && allIdChars(id);
}

std::string startValue(const std::vector<Param> &query, const std::vector<Param> &fragment) {
    bool found = false;
    std::string v = param(query, "t", &found);
    if (found) return v;
    v = param(query, "start", &found);
    if (found) return v;
    v = param(fragment, "t", &found);
    if (found) return v;
    return param(fragment, "start");
}

} // namespace

bool isYouTubeHost(const std::string &host) {
    return host == "youtube.com" || host == "www.youtube.com" || host == "m.youtube.com" || host == "youtu.be"
        || host == "www.youtube-nocookie.com" || host == "youtube-nocookie.com" || host == "music.youtube.com";
}

bool isValidVideoId(const std::string &id) {
    return id.size() == 11 && allIdChars(id);
}

bool isValidPlaylistId(const std::string &id) {
    return id.size() >= 2 && id.size() <= 64 && allIdChars(id);
}

bool isValidChannelId(const std::string &id) {
    return id.size() == 24 && id.compare(0, 2, "UC") == 0 && allIdChars(id);
}

int parseStartTime(const std::string &input) {
    const std::string value = lower(trim(input));
    if (value.empty() || value.size() > 16) return -1;
    long total = 0;
    long number = 0;
    bool haveNumber = false;
    int lastUnit = 4; // h=3, m=2, s=1: units must appear once each, in that order
    for (char c : value) {
        if (c >= '0' && c <= '9') {
            number = number * 10 + (c - '0');
            haveNumber = true;
            if (number > 10L * 24 * 3600) return -1;
            continue;
        }
        int unit = 0;
        long scale = 0;
        if (c == 'h') { unit = 3; scale = 3600; }
        else if (c == 'm') { unit = 2; scale = 60; }
        else if (c == 's') { unit = 1; scale = 1; }
        else return -1;
        if (!haveNumber || unit >= lastUnit) return -1;
        total += number * scale;
        lastUnit = unit;
        number = 0;
        haveNumber = false;
    }
    if (haveNumber) {
        // A bare number is seconds ("90"); a trailing number after units is seconds too ("1m30").
        if (lastUnit <= 1) return -1;
        total += number;
    }
    if (total > 10L * 24 * 3600) return -1;
    return static_cast<int>(total);
}

LinkTarget classifyLink(const std::string &input) {
    LinkTarget t;
    t.url = trim(input);
    const ParsedUrl u = parseUrl(t.url);
    if (!u.ok || !isYouTubeHost(u.host)) return t;
    t.host = u.host;
    t.sailTuneHost = u.host == "music.youtube.com";
    t.embed = u.host == "www.youtube-nocookie.com" || u.host == "youtube-nocookie.com";

    const std::vector<Param> query = parseParams(u.query);
    const std::vector<Param> fragment = parseParams(u.fragment);
    const std::string first = u.segments.empty() ? std::string() : lower(u.segments[0]);
    const std::string second = u.segments.size() > 1 ? percentDecode(u.segments[1], false) : std::string();

    // 1. youtu.be/<id>
    std::string video;
    if (u.host == "youtu.be" && !u.segments.empty()) {
        const std::string id = percentDecode(u.segments[0], false);
        if (isValidVideoId(id)) video = id;
    }
    // 2. v=
    if (video.empty()) {
        const std::string v = param(query, "v");
        if (isValidVideoId(v)) video = v;
    }
    // 3. /shorts/ /embed/ /live/ /v/ ("/embed/videoseries" is a playlist embed, not an id)
    if (video.empty() && u.host != "youtu.be" && isValidVideoId(second)
        && (first == "shorts" || first == "embed" || first == "live" || first == "v")
        && !(first == "embed" && second == "videoseries")) {
        video = second;
    }
    if (first == "embed") t.embed = true;

    // 4. list=
    const std::string list = param(query, "list");
    const bool haveList = isValidPlaylistId(list);
    if (!video.empty()) {
        t.kind = LinkKind::Video;
        t.videoId = video;
        t.startSeconds = parseStartTime(startValue(query, fragment));
        if (haveList) {
            t.playlistId = list;
            t.index = parseIndex(param(query, "index"));
        }
        return t;
    }
    if (haveList) {
        t.kind = LinkKind::Playlist;
        t.playlistId = list;
        t.index = parseIndex(param(query, "index"));
        return t;
    }
    if (t.sailTuneHost) return t; // music.youtube.com channels/browse pages are SailTune's, not ours
    // 5. /channel/UC...
    if (first == "channel" && isValidChannelId(second)) {
        t.kind = LinkKind::Channel;
        t.channelKind = ChannelRefKind::ChannelId;
        t.channelRef = second;
        return t;
    }
    // 6. /@handle, /c/<name>, /user/<name>
    if (!u.segments.empty() && u.segments[0].size() > 1 && u.segments[0][0] == '@') {
        t.kind = LinkKind::Channel;
        t.channelKind = ChannelRefKind::Handle;
        t.channelRef = percentDecode(u.segments[0], false);
        return t;
    }
    if ((first == "c" || first == "user") && u.segments.size() > 1) {
        t.kind = LinkKind::Channel;
        t.channelKind = first == "c" ? ChannelRefKind::Custom : ChannelRefKind::User;
        // Kept percent-encoded: the reference is resolved as a URL (navigation/resolve_url).
        t.channelRef = "https://www.youtube.com/" + first + "/" + u.segments[1];
        return t;
    }
    // 7. /clip/<id> (MAY)
    if (first == "clip" && isValidClipId(second)) {
        t.kind = LinkKind::Clip;
        t.clipId = second;
        return t;
    }
    return t;
}

int chooseLinkCandidate(const std::vector<std::string> &candidates) {
    int firstNonEmpty = -1;
    for (std::size_t i = 0; i < candidates.size(); ++i) {
        if (classifyLink(candidates[i]).supported()) return static_cast<int>(i);
        if (firstNonEmpty < 0 && !trim(candidates[i]).empty()) firstNonEmpty = static_cast<int>(i);
    }
    return firstNonEmpty;
}

bool LinkQueue::push(const LinkTarget &link) {
    if (ready_) return true;
    if (queue_.size() >= kCapacity) { ++dropped_; return false; }
    queue_.push_back(link);
    return false;
}

std::vector<LinkTarget> LinkQueue::setReady(bool ready) {
    ready_ = ready;
    std::vector<LinkTarget> out;
    if (!ready) return out;
    out.assign(queue_.begin(), queue_.end());
    queue_.clear();
    return out;
}

}}}
