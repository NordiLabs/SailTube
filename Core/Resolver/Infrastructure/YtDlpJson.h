// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#pragma once

// Bounded parsers for the JSON that yt-dlp prints with --print "%(...)j"
// (field names from yt-dlp's documented output template). Provider text never
// reaches error values: failures carry a code only, presentation maps it.

#include "../../Catalog/Domain/Video.h"
#include <QByteArray>
#include <QJsonObject>
#include <QString>
#include <vector>

namespace nordtile { namespace video { namespace resolver {

enum class ErrorCode {
    None, InvalidRequest, RuntimeUnavailable, Busy, Cancelled, Timeout, Network,
    BotCheck, AgeRestricted, PrivateVideo, VideoUnavailable, Upcoming,
    InvalidResponse, NoPlayableStream, Failed
};

const char *errorLabel(ErrorCode);

media::LiveStatus parseLiveStatus(const QString &);
// Stream URL allow-list (RES-22): direct media only from https *.googlevideo.com
// /videoplayback; HLS playlists from https *.googlevideo.com or *.youtube.com.
bool acceptedMediaUrl(const QString &url, media::Protocol protocol);
media::MediaFormat parseFormat(const QJsonObject &format, const QJsonObject &fallbackHeaders);

// One JSON object per line (flat search/channel listing). Invalid lines are skipped.
std::vector<catalog::VideoSummary> parseListing(const QByteArray &output, int limit);

// Full extraction of one video. The id must match the requested one.
ErrorCode parseDetails(const QByteArray &output, const std::string &expectedVideoId,
                       catalog::VideoDetails *details);

// Scrub-preview storyboard (yt-dlp's storyboard formats sb0..sb3, format_note "storyboard"):
// sprite sheets that cover the video in order. Each sheet holds columns x rows cells of
// width x height pixels, left to right then top to bottom; the last sheet may hold fewer.
struct StoryboardSheet {
    std::string url;                // https *.ytimg.com/sb/... (acceptedStoryboardUrl)
    std::int64_t durationMs = 0;
};
struct StoryboardLevel {
    int width = 0, height = 0;      // one cell, pixels
    int columns = 0, rows = 0;      // cells per sheet
    std::vector<StoryboardSheet> sheets;
    bool empty() const { return sheets.empty(); }
};
// The cell width the scrub preview asks for (the seek bar's bubble shows one cell).
const int kStoryboardCellWidth = 160;
// Storyboard images only from https *.ytimg.com under /sb/ (the artwork host the image cache
// already fetches thumbnails from).
bool acceptedStoryboardUrl(const QString &url);
// One entry of `formats`. An empty level when it is not a storyboard or is malformed: a cell or
// grid size out of range, no sheets or more than 2048, a sheet outside the allow-list or without
// a duration (a zero-length last sheet is dropped, not an error).
StoryboardLevel parseStoryboard(const QJsonObject &format);
// The level for a preview about `targetCellWidth` pixels wide: the narrowest level at least that
// wide, else the widest. An empty level when there is none.
StoryboardLevel pickStoryboard(const std::vector<StoryboardLevel> &levels, int targetCellWidth);

// What a details answer says besides VideoDetails (RES-19).
struct DetailsExtras {
    std::int64_t scheduledStartUnix = 0;  // release_timestamp of an upcoming video, 0 unknown
    int formatsWithoutUrl = 0;            // listed audio / video formats dropped for lack of an accepted URL
    StoryboardLevel storyboard;           // the scrub-preview level (kStoryboardCellWidth), empty when none
};
// As above; extras (may be null) gets the fields VideoDetails does not carry.
ErrorCode parseDetails(const QByteArray &output, const std::string &expectedVideoId,
                       catalog::VideoDetails *details, DetailsExtras *extras);

// Classifies a failed run from its stderr. Never returns provider text. Rate limiting (HTTP 429,
// "try again later") is Network, never VideoUnavailable (RES-09); the runtime's temporary-file
// budget is Failed (a retryable extraction failure), not RuntimeUnavailable.
ErrorCode classifyFailure(const QByteArray &diagnostics);

// RES-09: the finer reason behind a run's ErrorCode, from the same stderr (warnings included).
// Fixed values, never provider text. Bare: "video unavailable" without one of the final reasons,
// which one client may be told where another is not (not final). DeliveryChanged: the provider
// withheld usable URLs (SABR-only formats, a failed n / signature challenge, formats skipped for a
// missing PO token).
enum class FailureReason {
    None, Removed, Terminated, GeoBlocked, Private, Upcoming, MembersOnly, RateLimited,
    StorageLimit, DeliveryChanged, BareUnavailable
};
FailureReason classifyFailureReason(const QByteArray &diagnostics);
const char *failureReasonLabel(FailureReason);

}}}
