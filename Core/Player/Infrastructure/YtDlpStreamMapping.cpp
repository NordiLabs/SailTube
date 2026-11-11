// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// yt-dlp fallback adapter, mapping half; see YtDlpStreamMapping.h.
#include "YtDlpStreamMapping.h"
#include "SharedStreamSourceLadder.h"

namespace nordtile { namespace video { namespace playeradapter {

using playback::StreamFormat;
using playback::StreamRequest;
using playback::StreamSet;

namespace {

playback::LiveState liveState(media::LiveStatus s) {
    switch (s) {
    case media::LiveStatus::IsLive: return playback::LiveState::Live;
    case media::LiveStatus::WasLive: return playback::LiveState::WasLive;
    case media::LiveStatus::PostLive: return playback::LiveState::PostLive;
    case media::LiveStatus::Upcoming: return playback::LiveState::Upcoming;
    case media::LiveStatus::NotLive: case media::LiveStatus::Unknown: break;
    }
    return playback::LiveState::NotLive;
}

bool httpsUrl(const std::string &url) { return url.compare(0, 8, "https://") == 0; }

QString qs(const std::string &s) { return QString::fromStdString(s); }

} // namespace

StreamFormat toStreamFormat(const media::MediaFormat &m) {
    StreamFormat f;
    const QByteArray encoded = QByteArray::fromStdString(m.url);
    f.formatId = qs(m.id);
    f.url = QUrl::fromEncoded(encoded, QUrl::StrictMode);
    f.container = qs(m.container);
    f.videoCodec = m.videoCodec == media::VideoCodec::None ? QStringLiteral("none") : qs(m.videoCodecTag);
    f.audioCodec = m.audioCodec == media::AudioCodec::None ? QStringLiteral("none") : qs(m.audioCodecTag);
    f.width = m.width;
    f.height = m.height;
    f.fps = m.fps;
    f.bitrateKbps = m.totalKbps > 0 ? m.totalKbps : m.audioKbps;
    f.audioSampleRate = m.audioSampleRate;
    const qint64 clen = player::urlQueryInteger(encoded, "clen");
    f.contentLength = m.fileSize > 0 ? m.fileSize : clen > 0 ? clen : -1;
    f.language = qs(m.language);
    f.languagePreference = m.languagePreference;
    f.originalAudio = m.audioCodec != media::AudioCodec::None && m.languagePreference > 0;
    f.dynamicRangeCompressed = m.dynamicRangeCompressed;
    f.premium = m.premium; // not parsed by core yet (docs/SHARED_REQUESTS.md R-CORE-1)
    f.hdr = m.highDynamicRange;
    f.isHls = m.protocol == media::Protocol::Hls;
    for (const auto &h : m.headers) f.headers << qMakePair(QByteArray::fromStdString(h.first), QByteArray::fromStdString(h.second));
    f.expiresAtUnixMs = player::urlExpireUnixSeconds(encoded) * 1000;
    return f;
}

StreamSet toStreamSet(const catalog::VideoDetails &d, const StreamRequest &r) {
    StreamSet s;
    s.videoId = qs(d.summary.videoId);
    s.liveState = liveState(d.summary.liveStatus);
    s.live = s.liveState == playback::LiveState::Live;
    s.path = playback::ResolvePath::Extractor;
    if (s.live && httpsUrl(d.manifestUrl)) s.hlsManifest = QUrl::fromEncoded(QByteArray::fromStdString(d.manifestUrl), QUrl::StrictMode);

    for (const media::MediaFormat &m : d.formats) {
        if (!httpsUrl(m.url)) continue;
        if (m.protocol != media::Protocol::Https && m.protocol != media::Protocol::Hls) continue;
        const StreamFormat f = toStreamFormat(m);
        if (!f.url.isValid()) continue;
        if (f.hasVideo() && f.hasAudio()) s.muxed << f;
        else if (f.hasVideo()) s.video << f;
        else if (f.hasAudio()) s.audio << f;
    }
    player::rankVideo(&s.video, r);
    player::rankAudio(&s.audio, r);
    player::rankMuxed(&s.muxed, r);
    s.expiresAtUnixMs = player::earliestExpiryMs(s);
    return s;
}

QUrl directUrl(const StreamFormat &f, playback::StreamError *error) {
    if (f.url.scheme() != QLatin1String("https") || !f.url.isValid()) {
        if (error) *error = playback::streamError(playback::StreamErrorClass::InvalidResponse, false);
        return QUrl();
    }
    if (error) *error = playback::StreamError();
    return f.url; // HLS manifests too: they are never proxied (RES-17)
}

}}}
