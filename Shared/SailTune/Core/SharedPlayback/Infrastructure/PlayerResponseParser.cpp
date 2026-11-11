// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "PlayerResponseParser.h"
#include "../Domain/FormatLadder.h"
#include "../Domain/Playability.h"
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QString>
#include <QUrl>
#include <QUrlQuery>
#include <cmath>

namespace nordtile { namespace music { namespace sharedplayback { namespace infrastructure {
namespace {
using namespace domain;

std::string str(const QJsonValue &value, int maxLength = 512) {
    const QString text = value.toString().left(maxLength);
    const QByteArray bytes = text.toUtf8();
    return std::string(bytes.constData(), std::size_t(bytes.size()));
}

// "audio/webm; codecs=\"opus\"" -> mime "audio/webm", codec "opus".
void splitMime(const QString &mimeType, StreamFormat *format) {
    const int semi = mimeType.indexOf(QLatin1Char(';'));
    format->mimeType = mimeType.left(semi < 0 ? mimeType.size() : semi).trimmed().toLower().toStdString();
    if (semi < 0) return;
    const QString rest = mimeType.mid(semi + 1);
    const int quote = rest.indexOf(QLatin1Char('"'));
    const int end = rest.lastIndexOf(QLatin1Char('"'));
    if (quote >= 0 && end > quote) {
        QString codecs = rest.mid(quote + 1, end - quote - 1);
        // A progressive stream lists "video, audio"; keep both, the family comes from the first.
        format->codec = codecs.split(QLatin1Char(','), QString::SkipEmptyParts).value(0).trimmed().toLower().toStdString();
    }
}

bool mediaHost(const QUrl &url) {
    return url.isValid() && url.scheme() == QLatin1String("https") && url.userInfo().isEmpty() && !url.hasFragment() &&
           url.port(443) == 443 && url.host().toLower().endsWith(QLatin1String(".googlevideo.com"));
}

std::int64_t expiryOf(const QUrl &url) {
    bool ok = false;
    const qlonglong seconds = QUrlQuery(url).queryItemValue(QStringLiteral("expire")).toLongLong(&ok);
    return ok && seconds > 0 ? std::int64_t(seconds) * 1000 : 0;
}

bool readFormat(const QJsonObject &object, StreamFormat *format) {
    if (object.contains(QStringLiteral("signatureCipher")) || object.contains(QStringLiteral("cipher")) ||
        object.contains(QStringLiteral("drmFamilies")) || object.contains(QStringLiteral("targetDurationSec")) ||
        object.value(QStringLiteral("type")).toString() == QLatin1String("FORMAT_STREAM_TYPE_OTF")) return false;
    const QString address = object.value(QStringLiteral("url")).toString();
    if (address.isEmpty() || address.size() > 16384) return false;
    const QUrl url(address, QUrl::StrictMode);
    if (!mediaHost(url)) return false;
    format->itag = object.value(QStringLiteral("itag")).toInt();
    format->url = address.toStdString();
    splitMime(object.value(QStringLiteral("mimeType")).toString(), format);
    format->hasAudio = format->mimeType.compare(0, 6, "audio/") == 0;
    format->hasVideo = format->mimeType.compare(0, 6, "video/") == 0;
    if (format->hasVideo) {
        // A progressive stream carries both: it lists an audio codec and an audio quality.
        if (object.contains(QStringLiteral("audioQuality")) || object.contains(QStringLiteral("audioSampleRate")))
            format->hasAudio = true;
    }
    if (!format->hasAudio && !format->hasVideo) return false;
    format->bitrate = object.value(QStringLiteral("averageBitrate")).toInt(object.value(QStringLiteral("bitrate")).toInt());
    format->contentLength = object.value(QStringLiteral("contentLength")).toString().toLongLong();
    format->width = object.value(QStringLiteral("width")).toInt();
    format->height = object.value(QStringLiteral("height")).toInt();
    format->fps = object.value(QStringLiteral("fps")).toInt();
    format->isDrc = object.value(QStringLiteral("isDrc")).toBool(false);
    const QJsonObject track = object.value(QStringLiteral("audioTrack")).toObject();
    if (!track.isEmpty()) {
        const QString id = track.value(QStringLiteral("id")).toString();
        const int dot = id.indexOf(QLatin1Char('.'));
        format->audioLanguage = (dot > 0 ? id.left(dot) : id).left(16).toLower().toStdString();
        format->isDefaultAudio = track.value(QStringLiteral("audioIsDefault")).toBool(true);
    }
    const QString xtags = object.value(QStringLiteral("xtags")).toString();
    // The tags are base64 of key=value pairs in some answers; the visible form carries "descriptive".
    format->isDescriptive = xtags.contains(QLatin1String("descriptive"), Qt::CaseInsensitive) ||
        str(track.value(QStringLiteral("displayName"))).find("escriptive") != std::string::npos;
    format->expiresAtUnixMs = expiryOf(url);
    return true;
}

std::string errorScreenReason(const QJsonObject &playability) {
    const QJsonObject renderer = playability.value(QStringLiteral("errorScreen")).toObject()
        .value(QStringLiteral("playerErrorMessageRenderer")).toObject();
    const QJsonObject reason = renderer.value(QStringLiteral("reason")).toObject();
    std::string text = str(reason.value(QStringLiteral("simpleText")), 400);
    if (text.empty()) {
        const QJsonArray runs = reason.value(QStringLiteral("runs")).toArray();
        for (int i = 0; i < runs.size() && i < 8; ++i) text += str(runs.at(i).toObject().value(QStringLiteral("text")), 200);
    }
    return text;
}
}  // namespace

ParsedPlayer parsePlayerResponse(const QByteArray &body, const std::string &videoId,
                                 const StreamRequest &request, std::int64_t nowUnixMs) {
    ParsedPlayer parsed;
    QJsonParseError error;
    const QJsonDocument document = QJsonDocument::fromJson(body, &error);
    if (error.error != QJsonParseError::NoError || !document.isObject()) {
        parsed.failure = FailureKind::BadResponse; parsed.reason = "not_json"; return parsed;
    }
    const QJsonObject root = document.object();
    const QJsonObject playability = root.value(QStringLiteral("playabilityStatus")).toObject();
    const std::string status = str(playability.value(QStringLiteral("status")), 32);
    if (status.empty()) { parsed.failure = FailureKind::BadResponse; parsed.reason = "no_playability"; return parsed; }
    std::string reason = str(playability.value(QStringLiteral("reason")), 400);
    if (reason.empty()) reason = errorScreenReason(playability);
    const FailureKind verdict = classifyPlayability(status, reason);
    if (verdict != FailureKind::None) {
        parsed.failure = verdict;
        parsed.reason = verdict == FailureKind::BotWall ? "bot_wall" : verdict == FailureKind::Final ? "final"
            : status == "LOGIN_REQUIRED" ? "login_required" : status == "UNPLAYABLE" ? "unplayable" : "not_ok";
        return parsed;
    }
    const QJsonObject details = root.value(QStringLiteral("videoDetails")).toObject();
    if (str(details.value(QStringLiteral("videoId")), 16) != videoId) {
        parsed.failure = FailureKind::BadResponse; parsed.reason = "other_video"; return parsed;
    }
    parsed.set.videoId = videoId;
    parsed.set.metadata.title = str(details.value(QStringLiteral("title")), 300);
    parsed.set.metadata.author = str(details.value(QStringLiteral("author")), 300);
    parsed.set.metadata.isLive = details.value(QStringLiteral("isLive")).toBool(false) ||
                                 details.value(QStringLiteral("isUpcoming")).toBool(false);
    const double seconds = details.value(QStringLiteral("lengthSeconds")).toString().toDouble();
    if (std::isfinite(seconds) && seconds > 0 && seconds < 365.0 * 24 * 3600) parsed.set.metadata.durationMs = std::int64_t(seconds * 1000);
    {
        const QJsonArray thumbs = details.value(QStringLiteral("thumbnail")).toObject().value(QStringLiteral("thumbnails")).toArray();
        int best = -1;
        for (int i = 0; i < thumbs.size() && i < 32; ++i) {
            const QJsonObject t = thumbs.at(i).toObject();
            const int width = t.value(QStringLiteral("width")).toInt();
            const std::string url = str(t.value(QStringLiteral("url")), 1024);
            if (!url.empty() && width > best) { best = width; parsed.set.metadata.thumbnailUrl = url; }
        }
    }
    if (parsed.set.metadata.isLive) { parsed.failure = FailureKind::Unplayable; parsed.reason = "live"; return parsed; }

    const QJsonObject streaming = root.value(QStringLiteral("streamingData")).toObject();
    std::vector<StreamFormat> formats;
    const QJsonArray lists[2] = {streaming.value(QStringLiteral("adaptiveFormats")).toArray(),
                                 streaming.value(QStringLiteral("formats")).toArray()};
    for (int l = 0; l < 2; ++l) {
        for (int i = 0; i < lists[l].size() && i < 256; ++i) {
            StreamFormat format;
            if (readFormat(lists[l].at(i).toObject(), &format)) { formats.push_back(format); ++parsed.keptFormats; }
            else ++parsed.droppedFormats;
        }
    }
    parsed.set.audio = rankAudio(formats, request);
    if (!request.audioOnly) parsed.set.video = rankVideo(formats, request);
    parsed.set.muxed = rankMuxed(formats);
    parsed.set.hlsUrl = str(streaming.value(QStringLiteral("hlsManifestUrl")), 2048);
    // No audio-only address: a progressive stream (video and audio together) is the last resort for an audio request.
    if (parsed.set.audio.empty() && (request.audioOnly ? parsed.set.muxed.empty() : true)) {
        parsed.failure = FailureKind::NoUsableFormat; parsed.reason = "no_direct_audio"; return parsed;
    }
    if (!request.audioOnly && parsed.set.video.empty()) { parsed.failure = FailureKind::NoUsableFormat; parsed.reason = "no_direct_video"; return parsed; }

    // Earliest expiry among what is offered; else the declared lifetime.
    std::int64_t earliest = 0;
    const std::vector<StreamFormat> *kept[3] = {&parsed.set.audio, &parsed.set.video, &parsed.set.muxed};
    for (int k = 0; k < 3; ++k)
        for (std::size_t i = 0; i < kept[k]->size(); ++i) {
            const std::int64_t at = (*kept[k])[i].expiresAtUnixMs;
            if (at > 0 && (earliest == 0 || at < earliest)) earliest = at;
        }
    if (earliest == 0) {
        const qlonglong life = streaming.value(QStringLiteral("expiresInSeconds")).toString().toLongLong();
        if (life > 0) earliest = nowUnixMs + std::int64_t(life) * 1000;
    }
    parsed.set.expiresAtUnixMs = earliest;

    const QJsonObject audioConfig = root.value(QStringLiteral("playerConfig")).toObject().value(QStringLiteral("audioConfig")).toObject();
    if (audioConfig.contains(QStringLiteral("loudnessDb"))) {
        parsed.set.hasLoudness = true; parsed.set.loudnessDb = audioConfig.value(QStringLiteral("loudnessDb")).toDouble();
    } else if (audioConfig.contains(QStringLiteral("perceptualLoudnessDb")) && audioConfig.contains(QStringLiteral("loudnessTargetLkfs"))) {
        // This client omits loudnessDb; the difference to the target is the same quantity.
        parsed.set.hasLoudness = true;
        parsed.set.loudnessDb = audioConfig.value(QStringLiteral("perceptualLoudnessDb")).toDouble() -
                                audioConfig.value(QStringLiteral("loudnessTargetLkfs")).toDouble();
    }
    if (parsed.set.hasLoudness && !std::isfinite(parsed.set.loudnessDb)) parsed.set.hasLoudness = false;
    return parsed;
}

}}}}
