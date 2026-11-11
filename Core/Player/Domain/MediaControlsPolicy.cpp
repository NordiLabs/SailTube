// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "MediaControlsPolicy.h"

namespace nordtile { namespace player {

MprisCapabilities mprisCapabilities(const MediaControlsState &s) {
    MprisCapabilities c;
    c.canControl = true;
    c.canPlay = s.hasSession || s.hasResumable;
    c.canPause = s.hasSession;
    c.canSeek = s.hasSession && s.seekable && !s.live;
    c.canGoNext = s.hasSession && s.canGoNext;
    c.canGoPrevious = s.hasSession && s.canGoPrevious;
    return c;
}

StopActions mprisStopActions(bool hasSession) {
    StopActions a;
    a.closeMiniPlayer = true;
    if (hasSession) {
        a.savePosition = true;
        a.endSession = true;
        a.releaseStreamJobs = true;
        a.releaseAudioResource = true;
    }
    return a;
}

MprisMetadata mprisMetadata(const TrackInfo &t) {
    MprisMetadata m;
    m.title = t.title;
    m.artist = t.channel;
    m.artUrl = localArtUrl(t.thumbnailPath);
    if (!t.live && t.durationMs > 0) {
        m.hasLength = true;
        m.lengthUs = t.durationMs * 1000;
    }
    return m;
}

static bool hasPrefixNoCase(const std::string &s, const char *prefix) {
    std::string::size_type i = 0;
    for (; prefix[i]; ++i) {
        if (i >= s.size()) return false;
        char a = s[i];
        if (a >= 'A' && a <= 'Z') a = static_cast<char>(a - 'A' + 'a');
        if (a != prefix[i]) return false;
    }
    return true;
}

std::string localArtUrl(const std::string &in) {
    std::string path;
    if (hasPrefixNoCase(in, "file://")) {
        path = in.substr(7);
        // Already a URL: keep its encoding, but it must name an absolute local path.
        if (path.empty() || path[0] != '/') return std::string();
        return "file://" + path;
    }
    if (in.empty() || in[0] != '/') return std::string(); // remote URLs, relative paths
    static const char hex[] = "0123456789ABCDEF";
    std::string out = "file://";
    for (std::string::size_type i = 0; i < in.size(); ++i) {
        unsigned char c = static_cast<unsigned char>(in[i]);
        bool plain = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-'
                || c == '.' || c == '_' || c == '~' || c == '/';
        if (plain) {
            out += static_cast<char>(c);
        } else {
            out += '%';
            out += hex[c >> 4];
            out += hex[c & 0x0F];
        }
    }
    return out;
}

CoverState coverState(const MediaControlsState &s) {
    CoverState c;
    if (s.hasSession) {
        c.mode = CoverMode::Session;
        c.left = CoverAction::PlayPause;
        c.right = s.canGoNext ? CoverAction::Next : CoverAction::Stop;
    } else if (s.hasResumable) {
        c.mode = CoverMode::ContinueWatching;
        c.left = CoverAction::ContinueWatching;
    }
    return c;
}

} } // namespace nordtile::player
