// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "ChannelReference.h"

#include <cctype>
#include <vector>

namespace nordtile { namespace video { namespace catalog {

namespace {

std::string lower(std::string s) {
    for (char &c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

std::string trim(const std::string &s) {
    std::size_t a = 0, b = s.size();
    while (a < b && std::isspace(static_cast<unsigned char>(s[a]))) ++a;
    while (b > a && std::isspace(static_cast<unsigned char>(s[b - 1]))) --b;
    return s.substr(a, b - a);
}

std::vector<std::string> splitPath(const std::string &path) {
    std::vector<std::string> out;
    std::string cur;
    for (char c : path) {
        if (c == '/') { if (!cur.empty()) out.push_back(cur); cur.clear(); }
        else cur += c;
    }
    if (!cur.empty()) out.push_back(cur);
    return out;
}

bool validName(const std::string &s) {
    if (s.empty()) return false;
    for (char c : s) if (c == ' ' || c == '?' || c == '#' || c == '/' || c == '"' || c == '<' || c == '>') return false;
    return true;
}

// Top-level paths that are YouTube pages, not custom channel URLs.
bool reservedTopLevel(const std::string &segment) {
    static const char *const reserved[] = {"watch", "shorts", "playlist", "results", "feed", "embed", "live", "v",
                                           "account", "signin", "premium", "gaming", "music", "redirect", "hashtag",
                                           "post", "attribution_link", "t", "s", "about", "channel", "c", "user"};
    const std::string l = lower(segment);
    for (const char *r : reserved) if (l == r) return true;
    return false;
}

}  // namespace

bool isChannelId(const std::string &id) {
    if (id.size() != 24 || id[0] != 'U' || id[1] != 'C') return false;
    for (std::size_t i = 2; i < id.size(); ++i) {
        const char c = id[i];
        const bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' || c == '_';
        if (!ok) return false;
    }
    return true;
}

bool isChannelTabSuffix(const std::string &segment) {
    static const char *const tabs[] = {"videos", "shorts", "streams", "playlists", "releases", "featured", "community",
                                       "posts", "about", "podcasts", "live", "search", "channels", "store", "courses"};
    const std::string l = lower(segment);
    for (const char *t : tabs) if (l == t) return true;
    return false;
}

std::string ChannelReference::resolveUrl() const {
    if (kind == Kind::Invalid) return std::string();
    return "https://www.youtube.com" + (path.empty() ? "/channel/" + value : path);
}

std::string ChannelReference::key() const {
    switch (kind) {
    case Kind::ChannelId: return value;
    case Kind::Handle: return lower(value);
    case Kind::Custom: return "c/" + lower(value);
    case Kind::User: return "user/" + lower(value);
    case Kind::Invalid: break;
    }
    return std::string();
}

ChannelReference parseChannelReference(const std::string &input) {
    ChannelReference ref;
    std::string s = trim(input);
    if (s.empty()) return ref;
    // Drop the query and fragment.
    const std::size_t cut = s.find_first_of("?#");
    if (cut != std::string::npos) s = s.substr(0, cut);
    if (isChannelId(s)) { ref.kind = ChannelReference::Kind::ChannelId; ref.value = s; ref.path = "/channel/" + s; return ref; }

    // Strip the scheme and a youtube.com host; anything else is not a channel reference.
    std::string path = s;
    const std::size_t scheme = path.find("://");
    if (scheme != std::string::npos) path = path.substr(scheme + 3);
    const std::string l = lower(path);
    bool hadHost = false;
    for (const char *host : {"www.youtube.com", "m.youtube.com", "music.youtube.com", "youtube.com"}) {
        const std::string h(host);
        if (l.compare(0, h.size(), h) == 0 && (l.size() == h.size() || l[h.size()] == '/')) {
            path = path.substr(h.size());
            hadHost = true;
            break;
        }
    }
    if (!hadHost && scheme != std::string::npos) return ref;
    if (!hadHost && !path.empty() && path[0] != '/' && path[0] != '@') return ref;   // a bare host with a path, or bare words

    std::vector<std::string> seg = splitPath(path);
    if (seg.empty()) return ref;
    // Tab suffix: the segment after the channel part.
    std::size_t nameParts = 1;
    const std::string first = lower(seg[0]);
    if (first == "channel" || first == "c" || first == "user") nameParts = 2;
    if (seg.size() < nameParts) return ref;
    if (seg.size() > nameParts) {
        if (!isChannelTabSuffix(seg[nameParts])) return ref;
        ref.tab = lower(seg[nameParts]);
        if (ref.tab == "live") ref.tab = "streams";
        if (ref.tab == "community") ref.tab = "posts";
    }

    if (first == "channel") {
        if (!isChannelId(seg[1])) return ChannelReference();
        ref.kind = ChannelReference::Kind::ChannelId;
        ref.value = seg[1];
        ref.path = "/channel/" + ref.value;
    } else if (first == "c" || first == "user") {
        if (!validName(seg[1])) return ChannelReference();
        ref.kind = first == "c" ? ChannelReference::Kind::Custom : ChannelReference::Kind::User;
        ref.value = seg[1];
        ref.path = "/" + first + "/" + ref.value;
    } else if (seg[0][0] == '@') {
        if (seg[0].size() < 2 || !validName(seg[0])) return ChannelReference();
        ref.kind = ChannelReference::Kind::Handle;
        ref.value = seg[0];
        ref.path = "/" + ref.value;
    } else {
        // youtube.com/<name>: a legacy custom URL, resolved like /c/<name>.
        if (!hadHost || reservedTopLevel(seg[0]) || !validName(seg[0])) return ChannelReference();
        ref.kind = ChannelReference::Kind::Custom;
        ref.value = seg[0];
        ref.path = "/" + ref.value;
    }
    return ref;
}

}}}
