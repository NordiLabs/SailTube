// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "SharedStreamSourceLadder.h"

#include <QSet>
#include <algorithm>
#include <cmath>
#include <tuple>

namespace nordtile { namespace player {

using playback::StreamFormat;
using playback::StreamRequest;
using playback::StreamSet;

namespace {

int indexOf(const QStringList &order, const QString &family) {
    return family.isEmpty() ? -1 : order.indexOf(family);
}

QStringList families(const QStringList &tags) {
    QStringList out;
    for (const QString &t : tags) {
        const QString f = codecFamily(t);
        if (!f.isEmpty() && f != QLatin1String("none") && !out.contains(f)) out << f;
    }
    return out;
}

QString baseLanguage(const QString &tag) {
    int cut = -1;
    for (int i = 0; i < tag.size() && cut < 0; ++i)
        if (tag.at(i) == QLatin1Char('-') || tag.at(i) == QLatin1Char('_')) cut = i;
    return (cut < 0 ? tag : tag.left(cut)).toLower();
}

} // namespace

QString codecFamily(const QString &tag) {
    const QString t = tag.trimmed().toLower();
    const auto starts = [&t](const char *p) { return t.startsWith(QLatin1String(p)); };
    if (t == QLatin1String("none")) return QStringLiteral("none");
    if (starts("avc1") || starts("avc3") || t == QLatin1String("h264")) return QStringLiteral("h264");
    if (starts("hev1") || starts("hvc1") || t == QLatin1String("h265") || t == QLatin1String("hevc")) return QStringLiteral("hevc");
    if (starts("vp09") || t == QLatin1String("vp9") || starts("vp9.")) return QStringLiteral("vp9");
    if (starts("vp08") || t == QLatin1String("vp8")) return QStringLiteral("vp8");
    if (starts("av01") || t == QLatin1String("av1")) return QStringLiteral("av1");
    if (starts("mp4a") || t == QLatin1String("aac")) return QStringLiteral("aac");
    if (t == QLatin1String("opus")) return QStringLiteral("opus");
    if (t == QLatin1String("vorbis")) return QStringLiteral("vorbis");
    return QString();
}

QStringList videoCodecOrder(const StreamRequest &r) {
    if (!r.videoCodecs.isEmpty()) return families(r.videoCodecs);
    return r.hardwareDecode ? QStringList{QStringLiteral("vp9"), QStringLiteral("h264")}
                            : QStringList{QStringLiteral("h264"), QStringLiteral("vp9")};
}

QStringList audioCodecOrder(const StreamRequest &r) {
    if (!r.audioCodecs.isEmpty()) return families(r.audioCodecs);
    return QStringList{QStringLiteral("opus"), QStringLiteral("aac")};
}

namespace {

bool inCeiling(const StreamFormat &f, int maxHeight) { return maxHeight <= 0 || f.height <= maxHeight; }

// Smaller sorts first: at/below the ceiling tallest first, above it lowest first.
int heightRank(const StreamFormat &f, int maxHeight) {
    return inCeiling(f, maxHeight) ? -f.height : 100000 + f.height;
}

int languageRank(const StreamFormat &f, const QString &dub) {
    if (dub.isEmpty()) return 0;
    if (f.language.compare(dub, Qt::CaseInsensitive) == 0) return 0;
    if (!f.language.isEmpty() && baseLanguage(f.language) == baseLanguage(dub)) return 1;
    return 2;
}

} // namespace

void rankVideo(QList<StreamFormat> *list, const StreamRequest &r) {
    if (r.purpose != playback::StreamPurpose::Video) { list->clear(); return; }
    const QStringList order = videoCodecOrder(r);
    QList<StreamFormat> kept;
    for (const StreamFormat &f : *list)
        if (f.hasVideo() && !f.hasAudio() && !f.isHls && indexOf(order, codecFamily(f.videoCodec)) >= 0) kept << f;
    const int ceiling = r.maxHeight;
    const bool hfr = r.highFrameRate;
    std::stable_sort(kept.begin(), kept.end(), [&](const StreamFormat &x, const StreamFormat &y) {
        const auto key = [&](const StreamFormat &f) {
            const int fast = f.fps > 30.5 ? 1 : 0;
            return std::make_tuple(heightRank(f, ceiling), f.premium ? 1 : 0, hfr ? 1 - fast : fast,
                                   indexOf(order, codecFamily(f.videoCodec)), f.hdr ? 1 : 0, -f.bitrateKbps);
        };
        return key(x) < key(y);
    });
    list->swap(kept);
}

void keepAllowedAudio(QList<StreamFormat> *list, const StreamRequest &r) {
    const QStringList order = audioCodecOrder(r);
    QList<StreamFormat> kept;
    for (const StreamFormat &f : *list)
        if (f.hasAudio() && !f.hasVideo() && !f.isHls && indexOf(order, codecFamily(f.audioCodec)) >= 0) kept << f;
    list->swap(kept);
}

void rankAudio(QList<StreamFormat> *list, const StreamRequest &r) {
    keepAllowedAudio(list, r);
    const QStringList order = audioCodecOrder(r);
    std::stable_sort(list->begin(), list->end(), [&](const StreamFormat &x, const StreamFormat &y) {
        const auto key = [&](const StreamFormat &f) {
            return std::make_tuple(f.descriptive ? 1 : 0, languageRank(f, r.dubLanguage), -f.languagePreference,
                                   indexOf(order, codecFamily(f.audioCodec)), f.dynamicRangeCompressed ? 1 : 0,
                                   -f.bitrateKbps);
        };
        return key(x) < key(y);
    });
    QList<StreamFormat> unique;
    QSet<QString> seen;
    for (const StreamFormat &f : *list) {
        const QString id = codecFamily(f.audioCodec) + QLatin1Char('|') + f.language.toLower() + QLatin1Char('|')
            + QString::number(f.dynamicRangeCompressed ? 1 : 0) + QString::number(f.descriptive ? 1 : 0)
            + QLatin1Char('|') + QString::number(static_cast<qlonglong>(std::llround(f.bitrateKbps)));
        if (seen.contains(id)) continue;
        seen.insert(id);
        unique << f;
    }
    list->swap(unique);
}

void rankMuxed(QList<StreamFormat> *list, const StreamRequest &r) {
    const QStringList vOrder = videoCodecOrder(r);
    const QStringList aOrder = audioCodecOrder(r);
    QList<StreamFormat> kept;
    for (const StreamFormat &f : *list) {
        if (!f.hasVideo() || !f.hasAudio()) continue;
        const QString vf = codecFamily(f.videoCodec), af = codecFamily(f.audioCodec);
        if (indexOf(vOrder, vf) < 0 && vf != QLatin1String("h264")) continue; // muxed rungs are H.264 in practice
        if (indexOf(aOrder, af) < 0 && af != QLatin1String("aac")) continue;
        kept << f;
    }
    const int ceiling = r.maxHeight;
    std::stable_sort(kept.begin(), kept.end(), [&](const StreamFormat &x, const StreamFormat &y) {
        const auto key = [&](const StreamFormat &f) {
            return std::make_tuple(inCeiling(f, ceiling) ? 0 : 1, f.isHls ? 0 : 1, heightRank(f, ceiling));
        };
        return key(x) < key(y);
    });
    list->swap(kept);
}

bool playable(const StreamSet &s, const StreamRequest &r) {
    if (!s.hlsManifest.isEmpty() || !s.muxed.isEmpty()) return true;
    return hasSplitPair(s, r);
}

bool hasSplitPair(const StreamSet &s, const StreamRequest &r) {
    if (s.audio.isEmpty()) return false;
    return r.purpose != playback::StreamPurpose::Video || !s.video.isEmpty();
}

qint64 earliestExpiryMs(const StreamSet &s) {
    qint64 earliest = 0;
    const auto take = [&earliest](qint64 at) {
        if (at > 0 && (earliest == 0 || at < earliest)) earliest = at;
    };
    for (const QList<StreamFormat> *list : {&s.video, &s.audio, &s.muxed})
        for (const StreamFormat &f : *list) take(f.expiresAtUnixMs);
    if (!s.hlsManifest.isEmpty()) take(urlExpireUnixSeconds(s.hlsManifest.toEncoded()) * 1000);
    return earliest;
}

namespace {
bool parseDigits(const QByteArray &s, int begin, int end, qint64 *out) {
    if (begin >= end || end - begin > 18) return false;
    qint64 v = 0;
    for (int i = begin; i < end; ++i) {
        if (s[i] < '0' || s[i] > '9') return false;
        v = v * 10 + (s[i] - '0');
    }
    *out = v;
    return true;
}
}

qint64 urlQueryInteger(const QByteArray &url, const char *name) {
    const int query = url.indexOf('?');
    if (query < 0) return 0;
    const int fragment = url.indexOf('#', query);
    const int end = fragment < 0 ? url.size() : fragment;
    const QByteArray prefix = QByteArray(name) + '=';
    qint64 value = 0;
    int seen = 0;
    int pos = query + 1;
    while (pos <= end) {
        int amp = url.indexOf('&', pos);
        if (amp < 0 || amp > end) amp = end;
        if (url.mid(pos, prefix.size()) == prefix) {
            ++seen;
            if (!parseDigits(url, pos + prefix.size(), amp, &value)) return 0;
        }
        pos = amp + 1;
    }
    return seen == 1 ? value : 0;
}

qint64 urlExpireUnixSeconds(const QByteArray &url) {
    const qint64 q = urlQueryInteger(url, "expire");
    if (q > 0) return q;
    const int query = url.indexOf('?');
    const QByteArray path = query < 0 ? url : url.left(query);
    const QByteArray marker("/expire/");
    const int at = path.indexOf(marker);
    if (at < 0 || path.indexOf(marker, at + 1) >= 0) return 0;
    const int begin = at + marker.size();
    int stop = path.indexOf('/', begin);
    if (stop < 0) stop = path.size();
    qint64 v = 0;
    return parseDigits(path, begin, stop, &v) ? v : 0;
}

}}
