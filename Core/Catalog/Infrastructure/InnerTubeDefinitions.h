// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#pragma once

#include <QString>

namespace nordtile { namespace video { namespace catalog {

// NFR-MNT-04: every client definition and endpoint parameter the catalog sends lives in
// InnerTubeClient.cpp; the services name them through these (C arrays, so other files can use
// their addresses in static initialisers).
namespace innertube {
extern const char kEndpointBrowse[];       // youtubei/v1/browse
extern const char kEndpointNext[];         // youtubei/v1/next
extern const char kEndpointPlayer[];       // youtubei/v1/player
extern const char kEndpointSearch[];       // youtubei/v1/search
extern const char kEndpointResolveUrl[];   // youtubei/v1/navigation/resolve_url
extern const char kEndpointReelSequence[]; // youtubei/v1/reel/reel_watch_sequence (Shorts)
extern const char kEndpointReelItemWatch[]; // youtubei/v1/reel/reel_item_watch (one Short's details)
extern const char kEndpointVisitorId[];    // youtubei/v1/visitor_id (caption track list only, SEL-06)
extern const char kVisitorDataPath[];      // where an answer names a visitor id
extern const char kBrowseIdHome[];         // the recommended home (signed in)
extern const char kBrowseIdHashtagPrefix[];
extern const char kTabParamsVideos[];      // channel tab params (the channel's own are preferred)
extern const char kTabParamsShorts[];
extern const char kTabParamsStreams[];
extern const char kTabParamsPlaylists[];
extern const char kTabParamsFeatured[];
extern const char kSearchParamsShorts[];   // search type filter: Shorts
extern const char kWebClientVersion[];     // built-in WEB version (DSC-02 last source)
extern const char kClientVersionKey[];     // the homepage key it is scraped from
extern const char kSuggestQueryPrefix[];   // suggestqueries client parameters, then hl=
// "videos" / "shorts" / "streams" / "playlists" / "featured" -> params; "" for another tab.
QString channelTabParams(const QString &tab);
}

}}}
