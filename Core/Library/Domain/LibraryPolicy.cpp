// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "LibraryPolicy.h"

#include "Core/Catalog/Domain/Video.h"

#include <algorithm>
#include <cstdio>

namespace nordtile { namespace video { namespace library {

namespace {

char asciiLower(char c) { return (c >= 'A' && c <= 'Z') ? char(c - 'A' + 'a') : c; }

std::string asciiLowerString(const std::string &s) {
    std::string out(s);
    for (std::size_t i = 0; i < out.size(); ++i) out[i] = asciiLower(out[i]);
    return out;
}

bool startsWithNoCase(const std::string &s, const char *prefix) {
    std::size_t i = 0;
    for (; prefix[i]; ++i) {
        if (i >= s.size() || asciiLower(s[i]) != prefix[i]) return false;
    }
    return true;
}

bool endsWith(const std::string &s, const std::string &suffix) {
    return s.size() >= suffix.size() && s.compare(s.size() - suffix.size(), suffix.size(), suffix) == 0;
}

// Control characters and ASCII space; UTF-8 continuation bytes are never whitespace.
bool isSpaceByte(unsigned char c) { return c <= 0x20 || c == 0x7f; }

std::string trimmed(const std::string &s) {
    std::size_t b = 0, e = s.size();
    while (b < e && isSpaceByte(static_cast<unsigned char>(s[b]))) ++b;
    while (e > b && isSpaceByte(static_cast<unsigned char>(s[e - 1]))) --e;
    return s.substr(b, e - b);
}

struct UrlParts {
    bool ok = false;
    std::string host;   // lowercase, no port, no user info
    std::string path;   // starts with '/' or empty
    std::string query;  // without '?'
};

UrlParts splitUrl(const std::string &input) {
    UrlParts u;
    std::string s = trimmed(input);
    if (startsWithNoCase(s, "https://")) s.erase(0, 8);
    else if (startsWithNoCase(s, "http://")) s.erase(0, 7);
    else if (s.compare(0, 2, "//") == 0) s.erase(0, 2);
    else if (s.find("://") != std::string::npos) return u; // another scheme
    const std::size_t hostEnd = s.find_first_of("/?#");
    std::string host = s.substr(0, hostEnd);
    const std::string rest = hostEnd == std::string::npos ? std::string() : s.substr(hostEnd);
    const std::size_t at = host.rfind('@');
    if (at != std::string::npos) host.erase(0, at + 1);
    const std::size_t colon = host.find(':');
    if (colon != std::string::npos) host.erase(colon);
    if (!host.empty() && host[host.size() - 1] == '.') host.erase(host.size() - 1);
    if (host.empty()) return u;
    u.host = asciiLowerString(host);
    const std::size_t fragment = rest.find('#');
    const std::string noFragment = rest.substr(0, fragment);
    const std::size_t q = noFragment.find('?');
    u.path = noFragment.substr(0, q);
    if (q != std::string::npos) u.query = noFragment.substr(q + 1);
    u.ok = true;
    return u;
}

bool hostIs(const std::string &host, const char *domain) {
    const std::string d(domain);
    return host == d || endsWith(host, "." + d);
}

bool isYouTubeHost(const std::string &host) {
    return hostIs(host, "youtube.com") || hostIs(host, "youtube-nocookie.com");
}

std::vector<std::string> pathSegments(const std::string &path) {
    std::vector<std::string> out;
    std::size_t i = 0;
    while (i < path.size()) {
        std::size_t j = path.find('/', i);
        if (j == std::string::npos) j = path.size();
        if (j > i) out.push_back(path.substr(i, j - i));
        i = j + 1;
    }
    return out;
}

std::string queryValue(const std::string &query, const char *key) {
    const std::string k(key);
    std::size_t i = 0;
    while (i <= query.size()) {
        std::size_t j = query.find_first_of("&;", i);
        if (j == std::string::npos) j = query.size();
        const std::string pair = query.substr(i, j - i);
        const std::size_t eq = pair.find('=');
        if (eq != std::string::npos && pair.compare(0, eq, k) == 0 && eq == k.size()) return pair.substr(eq + 1);
        i = j + 1;
    }
    return std::string();
}

bool idChar(char c) {
    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' || c == '-';
}

bool validListId(const std::string &id) {
    if (id.size() < 2 || id.size() > 64) return false;
    for (std::size_t i = 0; i < id.size(); ++i) {
        if (!idChar(id[i])) return false;
    }
    return true;
}

std::string validVideoOrEmpty(const std::string &id) {
    return catalog::validVideoId(id) ? id : std::string();
}

int compareNameFolded(const std::string &a, const std::string &b) {
    const std::size_t n = std::min(a.size(), b.size());
    for (std::size_t i = 0; i < n; ++i) {
        const unsigned char x = static_cast<unsigned char>(asciiLower(a[i]));
        const unsigned char y = static_cast<unsigned char>(asciiLower(b[i]));
        if (x != y) return x < y ? -1 : 1;
    }
    if (a.size() == b.size()) return 0;
    return a.size() < b.size() ? -1 : 1;
}

bool nameLess(const ChannelRecord &a, const ChannelRecord &b) {
    const int c = compareNameFolded(a.name, b.name);
    if (c != 0) return c < 0;
    return a.channelId < b.channelId;
}

bool latestLess(const ChannelRecord &a, const ChannelRecord &b) {
    const bool aKnown = a.latestUploadMs > 0, bKnown = b.latestUploadMs > 0;
    if (aKnown != bKnown) return aKnown;
    if (aKnown && a.latestUploadMs != b.latestUploadMs) return a.latestUploadMs > b.latestUploadMs;
    return nameLess(a, b);
}

// Lenient UTF-8 decoder: a malformed byte becomes its own code point (0xDC00 + byte)
// so that comparisons stay byte-faithful for broken input.
std::vector<std::uint32_t> decodeUtf8(const std::string &s) {
    std::vector<std::uint32_t> out;
    std::size_t i = 0;
    while (i < s.size()) {
        const unsigned char c = static_cast<unsigned char>(s[i]);
        int len = 0;
        std::uint32_t cp = 0;
        if (c < 0x80) { len = 1; cp = c; }
        else if ((c & 0xE0) == 0xC0) { len = 2; cp = c & 0x1F; }
        else if ((c & 0xF0) == 0xE0) { len = 3; cp = c & 0x0F; }
        else if ((c & 0xF8) == 0xF0) { len = 4; cp = c & 0x07; }
        bool ok = len > 0 && i + len <= s.size();
        for (int k = 1; ok && k < len; ++k) {
            const unsigned char d = static_cast<unsigned char>(s[i + k]);
            if ((d & 0xC0) != 0x80) ok = false;
            else cp = (cp << 6) | (d & 0x3F);
        }
        if (!ok) { out.push_back(0xDC00u + c); ++i; continue; }
        out.push_back(cp);
        i += len;
    }
    return out;
}

// Simple case folding for ASCII, Latin-1, Latin Extended-A, Greek and Cyrillic capitals.
std::uint32_t foldCodePoint(std::uint32_t c) {
    if (c >= 'A' && c <= 'Z') return c + 32;
    if (c < 0x80) return c;
    if (c >= 0xC0 && c <= 0xDE && c != 0xD7) return c + 32;
    if (c == 0x130) return 'i';
    if (c >= 0x100 && c <= 0x137) return c | 1u;
    if (c >= 0x139 && c <= 0x148) return (c & 1u) ? c + 1 : c;
    if (c >= 0x14A && c <= 0x177) return c | 1u;
    if (c == 0x178) return 0xFF;
    if (c >= 0x179 && c <= 0x17E) return (c & 1u) ? c + 1 : c;
    if (c >= 0x391 && c <= 0x3A9 && c != 0x3A2) return c + 32;
    if (c >= 0x410 && c <= 0x42F) return c + 32;
    if (c >= 0x400 && c <= 0x40F) return c + 80;
    return c;
}

} // namespace

bool reachesWatched(std::int64_t positionMs, std::int64_t durationMs) {
    if (durationMs <= 0 || positionMs <= 0) return false;
    if (positionMs * 5 >= durationMs * 4) return true; // 80 %, exact in integers
    // The end margin only makes sense for media longer than the margin itself:
    // otherwise any position of a 12 s clip would count as watched.
    return durationMs > limits::endMarginMs && durationMs - positionMs <= limits::endMarginMs;
}

HistoryRecord mergeProgress(const HistoryRecord *existing, const PlaybackProgress &progress,
                            std::int64_t nowMs, bool newSession) {
    HistoryRecord r;
    if (existing) {
        r = *existing;
        const VideoEntry &v = progress.video;
        if (!v.videoId.empty()) r.video.videoId = v.videoId;
        if (!v.title.empty()) r.video.title = v.title;
        if (!v.channel.empty()) r.video.channel = v.channel;
        if (!v.channelId.empty()) r.video.channelId = v.channelId;
        if (v.durationSeconds >= 0) r.video.durationSeconds = v.durationSeconds;
        if (!v.thumbnailUrl.empty()) r.video.thumbnailUrl = v.thumbnailUrl;
        if (newSession) r.playCount = std::max(0, r.playCount) + 1;
    } else {
        r.video = progress.video;
        r.playCount = 1;
    }
    r.positionMs = std::max<std::int64_t>(0, progress.positionMs);
    if (progress.durationMs > 0) r.durationMs = progress.durationMs;
    if (!progress.live && progress.durationMs > 0) {
        double f = double(r.positionMs) / double(progress.durationMs);
        if (progress.ended) f = 1.0;
        f = std::min(1.0, std::max(0.0, f));
        r.fraction = std::max(existing ? existing->fraction : 0.0, f);
        if (reachesWatched(r.positionMs, progress.durationMs)) r.watched = true;
    }
    if (progress.ended && !progress.live) r.watched = true;
    r.watchedAtMs = nowMs;
    return r;
}

HistoryRecord markWatched(const HistoryRecord &record, std::int64_t nowMs) {
    HistoryRecord r = record;
    if (r.durationMs <= 0 && r.video.durationSeconds > 0) r.durationMs = r.video.durationSeconds * 1000;
    r.fraction = 1.0;
    r.positionMs = std::max<std::int64_t>(0, r.durationMs);
    r.watched = true;
    r.watchedAtMs = nowMs;
    return r;
}

HistoryRecord unmarkWatched(const HistoryRecord &record) {
    HistoryRecord r = record;
    r.watched = false;
    r.fraction = 0;
    r.positionMs = 0;
    return r;
}

int clampHistoryCap(int requested) {
    if (requested <= 0) return limits::historyCapDefault;
    if (requested < limits::historyCapMinimum) return limits::historyCapMinimum;
    if (requested > limits::historyCapMaximum) return limits::historyCapMaximum;
    return requested;
}

bool validHistoryCap(int requested) {
    return requested >= limits::historyCapMinimum && requested <= limits::historyCapMaximum;
}

std::int64_t resumePositionToStore(const PlaybackProgress &progress) {
    if (progress.live || progress.ended || progress.durationMs <= 0) return -1;
    if (progress.positionMs < limits::resumeMinimumMs) return -1;
    if (progress.durationMs - progress.positionMs <= limits::endMarginMs) return -1;
    return progress.positionMs;
}

bool resumeEligibleForImport(std::int64_t positionMs, std::int64_t durationMs) {
    if (durationMs <= 0) return false;
    return positionMs > limits::resumeMinimumMs && durationMs - positionMs > limits::endMarginMs;
}

bool resumeExpired(const ResumeRecord &record, std::int64_t nowMs) {
    return nowMs - record.touchedMs > limits::resumeExpiryMs;
}

ResumeDecision decideResume(std::int64_t savedMs, ResumeBehaviour behaviour, std::int64_t linkStartMs) {
    ResumeDecision d;
    if (linkStartMs >= 0) {
        d.action = ResumeAction::StartAtLink;
        d.positionMs = linkStartMs;
        return d;
    }
    if (savedMs <= 0) return d;
    switch (behaviour) {
    case ResumeBehaviour::Always:
        d.action = ResumeAction::ResumeAt;
        d.positionMs = savedMs;
        break;
    case ResumeBehaviour::Ask:
        d.action = ResumeAction::AskThenResume;
        d.positionMs = savedMs;
        d.autoResumeSeconds = limits::resumeAskAutoSeconds;
        break;
    case ResumeBehaviour::Never:
        break;
    }
    return d;
}

OverlayFlags overlayFor(const WatchState &state) {
    OverlayFlags f;
    if (!state.known) return f;
    f.progress = std::min(1.0, std::max(0.0, state.fraction));
    f.showProgress = f.progress > limits::progressBarMinimumFraction;
    f.watchedBadge = state.watched;
    return f;
}

bool hideFromList(const WatchState &state, ListContext context, const HideWatchedSettings &settings) {
    if (!settings.enabled || !state.known || !state.watched) return false;
    switch (context) {
    case ListContext::Feed:
    case ListContext::Channel: return true;
    case ListContext::Search: return settings.includeSearch;
    case ListContext::Other: break;
    }
    return false;
}

void sortSubscriptions(std::vector<ChannelRecord> &channels, SubscriptionSort sort) {
    if (sort == SubscriptionSort::Name) std::sort(channels.begin(), channels.end(), nameLess);
    else std::sort(channels.begin(), channels.end(), latestLess);
}

std::string channelIdFromUrl(const std::string &url) {
    const UrlParts u = splitUrl(url);
    if (!u.ok || !hostIs(u.host, "youtube.com")) return std::string();
    const std::vector<std::string> seg = pathSegments(u.path);
    if (seg.size() < 2 || seg[0] != "channel") return std::string();
    return catalog::validChannelId(seg[1]) ? seg[1] : std::string();
}

bool isHandleUrl(const std::string &url) {
    const UrlParts u = splitUrl(url);
    if (!u.ok || !hostIs(u.host, "youtube.com")) return false;
    const std::vector<std::string> seg = pathSegments(u.path);
    if (seg.empty()) return false;
    if (seg[0].size() > 1 && seg[0][0] == '@') return true;
    return seg.size() >= 2 && (seg[0] == "c" || seg[0] == "user");
}

std::string videoIdFromUrl(const std::string &url) {
    const UrlParts u = splitUrl(url);
    if (!u.ok) return std::string();
    const std::vector<std::string> seg = pathSegments(u.path);
    if (hostIs(u.host, "youtu.be")) return seg.empty() ? std::string() : validVideoOrEmpty(seg[0]);
    if (!isYouTubeHost(u.host) || seg.empty()) return std::string();
    if (seg[0] == "watch") return validVideoOrEmpty(queryValue(u.query, "v"));
    if (seg.size() >= 2 && (seg[0] == "shorts" || seg[0] == "embed" || seg[0] == "live" || seg[0] == "v"))
        return validVideoOrEmpty(seg[1]);
    return std::string();
}

std::string playlistIdFromUrl(const std::string &url) {
    const std::string s = trimmed(url);
    std::string id;
    if (s.find("list=") != std::string::npos) {
        const std::size_t q = s.find('?');
        std::string query = q == std::string::npos ? s : s.substr(q + 1);
        const std::size_t fragment = query.find('#');
        if (fragment != std::string::npos) query.erase(fragment);
        id = queryValue(query, "list");
    } else if (s.find_first_of("/:?&=#") == std::string::npos) {
        id = s;
    }
    return validListId(id) ? id : std::string();
}

std::string localPlaylistId(std::uint64_t entropy) {
    const std::uint64_t v = (entropy ^ (entropy >> 48)) & 0xFFFFFFFFFFFFull;
    char buf[16];
    std::snprintf(buf, sizeof buf, "%012llx", static_cast<unsigned long long>(v));
    return std::string(buf);
}

std::string normalizePlaylistTitle(const std::string &title, const std::string &fallback) {
    std::string out;
    out.reserve(title.size());
    bool pendingSpace = false;
    int chars = 0;
    for (std::size_t i = 0; i < title.size(); ++i) {
        const unsigned char c = static_cast<unsigned char>(title[i]);
        if (isSpaceByte(c)) { pendingSpace = !out.empty(); continue; }
        const bool lead = (c & 0xC0) != 0x80;
        if (lead) {
            if (chars + (pendingSpace ? 1 : 0) >= limits::playlistTitleMaxChars) break;
            if (pendingSpace) { out += ' '; ++chars; pendingSpace = false; }
            ++chars;
        }
        out += char(c);
    }
    return out.empty() ? fallback : out;
}

bool sameListName(const std::string &a, const std::string &b) {
    const std::vector<std::uint32_t> x = decodeUtf8(normalizePlaylistTitle(a, std::string()));
    const std::vector<std::uint32_t> y = decodeUtf8(normalizePlaylistTitle(b, std::string()));
    if (x.size() != y.size()) return false;
    for (std::size_t i = 0; i < x.size(); ++i) {
        if (foldCodePoint(x[i]) != foldCodePoint(y[i])) return false;
    }
    return true;
}

bool loopEndReached(std::int64_t savedMs, std::int64_t lastPositionMs, std::int64_t durationMs) {
    if (savedMs >= 0 || durationMs <= 0) return false;
    const std::int64_t margin = std::max<std::int64_t>(2000, durationMs / 10);
    return lastPositionMs >= durationMs - margin;
}

}}}
