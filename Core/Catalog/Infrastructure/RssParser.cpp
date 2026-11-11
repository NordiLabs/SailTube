// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "RssParser.h"

#include "Core/Catalog/Domain/Images.h"

#include <QDateTime>
#include <QUrlQuery>
#include <QXmlStreamReader>

namespace nordtile { namespace video { namespace catalog {

namespace {

const QLatin1String kAtom("http://www.w3.org/2005/Atom");
const QLatin1String kYt("http://www.youtube.com/xml/schemas/2015");
const QLatin1String kMedia("http://search.yahoo.com/mrss/");

std::int64_t parseIsoDate(const QString &text) {
    QDateTime dt = QDateTime::fromString(text.trimmed(), Qt::ISODate);
    if (!dt.isValid()) return -1;
    return dt.toMSecsSinceEpoch() / 1000;
}

bool plausibleVideoId(const QString &id) {
    if (id.size() != 11) return false;
    for (const QChar c : id) {
        const ushort u = c.unicode();
        const bool ok = (u >= 'A' && u <= 'Z') || (u >= 'a' && u <= 'z') || (u >= '0' && u <= '9') || u == '_' || u == '-';
        if (!ok) return false;
    }
    return true;
}

}  // namespace

QUrl RssParser::feedUrl(const QString &channelId) {
    QUrl url(QStringLiteral("https://www.youtube.com/feeds/videos.xml"));
    QUrlQuery q;
    q.addQueryItem(QStringLiteral("channel_id"), channelId);
    url.setQuery(q);
    return url;
}

RssFeed RssParser::parse(const QByteArray &body, int httpStatus) {
    RssFeed out;
    out.outcome = outcomeForHttp(httpStatus);
    if (out.outcome != Outcome::Ok) return out;

    QXmlStreamReader xml(body);
    bool sawFeed = false;
    bool inEntry = false;
    int depthInEntry = 0;
    Item cur;
    QString link;
    while (!xml.atEnd()) {
        const QXmlStreamReader::TokenType token = xml.readNext();
        if (token == QXmlStreamReader::StartElement) {
            const QStringRef ns = xml.namespaceUri();
            const QStringRef name = xml.name();
            if (!sawFeed) {
                if (ns == kAtom && name == QLatin1String("feed")) { sawFeed = true; continue; }
                break;   // not an Atom feed (an HTML error page, for example)
            }
            if (!inEntry) {
                if (ns == kAtom && name == QLatin1String("entry")) {
                    inEntry = true;
                    depthInEntry = 0;
                    cur = Item();
                    cur.kind = ItemKind::Video;
                    cur.liveStatus = media::LiveStatus::Unknown;
                    link.clear();
                } else if (ns == kYt && name == QLatin1String("channelId")) {
                    const QString id = xml.readElementText().trimmed();
                    out.channelId = id.startsWith(QLatin1String("UC")) ? id : QStringLiteral("UC") + id;
                } else if (ns == kAtom && name == QLatin1String("title")) {
                    out.channelTitle = xml.readElementText().trimmed();
                }
                continue;
            }
            ++depthInEntry;
            if (ns == kYt && name == QLatin1String("videoId")) {
                cur.id = xml.readElementText().trimmed().toStdString(); --depthInEntry;
            } else if (ns == kYt && name == QLatin1String("channelId")) {
                cur.channelId = xml.readElementText().trimmed().toStdString(); --depthInEntry;
            } else if (ns == kAtom && name == QLatin1String("title") && depthInEntry == 1) {
                cur.title = xml.readElementText().trimmed().toStdString(); --depthInEntry;
            } else if (ns == kAtom && name == QLatin1String("published")) {
                const QString text = xml.readElementText();
                cur.publishedEpoch = parseIsoDate(text);
                --depthInEntry;
            } else if (ns == kAtom && name == QLatin1String("link")) {
                const QString rel = xml.attributes().value(QStringLiteral("rel")).toString();
                if (rel.isEmpty() || rel == QLatin1String("alternate"))
                    link = xml.attributes().value(QStringLiteral("href")).toString();
            } else if (ns == kAtom && name == QLatin1String("name") && depthInEntry == 2) {
                cur.channel = xml.readElementText().trimmed().toStdString(); --depthInEntry;
            } else if (ns == kMedia && name == QLatin1String("statistics")) {
                bool ok = false;
                const qlonglong views = xml.attributes().value(QStringLiteral("views")).toString().toLongLong(&ok);
                if (ok && views >= 0) cur.viewCount = views;
            }
        } else if (token == QXmlStreamReader::EndElement) {
            if (!inEntry) continue;
            if (depthInEntry > 0) { --depthInEntry; continue; }
            // </entry>
            inEntry = false;
            const QString id = QString::fromStdString(cur.id);
            if (!plausibleVideoId(id)) { ++out.skipped; continue; }
            if (link.contains(QLatin1String("/shorts/"))) {
                cur.kind = ItemKind::Short;
                cur.shortEvidence = ShortEvidence::RssLink;
            }
            if (cur.channelId.empty() && !out.channelId.isEmpty()) cur.channelId = out.channelId.toStdString();
            if (cur.channel.empty()) cur.channel = out.channelTitle.toStdString();
            cur.thumbnailUrl = listThumbnailUrl(cur.id);
            out.items.push_back(cur);
        }
    }
    if (!sawFeed || (xml.hasError() && out.items.empty())) {
        out.outcome = Outcome::ShapeDrift;
        out.items.clear();
        return out;
    }
    out.outcome = out.items.empty() ? Outcome::ValidEmpty : Outcome::Ok;
    return out;
}

}}}
