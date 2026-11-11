// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#pragma once

// The channel RSS feed, the feed's first tier (DSC-05):
//   GET https://www.youtube.com/feeds/videos.xml?channel_id=<UC...>
// Reads per entry yt:videoId, title, published (absolute date), media:statistics views, the
// author and the alternate link. A link containing /shorts/ is structural evidence of a Short
// (DSC-17, ShortEvidence::RssLink). RSS has no duration and no live state; the service enriches
// rows with the channel's Live tab (DSC-07). About 15 entries per channel.
//
// Outcomes: a non-200 answer is classified by status (a dead channel answers 404: Http4xx);
// a document without the Atom <feed> root is drift; a feed without entries is valid-empty.

#include "Core/Catalog/Domain/Item.h"
#include "Core/Catalog/Domain/Outcome.h"

#include <QByteArray>
#include <QString>
#include <QUrl>
#include <vector>

namespace nordtile { namespace video { namespace catalog {

struct RssFeed {
    Outcome outcome = Outcome::Network;
    std::vector<Item> items;
    QString channelId;      // from the feed's own yt:channelId / alternate link (UC...)
    QString channelTitle;
    int skipped = 0;        // entries without a usable video id
};

class RssParser {
public:
    static QUrl feedUrl(const QString &channelId);
    // `httpStatus` 0 means the request failed below HTTP.
    static RssFeed parse(const QByteArray &body, int httpStatus = 200);
};

}}}
