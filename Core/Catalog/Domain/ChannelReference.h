// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#pragma once

// Channel reference normalisation (DSC-12). Qt-free.
//
// Accepted inputs: a UC id; "@handle"; youtube.com URLs (any of www., m., music., with or
// without scheme) or paths of the forms /channel/UC..., /@handle, /c/name, /user/name and a
// bare /name custom URL. Query strings and fragments are dropped and tab suffixes
// (/videos, /shorts, /streams, /playlists, /releases, /featured, /community, /posts, /about,
// /podcasts, /live, /search) are stripped and reported separately.
//
// UC ids are used directly; every other kind is resolved through
// POST /youtubei/v1/navigation/resolve_url {url: resolveUrl()} -> endpoint.browseEndpoint.browseId.

#include <string>

namespace nordtile { namespace video { namespace catalog {

struct ChannelReference {
    enum class Kind { Invalid, ChannelId, Handle, Custom, User };
    ChannelReference() : kind(Kind::Invalid) {}
    Kind kind;
    std::string value;   // the UC id, "@handle", or the custom / user name
    std::string tab;     // the stripped tab suffix ("videos", "shorts", ...), or ""
    std::string path;    // the normalised path without the tab ("/@NASA", "/c/name", "/name")

    bool valid() const { return kind != Kind::Invalid; }
    bool needsResolve() const { return kind == Kind::Handle || kind == Kind::Custom || kind == Kind::User; }
    // "https://www.youtube.com/@NASA", ".../c/name", ".../user/name", ".../channel/UC..."
    std::string resolveUrl() const;
    // A stable cache key ("@nasa" handles compare case-insensitively).
    std::string key() const;
};

ChannelReference parseChannelReference(const std::string &input);

// "UC" + 22 characters of [A-Za-z0-9_-].
bool isChannelId(const std::string &id);

// True for a channel tab suffix that parseChannelReference strips.
bool isChannelTabSuffix(const std::string &segment);

}}}
