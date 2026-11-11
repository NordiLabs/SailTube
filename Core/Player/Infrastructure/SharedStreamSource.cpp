// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Non-template helpers of SharedStreamSource; see SharedStreamSource.h.
#include "SharedStreamSource.h"

namespace nordtile { namespace player { namespace sharedsource {

bool validVideoId(const QString &id) {
    if (id.size() != 11) return false;
    for (const QChar c : id) {
        const ushort u = c.unicode();
        const bool ok = (u >= 'A' && u <= 'Z') || (u >= 'a' && u <= 'z') || (u >= '0' && u <= '9') || u == '_' || u == '-';
        if (!ok) return false;
    }
    return true;
}

// The same type on both paths, so that MediaProxy's identity check on refresh (same length and
// type) holds when a refreshed address comes from the other path.
QString mimeTypeOf(const playback::StreamFormat &f) {
    const QString kind = f.hasVideo() ? QStringLiteral("video/") : QStringLiteral("audio/");
    return kind + (f.container.contains(QLatin1String("webm"), Qt::CaseInsensitive) ? QStringLiteral("webm") : QStringLiteral("mp4"));
}

QByteArray headerValue(const playback::StreamFormat &f, const char *name) {
    const QByteArray wanted = QByteArray(name).toLower();
    for (const auto &h : f.headers)
        if (h.first.toLower() == wanted) return h.second;
    return QByteArray();
}

std::vector<std::string> componentAudioCodecs(const playback::StreamRequest &r) {
    std::vector<std::string> out;
    if (r.audioCodecs.isEmpty()) return out; // the component's default: Opus, then AAC
    for (const QString &family : audioCodecOrder(r)) out.push_back(family.toStdString());
    return out;
}

QUrl directRemoteUrl(const playback::StreamFormat &f, playback::StreamError *error) {
    if (!f.url.isValid() || f.url.scheme() != QLatin1String("https")) {
        if (error) *error = playback::streamError(playback::StreamErrorClass::InvalidResponse, false);
        return QUrl();
    }
    if (error) *error = playback::StreamError();
    return f.url;
}


}}}
