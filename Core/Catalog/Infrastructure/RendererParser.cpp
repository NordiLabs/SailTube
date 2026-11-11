// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "RendererParser.h"
#include "Json.h"
#include "Core/Catalog/Domain/AvatarBook.h"
#include "Core/Catalog/Domain/Images.h"
#include "Core/Catalog/Domain/ThumbnailBook.h"
#include "Core/Catalog/Domain/TextParse.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QSet>

namespace nordtile { namespace video { namespace catalog {

using media::LiveStatus;

namespace {

const QSet<QString> &itemRenderers() {
    static const QSet<QString> s = {
        QStringLiteral("lockupViewModel"), QStringLiteral("videoRenderer"), QStringLiteral("gridVideoRenderer"),
        QStringLiteral("compactVideoRenderer"), QStringLiteral("playlistVideoRenderer"),
        QStringLiteral("playlistPanelVideoRenderer"), QStringLiteral("shortsLockupViewModel"),
        QStringLiteral("reelItemRenderer"), QStringLiteral("channelRenderer"), QStringLiteral("gridChannelRenderer"),
        QStringLiteral("playlistRenderer"), QStringLiteral("gridPlaylistRenderer")};
    return s;
}

const QSet<QString> &containers() {
    static const QSet<QString> s = {
        QStringLiteral("richItemRenderer"), QStringLiteral("richSectionRenderer"), QStringLiteral("richShelfRenderer"),
        QStringLiteral("richGridRenderer"), QStringLiteral("itemSectionRenderer"), QStringLiteral("sectionListRenderer"),
        QStringLiteral("shelfRenderer"), QStringLiteral("reelShelfRenderer"), QStringLiteral("gridRenderer"),
        QStringLiteral("verticalListRenderer"), QStringLiteral("horizontalListRenderer"),
        QStringLiteral("expandedShelfContentsRenderer"), QStringLiteral("gridShelfViewModel"),
        QStringLiteral("horizontalCardListRenderer"), QStringLiteral("playlistVideoListRenderer"),
        QStringLiteral("tabRenderer"), QStringLiteral("twoColumnBrowseResultsRenderer"),
        QStringLiteral("twoColumnSearchResultsRenderer"), QStringLiteral("twoColumnWatchNextResults"),
        QStringLiteral("secondarySearchContainerRenderer"), QStringLiteral("appendContinuationItemsAction"),
        QStringLiteral("reloadContinuationItemsCommand"), QStringLiteral("watchNextSecondaryResultsRenderer"),
        QStringLiteral("playlistPanelRenderer"), QStringLiteral("expandableTabRenderer"),
        QStringLiteral("continuationItemRenderer"), QStringLiteral("continuationItemViewModel")};
    return s;
}

// Subtrees that never hold result rows (and may hold misleading ones).
const QSet<QString> &skipped() {
    static const QSet<QString> s = {
        QStringLiteral("responseContext"), QStringLiteral("frameworkUpdates"), QStringLiteral("engagementPanels"),
        QStringLiteral("topbar"), QStringLiteral("header"), QStringLiteral("metadata"), QStringLiteral("microformat"),
        QStringLiteral("adSlotRenderer"), QStringLiteral("promotedSparklesWebRenderer"),
        QStringLiteral("searchPyvRenderer"), QStringLiteral("menu"), QStringLiteral("sidebar"),
        QStringLiteral("chipCloudRenderer"), QStringLiteral("feedFilterChipBarRenderer"),
        QStringLiteral("searchHeaderRenderer"), QStringLiteral("refinements"), QStringLiteral("overlay"),
        QStringLiteral("playerOverlays"), QStringLiteral("onResponseReceivedEndpointsMetadata")};
    return s;
}

const QSet<QString> &contentListKeys() {
    static const QSet<QString> s = {QStringLiteral("contents"), QStringLiteral("items"),
                                    QStringLiteral("continuationItems"), QStringLiteral("results")};
    return s;
}

std::string str(const QString &q) { return q.toStdString(); }

// 1 when `holder` (a row's thumbnail object) names /vi/<id>/hq720..., -1 when it names only other
// pictures of that video, 0 when it names none.
int largeListed(const QJsonValue &holder, const std::string &id) {
    if (!holder.isObject() || id.empty()) return 0;
    const QByteArray text = QJsonDocument(holder.toObject()).toJson(QJsonDocument::Compact);
    const QByteArray base = "/vi/" + QByteArray::fromStdString(id) + "/";
    if (text.contains(base + "hq720")) return 1;
    return text.contains(base) ? -1 : 0;
}

bool isMembersBadge(const QString &style, const QString &label) {
    return style.contains(QLatin1String("MEMBER"), Qt::CaseInsensitive) ||
           label.contains(QLatin1String("member"), Qt::CaseInsensitive);
}

}  // namespace

RendererParser::RendererParser(const QString &hl) : hl_(hl), hlStd_(hl.toStdString()) {}

bool RendererParser::isItemRenderer(const QString &key) { return itemRenderers().contains(key); }
bool RendererParser::isKnownContainer(const QString &key) { return containers().contains(key); }

ParsedItems RendererParser::parse(const QJsonValue &root) const {
    ParsedItems out;
    walk(root, out, false);
    // Owner avatars the rows carry are remembered for rows that carry none (AvatarBook).
    bool namesLarge = false;
    for (const Item &item : out.items) {
        if (item.largeThumbnailListed > 0) namesLarge = true;
        if (item.avatarUrl.empty()) continue;
        AvatarBook::instance().learn(item.kind == ItemKind::Channel ? item.id : item.channelId, item.avatarUrl);
    }
    // In an answer whose rows name the 1280x720 picture, a video with only smaller ones has none.
    if (namesLarge) {
        for (const Item &item : out.items)
            if (item.kind == ItemKind::Video && item.largeThumbnailListed < 0) ThumbnailBook::instance().markLargeMissing(item.id);
    }
    return out;
}

bool RendererParser::learnChannelAvatar(const QJsonObject &browseRoot) {
    const QJsonObject md = json::at(browseRoot, QStringLiteral("metadata.channelMetadataRenderer")).toObject();
    const std::string id = str(md.value(QStringLiteral("externalId")).toString());
    const std::string avatar = rewriteAvatarUrl(str(json::bestImage(md.value(QStringLiteral("avatar")))));
    if (!AvatarBook::acceptable(id, avatar)) return false;
    AvatarBook::instance().learn(id, avatar);
    return true;
}

void RendererParser::walk(const QJsonValue &v, ParsedItems &out, bool contentList) const {
    if (v.isArray()) {
        for (const QJsonValue &x : v.toArray()) {
            if (contentList && x.isObject()) {
                const QJsonObject o = x.toObject();
                if (o.size() >= 1) {
                    const QString k = o.begin().key();
                    const bool rendererLike = k.endsWith(QLatin1String("Renderer")) || k.endsWith(QLatin1String("ViewModel"));
                    if (rendererLike && !isItemRenderer(k) && !isKnownContainer(k) && !skipped().contains(k))
                        out.unknown[k] += 1;
                }
            }
            walk(x, out, false);
        }
        return;
    }
    if (!v.isObject()) return;
    const QJsonObject o = v.toObject();
    for (auto it = o.begin(); it != o.end(); ++it) {
        const QString &key = it.key();
        if (skipped().contains(key)) continue;
        if (isItemRenderer(key) && it.value().isObject()) {
            Item item;
            if (parseItem(key, it.value().toObject(), item)) out.items.push_back(item);
            else ++out.unparsed;
            continue;
        }
        if ((key == QLatin1String("continuationItemRenderer") || key == QLatin1String("continuationItemViewModel")) &&
            it.value().isObject()) {
            const QString token = json::continuationToken(it.value().toObject());
            if (!token.isEmpty()) out.continuation = token;
            continue;
        }
        walk(it.value(), out, contentListKeys().contains(key));
    }
}

bool RendererParser::parseItem(const QString &key, const QJsonObject &r, Item &out) const {
    if (key == QLatin1String("lockupViewModel")) return parseLockup(r, out);
    if (key == QLatin1String("shortsLockupViewModel")) return parseShortsLockup(r, out);
    if (key == QLatin1String("reelItemRenderer")) return parseReelItem(r, out);
    if (key == QLatin1String("channelRenderer") || key == QLatin1String("gridChannelRenderer")) return parseChannelRenderer(r, out);
    if (key == QLatin1String("playlistRenderer") || key == QLatin1String("gridPlaylistRenderer")) return parsePlaylistRenderer(r, out);
    return parseVideoRenderer(r, out);
}

void RendererParser::applyBadges(const QJsonValue &holder, Item &out) const {
    // Lockup generation: thumbnailBadgeViewModel {text, badgeStyle}.
    json::forEachKey(holder, QStringLiteral("thumbnailBadgeViewModel"), [&](const QJsonObject &b) {
        const QString style = b.value(QStringLiteral("badgeStyle")).toString();
        const QString text = b.value(QStringLiteral("text")).toString();
        if (style.contains(QLatin1String("LIVE"))) { out.liveStatus = LiveStatus::IsLive; return true; }
        if (style.contains(QLatin1String("UPCOMING")) || style.contains(QLatin1String("PREMIERE"))) { out.liveStatus = LiveStatus::Upcoming; return true; }
        if (isMembersBadge(style, text)) { out.membersOnly = true; return true; }
        const std::int64_t d = parseDurationText(str(text));
        if (d >= 0 && out.durationSeconds < 0) { out.durationSeconds = d; out.durationText = str(text); }
        else if (out.kind == ItemKind::Playlist && out.videoCount < 0) {
            out.videoCountText = str(text);
            out.videoCount = parseCountText(str(text), hlStd_);
        }
        return true;
    });
    // badgeViewModel {badgeText, badgeStyle} (members-only, premiere, live in lockup metadata).
    json::forEachKey(holder, QStringLiteral("badgeViewModel"), [&](const QJsonObject &b) {
        const QString style = b.value(QStringLiteral("badgeStyle")).toString();
        const QString text = b.value(QStringLiteral("badgeText")).toString();
        if (isMembersBadge(style, text)) out.membersOnly = true;
        else if (style.contains(QLatin1String("LIVE"))) out.liveStatus = LiveStatus::IsLive;
        return true;
    });
    // Older generation: metadataBadgeRenderer {style, label}.
    json::forEachKey(holder, QStringLiteral("metadataBadgeRenderer"), [&](const QJsonObject &b) {
        const QString style = b.value(QStringLiteral("style")).toString();
        const QString label = b.value(QStringLiteral("label")).toString();
        if (style == QLatin1String("BADGE_STYLE_TYPE_LIVE_NOW")) out.liveStatus = LiveStatus::IsLive;
        else if (isMembersBadge(style, label)) out.membersOnly = true;
        else if (style == QLatin1String("BADGE_STYLE_TYPE_AGE_RESTRICTED") || style.contains(QLatin1String("RESTRICT"))) out.restricted = true;
        return true;
    });
    json::forEachKey(holder, QStringLiteral("thumbnailOverlayTimeStatusRenderer"), [&](const QJsonObject &b) {
        const QString style = b.value(QStringLiteral("style")).toString();
        if (style == QLatin1String("LIVE")) out.liveStatus = LiveStatus::IsLive;
        else if (style == QLatin1String("UPCOMING")) out.liveStatus = LiveStatus::Upcoming;
        else if (style == QLatin1String("SHORTS")) { out.kind = ItemKind::Short; out.shortEvidence = ShortEvidence::Renderer; }
        if (out.durationSeconds < 0) {
            const QString t = json::text(b.value(QStringLiteral("text")));
            const std::int64_t d = parseDurationText(str(t));
            if (d >= 0) { out.durationSeconds = d; out.durationText = str(t); }
        }
        return true;
    });
}

bool RendererParser::parseVideoRenderer(const QJsonObject &r, Item &out) const {
    out.kind = ItemKind::Video;
    out.id = str(r.value(QStringLiteral("videoId")).toString());
    if (out.id.empty()) return false;
    out.title = str(json::text(r.value(QStringLiteral("title"))));
    if (out.title.empty()) out.title = str(json::text(r.value(QStringLiteral("headline"))));
    for (const char *k : {"longBylineText", "ownerText", "shortBylineText"}) {
        const QJsonValue byline = r.value(QLatin1String(k));
        if (byline.isUndefined()) continue;
        if (out.channel.empty()) out.channel = str(json::text(byline));
        if (out.channelId.empty()) out.channelId = str(json::firstBrowseId(byline));
    }
    // Structured length first (playlist rows carry lengthSeconds), then text.
    const QJsonValue lengthSeconds = r.value(QStringLiteral("lengthSeconds"));
    if (lengthSeconds.isString() || lengthSeconds.isDouble())
        out.durationSeconds = lengthSeconds.isString() ? lengthSeconds.toString().toLongLong() : static_cast<qint64>(lengthSeconds.toDouble());
    const QString lengthText = json::text(r.value(QStringLiteral("lengthText")));
    if (!lengthText.isEmpty()) {
        out.durationText = str(lengthText);
        if (out.durationSeconds < 0) out.durationSeconds = parseDurationText(out.durationText);
    }
    QString views = json::text(r.value(QStringLiteral("viewCountText")));
    if (views.isEmpty()) views = json::text(r.value(QStringLiteral("shortViewCountText")));
    if (!views.isEmpty()) {
        out.viewText = str(views);
        out.viewCount = parseCountText(out.viewText, hlStd_);
    }
    const QString published = json::text(r.value(QStringLiteral("publishedTimeText")));
    if (!published.isEmpty()) {
        out.publishedText = str(published);
        out.publishedAgeSeconds = parseAgeSeconds(out.publishedText, hlStd_);
    }
    if (r.contains(QStringLiteral("upcomingEventData"))) {
        out.liveStatus = LiveStatus::Upcoming;
        const qint64 start = r.value(QStringLiteral("upcomingEventData")).toObject().value(QStringLiteral("startTime")).toString().toLongLong();
        if (start > 0) out.publishedEpoch = start;
    }
    applyBadges(r.value(QStringLiteral("badges")), out);
    applyBadges(r.value(QStringLiteral("thumbnailOverlays")), out);
    if (out.liveStatus == LiveStatus::Unknown) out.liveStatus = out.durationSeconds >= 0 ? LiveStatus::NotLive : LiveStatus::Unknown;
    const QJsonValue avatar = json::at(r, QStringLiteral("channelThumbnailSupportedRenderers.channelThumbnailWithLinkRenderer.thumbnail"));
    if (avatar.isObject()) out.avatarUrl = rewriteAvatarUrl(str(json::bestImage(avatar)));
    const QJsonValue index = r.value(QStringLiteral("index"));
    if (!index.isUndefined()) out.playlistIndex = json::text(index).toInt() - 1;
    out.thumbnailUrl = listThumbnailUrl(out.id);
    out.largeThumbnailListed = largeListed(r.value(QStringLiteral("thumbnail")), out.id);
    return true;
}

bool RendererParser::parseLockup(const QJsonObject &r, Item &out) const {
    const QString type = r.value(QStringLiteral("contentType")).toString();
    const QString id = r.value(QStringLiteral("contentId")).toString();
    if (id.isEmpty()) return false;
    if (type == QLatin1String("LOCKUP_CONTENT_TYPE_VIDEO")) out.kind = ItemKind::Video;
    else if (type == QLatin1String("LOCKUP_CONTENT_TYPE_PLAYLIST") || type == QLatin1String("LOCKUP_CONTENT_TYPE_ALBUM") ||
             type == QLatin1String("LOCKUP_CONTENT_TYPE_PODCAST")) out.kind = ItemKind::Playlist;
    else if (type == QLatin1String("LOCKUP_CONTENT_TYPE_CHANNEL")) out.kind = ItemKind::Channel;
    else return false;  // unknown content types are not rows
    out.id = str(id);
    const QJsonObject md = json::at(r, QStringLiteral("metadata.lockupMetadataViewModel")).toObject();
    out.title = str(json::text(md.value(QStringLiteral("title"))));
    out.channelId = str(json::firstBrowseId(md));
    const QJsonValue avatar = json::at(md, QStringLiteral("image.decoratedAvatarViewModel.avatar.avatarViewModel.image"));
    if (avatar.isObject()) out.avatarUrl = rewriteAvatarUrl(str(json::bestImage(avatar)));
    const QJsonArray rows = json::at(md, QStringLiteral("metadata.contentMetadataViewModel.metadataRows")).toArray();
    for (int ri = 0; ri < rows.size(); ++ri) {
        const QJsonArray parts = rows.at(ri).toObject().value(QStringLiteral("metadataParts")).toArray();
        for (int pi = 0; pi < parts.size(); ++pi) {
            const QJsonObject part = parts.at(pi).toObject();
            const QString t = json::text(part.value(QStringLiteral("text")));
            if (t.isEmpty()) continue;
            const std::string s = str(t);
            switch (classifyMetaPart(s, hlStd_)) {
            case MetaPart::Watching:
                out.liveStatus = LiveStatus::IsLive;
                out.viewText = s; out.viewCount = parseCountText(s, hlStd_);
                break;
            case MetaPart::Views:
                if (out.viewCount < 0) { out.viewText = s; out.viewCount = parseCountText(s, hlStd_); }
                break;
            case MetaPart::Age:
                out.publishedText = s; out.publishedAgeSeconds = parseAgeSeconds(s, hlStd_);
                break;
            case MetaPart::Scheduled:
                out.liveStatus = LiveStatus::Upcoming; out.publishedText = s;
                break;
            case MetaPart::VideoCount:
                if (out.videoCount < 0) { out.videoCountText = s; out.videoCount = parseCountText(s, hlStd_); }
                break;
            case MetaPart::Subscribers:
                out.subscriberText = s; out.subscriberCount = parseSubscriberText(s, hlStd_);
                break;
            case MetaPart::Unknown:
                // The first unclassified text of the first row is the owner (absent on a
                // channel's own tabs). Playlist rows add a kind label ("Playlist") after it.
                if (ri == 0 && pi == 0 && out.channel.empty()) out.channel = s;
                break;
            }
        }
    }
    applyBadges(r.value(QStringLiteral("contentImage")), out);
    applyBadges(md, out);
    if (out.kind == ItemKind::Video) {
        if (out.liveStatus == LiveStatus::Unknown && out.durationSeconds >= 0) out.liveStatus = LiveStatus::NotLive;
        out.thumbnailUrl = listThumbnailUrl(out.id);
        out.largeThumbnailListed = largeListed(r.value(QStringLiteral("contentImage")), out.id);
    } else {
        out.thumbnailUrl = str(json::bestImage(json::findFirst(r.value(QStringLiteral("contentImage")), QStringLiteral("image")), false));
        const QJsonObject image = json::findFirst(r.value(QStringLiteral("contentImage")), QStringLiteral("thumbnailViewModel"));
        if (out.thumbnailUrl.empty()) out.thumbnailUrl = str(json::bestImage(image.value(QStringLiteral("image")), false));
    }
    return true;
}

bool RendererParser::parseShortsLockup(const QJsonObject &r, Item &out) const {
    out.kind = ItemKind::Short;
    out.shortEvidence = ShortEvidence::Renderer;
    QString id = json::findFirst(r.value(QStringLiteral("onTap")), QStringLiteral("reelWatchEndpoint")).value(QStringLiteral("videoId")).toString();
    if (id.isEmpty()) {
        const QString entity = r.value(QStringLiteral("entityId")).toString();
        if (entity.size() >= 11) id = entity.right(11);
    }
    if (id.isEmpty()) return false;
    out.id = str(id);
    out.title = str(json::text(json::at(r, QStringLiteral("overlayMetadata.primaryText"))));
    const QString views = json::text(json::at(r, QStringLiteral("overlayMetadata.secondaryText")));
    if (!views.isEmpty()) { out.viewText = str(views); out.viewCount = parseCountText(out.viewText, hlStd_); }
    if (out.title.empty()) {
        // "Title, 1.8 million views - play Short": keep the text before the last comma.
        const QString a11y = r.value(QStringLiteral("accessibilityText")).toString();
        const int comma = a11y.lastIndexOf(QLatin1Char(','));
        out.title = str(comma > 0 ? a11y.left(comma) : a11y);
    }
    out.liveStatus = LiveStatus::NotLive;
    out.thumbnailUrl = listThumbnailUrl(out.id);
    return true;
}

bool RendererParser::parseReelItem(const QJsonObject &r, Item &out) const {
    out.kind = ItemKind::Short;
    out.shortEvidence = ShortEvidence::Renderer;
    out.id = str(r.value(QStringLiteral("videoId")).toString());
    if (out.id.empty()) return false;
    out.title = str(json::text(r.value(QStringLiteral("headline"))));
    const QString views = json::text(r.value(QStringLiteral("viewCountText")));
    if (!views.isEmpty()) { out.viewText = str(views); out.viewCount = parseCountText(out.viewText, hlStd_); }
    out.liveStatus = LiveStatus::NotLive;
    out.thumbnailUrl = listThumbnailUrl(out.id);
    return true;
}

bool RendererParser::parseChannelRenderer(const QJsonObject &r, Item &out) const {
    out.kind = ItemKind::Channel;
    out.id = str(r.value(QStringLiteral("channelId")).toString());
    if (out.id.empty()) return false;
    out.title = str(json::text(r.value(QStringLiteral("title"))));
    out.channel = out.title;
    out.channelId = out.id;
    // Field names are swapped on search results (the @handle sits in subscriberCountText and
    // the subscriber count in videoCountText): classify by content, not by name.
    for (const char *k : {"subscriberCountText", "videoCountText"}) {
        const QString t = json::text(r.value(QLatin1String(k)));
        if (t.isEmpty()) continue;
        const std::string s = str(t);
        if (t.startsWith(QLatin1Char('@'))) { out.handle = s; continue; }
        const MetaPart kind = classifyMetaPart(s, hlStd_);
        if (kind == MetaPart::Subscribers) { out.subscriberText = s; out.subscriberCount = parseSubscriberText(s, hlStd_); }
        else if (kind == MetaPart::VideoCount) { out.videoCountText = s; out.videoCount = parseCountText(s, hlStd_); }
        else if (out.subscriberText.empty()) out.subscriberText = s;
    }
    std::vector<ImageCandidate> candidates;
    const QJsonArray thumbs = json::at(r, QStringLiteral("thumbnail.thumbnails")).toArray();
    for (const QJsonValue &t : thumbs) {
        const QJsonObject o = t.toObject();
        candidates.push_back({str(o.value(QStringLiteral("url")).toString()), o.value(QStringLiteral("width")).toInt(),
                              o.value(QStringLiteral("height")).toInt()});
    }
    out.avatarUrl = chooseAvatar(candidates);
    out.thumbnailUrl = out.avatarUrl;
    return true;
}

bool RendererParser::parsePlaylistRenderer(const QJsonObject &r, Item &out) const {
    out.kind = ItemKind::Playlist;
    out.id = str(r.value(QStringLiteral("playlistId")).toString());
    if (out.id.empty()) return false;
    out.title = str(json::text(r.value(QStringLiteral("title"))));
    for (const char *k : {"longBylineText", "shortBylineText"}) {
        const QJsonValue byline = r.value(QLatin1String(k));
        if (byline.isUndefined()) continue;
        if (out.channel.empty()) out.channel = str(json::text(byline));
        if (out.channelId.empty()) out.channelId = str(json::firstBrowseId(byline));
    }
    const QJsonValue count = r.value(QStringLiteral("videoCount"));
    if (count.isString()) out.videoCount = count.toString().toLongLong();
    for (const char *k : {"videoCountText", "videoCountShortText", "thumbnailText"}) {
        const QString t = json::text(r.value(QLatin1String(k)));
        if (t.isEmpty()) continue;
        if (out.videoCountText.empty()) out.videoCountText = str(t);
        if (out.videoCount < 0) out.videoCount = parseCountText(str(t), hlStd_);
    }
    QJsonValue thumb = r.value(QStringLiteral("thumbnail"));
    if (!thumb.isObject()) thumb = json::at(r, QStringLiteral("thumbnails.0"));
    out.thumbnailUrl = str(json::bestImage(thumb, false));
    return true;
}

}}}
