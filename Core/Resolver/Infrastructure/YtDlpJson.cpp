// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "YtDlpJson.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonParseError>
#include <QList>
#include <QUrl>
#include <algorithm>
#include <cmath>

namespace nordtile { namespace video { namespace resolver {

using catalog::VideoDetails;
using catalog::VideoSummary;
using media::MediaFormat;

const char *errorLabel(ErrorCode code) {
    switch (code) {
    case ErrorCode::None: return "none";
    case ErrorCode::InvalidRequest: return "invalid_request";
    case ErrorCode::RuntimeUnavailable: return "runtime_unavailable";
    case ErrorCode::Busy: return "busy";
    case ErrorCode::Cancelled: return "cancelled";
    case ErrorCode::Timeout: return "timeout";
    case ErrorCode::Network: return "network";
    case ErrorCode::BotCheck: return "bot_check";
    case ErrorCode::AgeRestricted: return "age_restricted";
    case ErrorCode::PrivateVideo: return "private_video";
    case ErrorCode::VideoUnavailable: return "video_unavailable";
    case ErrorCode::Upcoming: return "upcoming";
    case ErrorCode::InvalidResponse: return "invalid_response";
    case ErrorCode::NoPlayableStream: return "no_playable_stream";
    case ErrorCode::Failed: return "failed";
    }
    return "unknown";
}

namespace {
const int maximumFormats = 512;
const int maximumChapters = 500;
const int maximumTags = 64;
const int maximumHeaders = 16;
const int maximumDescription = 16384;
const int maximumLabel = 512;
const int maximumStoryboardSheets = 2048;

std::string text(const QJsonObject &o, const char *key, int limit = maximumLabel) {
    const QJsonValue v = o.value(QLatin1String(key));
    if (!v.isString()) return std::string();
    return v.toString().left(limit).toStdString();
}

std::int64_t integer(const QJsonObject &o, const char *key, std::int64_t fallback = -1) {
    const QJsonValue v = o.value(QLatin1String(key));
    if (!v.isDouble()) return fallback;
    const double d = v.toDouble();
    if (!std::isfinite(d) || d < 0 || d > 9.0e15) return fallback;
    return static_cast<std::int64_t>(std::llround(d));
}

double number(const QJsonObject &o, const char *key) {
    const QJsonValue v = o.value(QLatin1String(key));
    if (!v.isDouble()) return 0;
    const double d = v.toDouble();
    return std::isfinite(d) && d > 0 && d < 1.0e9 ? d : 0;
}

bool httpsUrl(const QString &url) {
    return url.startsWith(QLatin1String("https://")) && url.size() <= 16384;
}

std::string thumbnail(const QJsonObject &o) {
    // Prefer a list entry close to list-row size; fall back to the single field.
    const QJsonArray list = o.value(QStringLiteral("thumbnails")).toArray();
    QString best;
    int bestWidth = 0;
    for (const QJsonValue &value : list) {
        const QJsonObject t = value.toObject();
        const QString url = t.value(QStringLiteral("url")).toString();
        if (!httpsUrl(url)) continue;
        const int width = t.value(QStringLiteral("width")).toInt();
        const bool better = best.isEmpty() || (width <= 720 && width > bestWidth) || (bestWidth > 720 && width < bestWidth);
        if (better) { best = url; bestWidth = width; }
    }
    if (best.isEmpty()) {
        const QString single = o.value(QStringLiteral("thumbnail")).toString();
        if (httpsUrl(single)) best = single;
    }
    return best.toStdString();
}

void fillSummary(const QJsonObject &o, VideoSummary *s) {
    s->videoId = text(o, "id", 32);
    s->title = text(o, "title");
    s->channel = text(o, "channel");
    if (s->channel.empty()) s->channel = text(o, "uploader");
    s->channelId = text(o, "channel_id", 32);
    if (!catalog::validChannelId(s->channelId)) s->channelId.clear();
    s->durationSeconds = integer(o, "duration");
    s->viewCount = integer(o, "view_count");
    s->thumbnailUrl = thumbnail(o);
    s->liveStatus = parseLiveStatus(o.value(QStringLiteral("live_status")).toString());
    if (s->liveStatus == media::LiveStatus::Unknown && o.value(QStringLiteral("is_live")).toBool())
        s->liveStatus = media::LiveStatus::IsLive;
}
}

bool acceptedMediaUrl(const QString &text, media::Protocol protocol) {
    if (!httpsUrl(text)) return false;
    const QUrl url(text, QUrl::StrictMode);
    if (!url.isValid() || url.scheme() != QLatin1String("https") || !url.userInfo().isEmpty()
            || (url.port() != -1 && url.port() != 443))
        return false;
    const QString host = url.host().toLower();
    const auto under = [&host](const char *domain) {
        const QString d = QLatin1String(domain);
        return host == d || host.endsWith(QLatin1Char('.') + d);
    };
    if (protocol == media::Protocol::Https)
        return under("googlevideo.com") && url.path() == QLatin1String("/videoplayback");
    if (protocol == media::Protocol::Hls)
        return under("googlevideo.com") || under("youtube.com");
    return false;
}

bool acceptedStoryboardUrl(const QString &text) {
    if (!httpsUrl(text)) return false;
    const QUrl url(text, QUrl::StrictMode);
    if (!url.isValid() || url.scheme() != QLatin1String("https") || !url.userInfo().isEmpty()
            || (url.port() != -1 && url.port() != 443))
        return false;
    return url.host().toLower().endsWith(QLatin1String(".ytimg.com")) && url.path().startsWith(QLatin1String("/sb/"));
}

StoryboardLevel parseStoryboard(const QJsonObject &o) {
    const bool named = o.value(QStringLiteral("format_note")).toString() == QLatin1String("storyboard")
                       || o.value(QStringLiteral("protocol")).toString() == QLatin1String("mhtml");
    if (!named) return StoryboardLevel();
    StoryboardLevel level;
    level.width = static_cast<int>(integer(o, "width", 0));
    level.height = static_cast<int>(integer(o, "height", 0));
    level.columns = static_cast<int>(integer(o, "columns", 0));
    level.rows = static_cast<int>(integer(o, "rows", 0));
    if (level.width < 16 || level.width > 1024 || level.height < 9 || level.height > 1024 || level.columns < 1
            || level.columns > 32 || level.rows < 1 || level.rows > 32)
        return StoryboardLevel();
    const QJsonArray fragments = o.value(QStringLiteral("fragments")).toArray();
    if (fragments.isEmpty() || fragments.size() > maximumStoryboardSheets) return StoryboardLevel();
    for (int i = 0; i < fragments.size(); ++i) {
        const QJsonObject f = fragments.at(i).toObject();
        StoryboardSheet sheet;
        const QString url = f.value(QStringLiteral("url")).toString();
        // Seconds, fractional (the video's duration over the sheet count).
        sheet.durationMs = static_cast<std::int64_t>(std::llround(number(f, "duration") * 1000.0));
        if (sheet.durationMs <= 0 && i == fragments.size() - 1 && i > 0) break;  // rounding left an empty last sheet
        if (sheet.durationMs <= 0 || !acceptedStoryboardUrl(url)) return StoryboardLevel();
        sheet.url = url.toStdString();
        level.sheets.push_back(sheet);
    }
    return level;
}

StoryboardLevel pickStoryboard(const std::vector<StoryboardLevel> &levels, int targetCellWidth) {
    const StoryboardLevel *best = nullptr;
    for (const StoryboardLevel &level : levels) {
        if (level.empty()) continue;
        if (!best) { best = &level; continue; }
        const bool enough = level.width >= targetCellWidth, bestEnough = best->width >= targetCellWidth;
        if (enough != bestEnough ? enough : (enough ? level.width < best->width : level.width > best->width)) best = &level;
    }
    return best ? *best : StoryboardLevel();
}

media::LiveStatus parseLiveStatus(const QString &s) {
    if (s == QLatin1String("not_live")) return media::LiveStatus::NotLive;
    if (s == QLatin1String("is_live")) return media::LiveStatus::IsLive;
    if (s == QLatin1String("was_live")) return media::LiveStatus::WasLive;
    if (s == QLatin1String("post_live")) return media::LiveStatus::PostLive;
    if (s == QLatin1String("is_upcoming")) return media::LiveStatus::Upcoming;
    return media::LiveStatus::Unknown;
}

MediaFormat parseFormat(const QJsonObject &o, const QJsonObject &fallbackHeaders) {
    MediaFormat f;
    f.id = text(o, "format_id", 64);
    f.container = text(o, "ext", 16);
    const QString protocol = o.value(QStringLiteral("protocol")).toString();
    f.protocol = protocol == QLatin1String("https") ? media::Protocol::Https
        : (protocol == QLatin1String("m3u8") || protocol == QLatin1String("m3u8_native")) ? media::Protocol::Hls
        : protocol == QLatin1String("http_dash_segments") ? media::Protocol::Dash
        : media::Protocol::Other;
    const QString url = o.value(QStringLiteral("url")).toString();
    if (acceptedMediaUrl(url, f.protocol)) f.url = url.toStdString();
    QJsonObject headers = o.value(QStringLiteral("http_headers")).toObject();
    if (headers.isEmpty()) headers = fallbackHeaders;
    int count = 0;
    for (auto it = headers.constBegin(); it != headers.constEnd() && count < maximumHeaders; ++it) {
        if (!it.value().isString() || it.key().size() > 128 || it.value().toString().size() > 1024) continue;
        f.headers[it.key().toStdString()] = it.value().toString().toStdString();
        ++count;
    }
    const QJsonValue vcodec = o.value(QStringLiteral("vcodec"));
    const QJsonValue acodec = o.value(QStringLiteral("acodec"));
    f.videoCodecTag = vcodec.isString() ? vcodec.toString().left(64).toStdString() : std::string();
    f.audioCodecTag = acodec.isString() ? acodec.toString().left(64).toStdString() : std::string();
    f.videoCodec = media::parseVideoCodec(f.videoCodecTag);
    f.audioCodec = media::parseAudioCodec(f.audioCodecTag);
    f.width = static_cast<int>(integer(o, "width", 0));
    f.height = static_cast<int>(integer(o, "height", 0));
    if (f.width > 16384 || f.height > 16384) f.width = f.height = 0;
    f.fps = number(o, "fps");
    f.totalKbps = number(o, "tbr");
    f.audioKbps = number(o, "abr");
    f.audioSampleRate = static_cast<int>(integer(o, "asr", 0));
    f.fileSize = integer(o, "filesize", 0);
    f.language = text(o, "language", 32);
    const QJsonValue preference = o.value(QStringLiteral("language_preference"));
    if (preference.isDouble()) f.languagePreference = preference.toInt();
    f.dynamicRangeCompressed = o.value(QStringLiteral("format_note")).toString().contains(QLatin1String("DRC"), Qt::CaseInsensitive);
    const QJsonValue range = o.value(QStringLiteral("dynamic_range"));
    f.highDynamicRange = range.isString() && range.toString() != QLatin1String("SDR");
    f.availableAtUnixSeconds = integer(o, "available_at", 0);
    return f;
}

std::vector<VideoSummary> parseListing(const QByteArray &output, int limit) {
    std::vector<VideoSummary> items;
    const QList<QByteArray> lines = output.split('\n');
    for (const QByteArray &line : lines) {
        if (static_cast<int>(items.size()) >= limit) break;
        if (line.trimmed().isEmpty() || line.size() > 256 * 1024) continue;
        QJsonParseError error;
        const QJsonDocument document = QJsonDocument::fromJson(line, &error);
        if (error.error != QJsonParseError::NoError || !document.isObject()) continue;
        VideoSummary s;
        fillSummary(document.object(), &s);
        if (!catalog::validVideoId(s.videoId) || s.title.empty()) continue;
        bool duplicate = false;
        for (const VideoSummary &existing : items) duplicate = duplicate || existing.videoId == s.videoId;
        if (!duplicate) items.push_back(s);
    }
    return items;
}

ErrorCode parseDetails(const QByteArray &output, const std::string &expectedVideoId, VideoDetails *details) {
    return parseDetails(output, expectedVideoId, details, nullptr);
}

ErrorCode parseDetails(const QByteArray &output, const std::string &expectedVideoId, VideoDetails *details,
                       DetailsExtras *extras) {
    QJsonParseError error;
    // --print emits one line; tolerate a trailing newline only.
    const QJsonDocument document = QJsonDocument::fromJson(output.trimmed(), &error);
    if (error.error != QJsonParseError::NoError || !document.isObject()) return ErrorCode::InvalidResponse;
    const QJsonObject o = document.object();
    VideoDetails d;
    fillSummary(o, &d.summary);
    if (d.summary.videoId != expectedVideoId) return ErrorCode::InvalidResponse;
    d.description = o.value(QStringLiteral("description")).toString().left(maximumDescription).toStdString();
    d.uploadDate = text(o, "upload_date", 8);
    d.likeCount = integer(o, "like_count");
    d.channelFollowerCount = integer(o, "channel_follower_count");
    const QString manifest = o.value(QStringLiteral("manifest_url")).toString();
    if (acceptedMediaUrl(manifest, media::Protocol::Hls)) d.manifestUrl = manifest.toStdString();
    for (const QJsonValue &value : o.value(QStringLiteral("chapters")).toArray()) {
        if (static_cast<int>(d.chapters.size()) >= maximumChapters) break;
        const QJsonObject c = value.toObject();
        catalog::Chapter chapter;
        chapter.startSeconds = c.value(QStringLiteral("start_time")).toDouble(-1);
        chapter.endSeconds = c.value(QStringLiteral("end_time")).toDouble(-1);
        chapter.title = text(c, "title");
        if (chapter.startSeconds >= 0 && chapter.endSeconds >= chapter.startSeconds) d.chapters.push_back(chapter);
    }
    for (const QJsonValue &value : o.value(QStringLiteral("tags")).toArray()) {
        if (static_cast<int>(d.tags.size()) >= maximumTags) break;
        if (value.isString()) d.tags.push_back(value.toString().left(128).toStdString());
    }
    const QJsonArray formats = o.value(QStringLiteral("formats")).toArray();
    if (formats.size() > maximumFormats) return ErrorCode::InvalidResponse;
    const QJsonObject topHeaders = o.value(QStringLiteral("http_headers")).toObject();
    int withoutUrl = 0;
    std::vector<StoryboardLevel> storyboards;
    for (const QJsonValue &value : formats) {
        if (!value.isObject()) continue;
        MediaFormat f = parseFormat(value.toObject(), topHeaders);
        if (!f.url.empty()) d.formats.push_back(f);
        // An audio or video format (not a storyboard) that was listed without a usable URL.
        else if (f.videoCodec != media::VideoCodec::None || f.audioCodec != media::AudioCodec::None) ++withoutUrl;
        else if (extras && storyboards.size() < 8) {
            const StoryboardLevel level = parseStoryboard(value.toObject());
            if (!level.empty()) storyboards.push_back(level);
        }
    }
    *details = d;
    if (extras) {
        DetailsExtras x;
        x.formatsWithoutUrl = withoutUrl;
        x.storyboard = pickStoryboard(storyboards, kStoryboardCellWidth);
        // RES-19: the scheduled start of an upcoming video (yt-dlp's release_timestamp, Unix s).
        if (d.summary.liveStatus == media::LiveStatus::Upcoming) x.scheduledStartUnix = std::max<std::int64_t>(0, integer(o, "release_timestamp", 0));
        *extras = x;
    }
    return ErrorCode::None;
}

namespace {
// Fixed phrases of the extractor's (and the provider's) error text, lower case. Provider text is
// only matched here, never forwarded.
struct Phrases {
    QString t;
    explicit Phrases(const QByteArray &diagnostics) : t(QString::fromUtf8(diagnostics).toLower()) {}
    bool has(const char *s) const { return t.contains(QLatin1String(s)); }
    bool storageLimit() const { return has("temporary storage limit"); }
    bool terminated() const {
        return has("been terminated") || has("closed their youtube account");
    }
    bool removed() const {
        return has("has been removed") || has("no longer available") || has("removed for violating");
    }
    bool geoBlocked() const {
        return has("not made this video available in your country") || has("not available in your country")
            || has("from your location");
    }
    bool privateVideo() const { return has("private video") || has("video is private"); }
    bool upcoming() const { return has("live event will begin") || has("premieres in") || has("this live event"); }
    bool membersOnly() const { return has("members-only") || has("join this channel") || has("members only"); }
    // HTTP 429 and the provider's "try again later" wording (RES-09: never final).
    bool rateLimited() const {
        return has("http error 429") || has("too many requests") || has("try again later") || has("rate-limited")
            || has("rate limited");
    }
    // SABR-only formats (no URL), failed player challenges and formats skipped for a missing PO
    // token: the answer had no usable URLs.
    bool deliveryChanged() const {
        return has("missing a url") || has("sabr") || has("n challenge solving failed")
            || has("signature solving failed") || has("nsig extraction failed") || has("signature extraction failed")
            || has("require a gvs po token");
    }
    bool requestedFormat() const { return has("requested format is not available"); }
    bool unavailable() const {
        // "Video unavailable" (web clients), "This video is unavailable" (app and TV clients).
        return !requestedFormat() && (has("video unavailable") || has("video is unavailable") || has("is not available")
                                      || has("isn't available"));
    }
};
}

ErrorCode classifyFailure(const QByteArray &diagnostics) {
    const Phrases p(diagnostics);
    const auto has = [&p](const char *s) { return p.has(s); };
    // The runtime's temporary-file budget is a failed extraction, not a missing runtime.
    if (p.storageLimit()) return ErrorCode::Failed;
    if (has("packaged resolver") || has("runtime dependency is unavailable") || has("unsupported python runtime"))
        return ErrorCode::RuntimeUnavailable;
    if (has("not a bot")) return ErrorCode::BotCheck;
    if (has("confirm your age") || has("age-restricted") || has("inappropriate for some users"))
        return ErrorCode::AgeRestricted;
    if (p.privateVideo()) return ErrorCode::PrivateVideo;
    if (p.upcoming()) return ErrorCode::Upcoming;
    // The final reasons are named before rate limiting: their text is the provider's verdict.
    if (p.removed() || p.terminated() || p.geoBlocked()) return ErrorCode::VideoUnavailable;
    // RES-09: rate limiting is worded as "unavailable" too; it is never final.
    if (p.rateLimited()) return ErrorCode::Network;
    if (p.unavailable()) return ErrorCode::VideoUnavailable;
    if (has("timed out") || has("unable to download") || has("name resolution") || has("network is unreachable")
            || has("connection refused") || has("connection reset") || has("urlopen error"))
        return ErrorCode::Network;
    return ErrorCode::Failed;
}

FailureReason classifyFailureReason(const QByteArray &diagnostics) {
    const Phrases p(diagnostics);
    if (p.storageLimit()) return FailureReason::StorageLimit;
    if (p.privateVideo()) return FailureReason::Private;
    if (p.upcoming()) return FailureReason::Upcoming;
    if (p.terminated()) return FailureReason::Terminated;
    if (p.removed()) return FailureReason::Removed;
    if (p.geoBlocked()) return FailureReason::GeoBlocked;
    if (p.membersOnly()) return FailureReason::MembersOnly;
    if (p.rateLimited()) return FailureReason::RateLimited;
    if (p.deliveryChanged()) return FailureReason::DeliveryChanged;
    if (p.unavailable()) return FailureReason::BareUnavailable;
    return FailureReason::None;
}

const char *failureReasonLabel(FailureReason reason) {
    switch (reason) {
    case FailureReason::None: return "none";
    case FailureReason::Removed: return "removed";
    case FailureReason::Terminated: return "terminated";
    case FailureReason::GeoBlocked: return "geo_blocked";
    case FailureReason::Private: return "private";
    case FailureReason::Upcoming: return "upcoming";
    case FailureReason::MembersOnly: return "members_only";
    case FailureReason::RateLimited: return "rate_limited";
    case FailureReason::StorageLimit: return "storage_limit";
    case FailureReason::DeliveryChanged: return "delivery_changed";
    case FailureReason::BareUnavailable: return "bare_unavailable";
    }
    return "none";
}

}}}
