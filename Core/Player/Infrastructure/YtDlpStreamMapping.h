// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#pragma once

// The Qt-Core half of the yt-dlp fallback adapter: maps core's VideoDetails into the shared
// playback::StreamSet (Shared/Playback/StreamSource.h). It names SailTube types
// (nordtile::video::...), so it lives in its own namespace, not nordtile::player. It fetches
// nothing and contains no YouTube protocol code. Ranking is player::rank* (SharedStreamSourceLadder),
// shared with SailTune's fast path.

#include "../../../Shared/Playback/StreamSource.h"
#include "../../Catalog/Domain/Video.h"
#include <QUrl>

namespace nordtile { namespace video { namespace playeradapter {

// One core format as a shared StreamFormat. contentLength comes from the URL's clen when core did
// not supply a size (-1 unknown), expiresAtUnixMs from the URL's expire. premium is core's flag
// (not parsed yet, docs/SHARED_REQUESTS.md R-CORE-1). originalAudio/languagePreference follow
// yt-dlp's language preference.
playback::StreamFormat toStreamFormat(const media::MediaFormat &);

// The ranked answer for a request (path Extractor, timings unset):
// - video: SEL-02 ladder (player::rankVideo); empty for an audio-only request;
// - audio: SEL-04 ladder (player::rankAudio);
// - muxed: last resort (player::rankMuxed), HLS variants before progressive;
// - live: liveState from core's live status, the HLS manifest only while live (RES-19).
// Formats that fail core's URL validation never reach VideoDetails; any non-https URL and any
// non-https/HLS protocol is dropped here too (RES-22).
playback::StreamSet toStreamSet(const catalog::VideoDetails &, const playback::StreamRequest &);

// Temporary direct hand-off: the remote https URL, which GStreamer's own HTTP source fetches with
// the format's headers (RES-16/RES-17, E5). Empty with error InvalidResponse for anything else.
QUrl directUrl(const playback::StreamFormat &, playback::StreamError *error);

}}}
