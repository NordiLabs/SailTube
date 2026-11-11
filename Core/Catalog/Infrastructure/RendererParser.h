// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#pragma once

// Shape-based extraction of list rows from any InnerTube answer (DSC-03). Accepts every
// renderer generation in use, inside richItemRenderer and other container wrappers:
//
//   lockupViewModel (VIDEO -> video rows, PLAYLIST -> playlist rows), videoRenderer,
//   gridVideoRenderer, compactVideoRenderer, playlistVideoRenderer, playlistPanelVideoRenderer,
//   shortsLockupViewModel, reelItemRenderer, channelRenderer, gridChannelRenderer,
//   playlistRenderer, gridPlaylistRenderer, continuationItemRenderer and
//   continuationItemViewModel (paging token).
//
// Badges (duration, LIVE, UPCOMING, PREMIERE, members-only) come from thumbnailBadgeViewModel,
// badgeViewModel, metadataBadgeRenderer and thumbnailOverlayTimeStatusRenderer styles first,
// text second. Unknown renderers in content lists are skipped, never crash the parser, and are
// counted for diagnostics. Text fields go through the locale tables of TextParse (I18N-02);
// a value that does not parse keeps its raw text and the row is kept.

#include "Core/Catalog/Domain/Item.h"

#include <QJsonObject>
#include <QJsonValue>
#include <QMap>
#include <QString>
#include <vector>

namespace nordtile { namespace video { namespace catalog {

struct ParsedItems {
    std::vector<Item> items;
    QString continuation;          // the list's own paging token (last one at list level)
    QMap<QString, int> unknown;    // unknown renderer name -> occurrences in content lists
    int unparsed = 0;              // known renderers without a usable id (counted, skipped)
};

class RendererParser {
public:
    // `hl` is the language the answer is in (the request's hl).
    explicit RendererParser(const QString &hl = QStringLiteral("en"));

    // Collects rows below `root` in document order and the paging token.
    ParsedItems parse(const QJsonValue &root) const;

    // Single renderers, exposed for endpoint parsers and tests. They return false when the
    // renderer has no usable id.
    bool parseVideoRenderer(const QJsonObject &r, Item &out) const;
    bool parseLockup(const QJsonObject &r, Item &out) const;
    bool parseShortsLockup(const QJsonObject &r, Item &out) const;
    bool parseReelItem(const QJsonObject &r, Item &out) const;
    bool parseChannelRenderer(const QJsonObject &r, Item &out) const;
    bool parsePlaylistRenderer(const QJsonObject &r, Item &out) const;

    // A channel browse answer (any tab) names the channel and its avatar in
    // metadata.channelMetadataRenderer: remembered in the AvatarBook. Returns true when the
    // answer carried both. No request is made; the answer was fetched for its rows.
    static bool learnChannelAvatar(const QJsonObject &browseRoot);

    static bool isItemRenderer(const QString &key);
    static bool isKnownContainer(const QString &key);

    QString hl() const { return hl_; }

private:
    void walk(const QJsonValue &v, ParsedItems &out, bool contentList) const;
    bool parseItem(const QString &key, const QJsonObject &r, Item &out) const;
    void applyBadges(const QJsonValue &holder, Item &out) const;
    QString hl_;
    std::string hlStd_;
};

}}}
