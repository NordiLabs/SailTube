// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#pragma once

// SailTube's format ladder over playback::StreamFormat (Shared/Playback/StreamSource.h), applied
// to every answer whatever path produced it, so that the yt-dlp fallback and SailTune's fast path
// rank alike: SEL-01 codec policy, SEL-02 video order, SEL-04 audio order. Qt Core only; no
// SailTube application types (namespace nordtile::player, pinnable by SailTune).

#include "../../../Shared/Playback/StreamSource.h"
#include <QList>
#include <QString>
#include <QStringList>

namespace nordtile { namespace player {

// Codec family of an RFC 6381 / yt-dlp codec tag or of a family name: "h264", "hevc", "vp9",
// "vp8", "av1", "aac", "opus", "vorbis", "none", or "" when unrecognised (never playable).
QString codecFamily(const QString &tag);

// SEL-01/SEL-02: allowed video families best first. request.videoCodecs when given (as families),
// else H.264 and VP9, VP9 first only with verified hardware decode. AV1 only when listed.
QStringList videoCodecOrder(const playback::StreamRequest &);
// SEL-04 key 2: request.audioCodecs when given (as families), else Opus then AAC.
QStringList audioCodecOrder(const playback::StreamRequest &);

// SEL-02 video ladder: keeps video-only, non-HLS formats of the allowed families (none for an
// audio-only request); at/below the ceiling tallest first, above it lowest first; at equal height
// non-Premium, then <= 30 fps (> 30 fps first only with request.highFrameRate), then codec order,
// then SDR, then bitrate.
void rankVideo(QList<playback::StreamFormat> *, const playback::StreamRequest &);

// SEL-04 audio ladder: keeps audio-only, non-HLS formats of the allowed families; descriptive
// last, then dub language (exact BCP-47, then base language; SEL-05), language preference, codec
// order, non-DRC, bitrate; deduplicated by (family, language, DRC, descriptive, rounded kbps).
void rankAudio(QList<playback::StreamFormat> *, const playback::StreamRequest &);

// Keeps the audio formats of the allowed families in their given order (for an answer that is
// already SEL-04 ranked by its producer).
void keepAllowedAudio(QList<playback::StreamFormat> *, const playback::StreamRequest &);

// Muxed last resort (INSTANT §4): H.264/AAC or allowed families; at/below the ceiling first, HLS
// variants before progressive, tallest first.
void rankMuxed(QList<playback::StreamFormat> *, const playback::StreamRequest &);

// Whether the answer can start playback for the request: audio plus (for video) video, or a
// muxed stream, or a live manifest.
bool playable(const playback::StreamSet &, const playback::StreamRequest &);

// Whether the answer holds a split pair: audio plus (for video) video-only (RES-08: a fast-path
// answer without one goes to the extractor).
bool hasSplitPair(const playback::StreamSet &, const playback::StreamRequest &);

// Earliest expiresAtUnixMs among the formats (and the HLS manifest's expire), 0 unknown.
qint64 earliestExpiryMs(const playback::StreamSet &);

// The URL's expire (Unix seconds): the "expire" query parameter when present exactly once, or an
// "/expire/<n>/" path segment as used by HLS manifest URLs. 0 when unknown.
qint64 urlExpireUnixSeconds(const QByteArray &encodedUrl);
// Integer value of a query parameter present exactly once ("clen"); 0 otherwise.
qint64 urlQueryInteger(const QByteArray &encodedUrl, const char *name);

}}
