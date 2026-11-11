// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "ShortDetailService.h"
#include "Json.h"
#include "Core/Catalog/Domain/AvatarBook.h"
#include "Core/Catalog/Domain/Images.h"
#include "Core/Catalog/Domain/Subscriptions.h"
#include "Core/Catalog/Domain/TextParse.h"
#include "Core/Catalog/Domain/ThumbnailBook.h"

#include <QJsonArray>
#include <QTimer>

namespace nordtile { namespace video { namespace catalog {

namespace {

const char kStatusSucceeded[] = "REEL_ITEM_WATCH_STATUS_SUCCEEDED";
const char kChipRelated[] = "id.reel_multi_format_link";
const char kButtonComments[] = "id.reel_comment_button";
const char kPanelAudio[] = "engagement-panel-shorts-audio-pivot";
const char kPanelDescription[] = "engagement-panel-structured-description";

std::string s(const QString &q) { return q.toStdString(); }

bool hasDigit(const QString &text) {
    for (const QChar c : text) if (c.isDigit()) return true;
    return false;
}

// "4.8K" / "115" / "62 K": the text as YouTube wrote it when it names a number, else "".
QString countText(const QString &title) {
    const QString t = title.trimmed();
    return hasDigit(t) ? t : QString();
}

// The count of a button: its accessibility text names the exact number where the title is
// compact ("4,862 likes", "62.285 Me gusta"); else the title.
std::int64_t countOf(const QString &accessibility, const QString &title, const std::string &hl) {
    if (hasDigit(accessibility)) {
        const std::int64_t v = parseCountText(s(accessibility), hl);
        if (v >= 0) return v;
    }
    return hasDigit(title) ? parseCountText(s(title), hl) : -1;
}

QString plain(const QJsonValue &v) { return json::text(v).trimmed(); }

QString contentOf(const QJsonValue &textHolder) {
    const QString c = textHolder.toObject().value(QStringLiteral("content")).toString();
    return c.isEmpty() ? plain(textHolder) : c;
}

void readChannelBar(const QJsonObject &bar, ShortDetail *d) {
    d->channelHandle = s(contentOf(bar.value(QStringLiteral("channelName"))).trimmed());
    const std::string id = s(json::firstBrowseId(bar));
    if (isFeedChannelId(id)) d->channelId = id;   // "UC" + 22 characters only
    const QJsonObject avatar = json::findFirst(bar, QStringLiteral("avatarViewModel"));
    const QString url = json::bestImage(avatar);
    if (!url.isEmpty()) d->channelAvatar = rewriteAvatarUrl(s(url));
    if (d->channelName.empty()) d->channelName = d->channelHandle;
}

void readChips(const QJsonObject &carousel, ShortDetail *d) {
    for (const QJsonValue &v : carousel.value(QStringLiteral("buttonViewModels")).toArray()) {
        const QJsonObject b = json::at(v, QStringLiteral("reelCarouselButtonViewModel.buttonViewModel.buttonViewModel")).toObject();
        if (b.isEmpty()) continue;
        const QJsonObject command = json::at(b, QStringLiteral("onTap.innertubeCommand")).toObject();
        const QString title = contentOf(b.value(QStringLiteral("titleFormatted"))).trimmed();
        if (b.value(QStringLiteral("accessibilityId")).toString() == QLatin1String(kChipRelated)) {
            QJsonObject target = command.value(QStringLiteral("watchEndpoint")).toObject();
            bool isShort = false;
            if (target.isEmpty()) { target = command.value(QStringLiteral("reelWatchEndpoint")).toObject(); isShort = true; }
            const std::string id = s(target.value(QStringLiteral("videoId")).toString());
            if (ThumbnailBook::isVideoId(id) && d->relatedVideoId.empty()) {
                d->relatedVideoId = id;
                d->relatedVideoIsShort = isShort;
                d->relatedVideoTitle = s(title.isEmpty() ? b.value(QStringLiteral("accessibilityText")).toString() : title);
            }
        } else if (json::at(command, QStringLiteral("showEngagementPanelEndpoint.identifier.tag")).toString() == QLatin1String(kPanelAudio)) {
            if (d->soundText.empty()) d->soundText = s(title);
        }
    }
}

void readActionBar(const QJsonArray &buttons, const std::string &hl, ShortDetail *d, bool *any) {
    bool comments = false;
    for (const QJsonValue &v : buttons) {
        const QJsonObject o = v.toObject();
        if (o.contains(QStringLiteral("likeButtonViewModel"))) {
            *any = true;
            const QJsonObject toggle = json::at(o, QStringLiteral("likeButtonViewModel.toggleButtonViewModel.toggleButtonViewModel")).toObject();
            const QJsonObject idle = json::at(toggle, QStringLiteral("defaultButtonViewModel.buttonViewModel")).toObject();
            const QJsonObject on = json::at(toggle, QStringLiteral("toggledButtonViewModel.buttonViewModel")).toObject();
            const QString title = idle.value(QStringLiteral("title")).toString();
            d->likeText = s(countText(title));
            d->likeCount = countOf(on.value(QStringLiteral("accessibilityText")).toString(), title, hl);
            if (d->likeText.empty()) d->likeCount = -1;   // a hidden count says "Like" only
        } else if (o.contains(QStringLiteral("buttonViewModel"))) {
            const QJsonObject b = o.value(QStringLiteral("buttonViewModel")).toObject();
            if (b.value(QStringLiteral("accessibilityId")).toString() != QLatin1String(kButtonComments)) continue;
            *any = true;
            comments = true;
            const QString title = b.value(QStringLiteral("title")).toString();
            d->commentText = s(countText(title));
            d->commentCount = countOf(b.value(QStringLiteral("accessibilityText")).toString(), title, hl);
        } else if (o.contains(QStringLiteral("pivotButtonViewModel"))) {
            const QString sound = contentOf(json::at(o, QStringLiteral("pivotButtonViewModel.soundAttributionTitle")));
            if (d->soundText.empty() && !sound.isEmpty()) d->soundText = s(sound.trimmed());
        }
    }
    if (!buttons.isEmpty()) d->commentsDisabled = !comments;
}

void readDescriptionPanel(const QJsonArray &panels, const std::string &hl, ShortDetail *d) {
    for (const QJsonValue &pv : panels) {
        const QJsonObject panel = pv.toObject().value(QStringLiteral("engagementPanelSectionListRenderer")).toObject();
        if (panel.value(QStringLiteral("targetId")).toString() != QLatin1String(kPanelDescription)) continue;
        for (const QJsonValue &iv : json::at(panel, QStringLiteral("content.structuredDescriptionContentRenderer.items")).toArray()) {
            const QJsonObject item = iv.toObject();
            const QJsonObject header = item.value(QStringLiteral("videoDescriptionHeaderRenderer")).toObject();
            if (!header.isEmpty()) {
                const QString name = plain(header.value(QStringLiteral("channel")));
                if (!name.isEmpty()) d->channelName = s(name);
                const QString views = plain(header.value(QStringLiteral("views")));
                if (!views.isEmpty()) { d->viewText = s(views); d->viewCount = parseCountText(s(views), hl); }
                if (d->publishedText.empty()) d->publishedText = s(plain(header.value(QStringLiteral("publishDate"))));
                if (d->title.empty()) d->title = s(plain(header.value(QStringLiteral("title"))));
            }
            const QJsonObject body = item.value(QStringLiteral("expandableVideoDescriptionBodyRenderer")).toObject();
            if (!body.isEmpty()) {
                QString text = plain(body.value(QStringLiteral("descriptionBodyText")));
                if (text.isEmpty()) text = contentOf(body.value(QStringLiteral("attributedDescriptionBodyText"))).trimmed();
                if (!text.isEmpty()) d->description = s(text);
            }
        }
    }
}

}  // namespace

ShortDetailService::ShortDetailService(InnerTubeClient *client, CatalogCounters *counters, QObject *parent, MillisClock clock)
    : QObject(parent), client_(client), counters_(counters), cache_(kCacheShorts, kCacheTtlMs, clock) {}

QJsonObject ShortDetailService::requestBody(const QString &videoId) {
    QJsonObject player;
    player.insert(QStringLiteral("videoId"), videoId);
    QJsonObject body;
    body.insert(QStringLiteral("playerRequest"), player);
    body.insert(QStringLiteral("disablePlayerResponse"), true);
    return body;
}

ShortDetailResult ShortDetailService::parse(const QJsonObject &answer, const QString &videoId, const QString &hl) {
    ShortDetailResult out;
    out.videoId = videoId;
    out.detail.videoId = s(videoId);
    const QString status = answer.value(QStringLiteral("status")).toString();
    if (!answer.contains(QStringLiteral("overlay")) ||
        (!status.isEmpty() && status != QLatin1String(kStatusSucceeded))) {
        out.outcome = Outcome::Unavailable;   // a missing, private or removed Short: final
        return out;
    }
    const QJsonObject model = json::at(answer, QStringLiteral(
        "overlay.reelPlayerOverlayRenderer.playerOverlay.reelPlayerOverlayViewModel")).toObject();
    const QJsonArray bar = json::at(model, QStringLiteral("actionBar.reelActionBarViewModel.buttonViewModels")).toArray();
    const QJsonArray meta = json::at(model, QStringLiteral("metapanel.reelMetapanelViewModel.metadataItems")).toArray();
    if (bar.isEmpty() && meta.isEmpty()) {
        out.outcome = Outcome::ShapeDrift;
        return out;
    }
    const std::string hlS = s(hl);
    ShortDetail &d = out.detail;
    bool recognised = false;
    // The metadata items come in no fixed order (a sticker or a suggested action can lead).
    for (const QJsonValue &v : meta) {
        const QJsonObject item = v.toObject();
        if (item.contains(QStringLiteral("reelChannelBarViewModel"))) {
            readChannelBar(item.value(QStringLiteral("reelChannelBarViewModel")).toObject(), &d);
            recognised = true;
        } else if (item.contains(QStringLiteral("shortsVideoTitleViewModel"))) {
            d.title = s(contentOf(json::at(item, QStringLiteral("shortsVideoTitleViewModel.text"))).trimmed());
            recognised = true;
        } else if (item.contains(QStringLiteral("reelCarouselViewModel"))) {
            readChips(item.value(QStringLiteral("reelCarouselViewModel")).toObject(), &d);
        }
    }
    readActionBar(bar, hlS, &d, &recognised);
    const QString stamp = plain(json::at(json::findFirst(answer.value(QStringLiteral("overlay")), QStringLiteral("reelPlayerHeaderRenderer")),
                                         QStringLiteral("timestampText")));
    if (!stamp.isEmpty()) d.publishedText = s(stamp);
    readDescriptionPanel(answer.value(QStringLiteral("engagementPanels")).toArray(), hlS, &d);
    if (!recognised) {
        out.outcome = Outcome::ShapeDrift;
        return out;
    }
    if (!d.channelId.empty() && !d.channelAvatar.empty()) AvatarBook::instance().learn(d.channelId, d.channelAvatar);
    out.outcome = Outcome::Ok;
    return out;
}

bool ShortDetailService::isCached(const QString &videoId) { return cache_.contains(s(videoId)); }

bool ShortDetailService::peek(const QString &videoId, ShortDetail *out) {
    ShortDetail d;
    if (!cache_.get(s(videoId), &d)) return false;
    if (out) *out = d;
    return true;
}

void ShortDetailService::forget(const QString &videoId) { cache_.remove(s(videoId)); }

bool ShortDetailService::fetch(const QString &videoId, QObject *context, std::function<void(const ShortDetailResult &)> done) {
    const std::string key = s(videoId);
    if (!ThumbnailBook::isVideoId(key) || !client_) return false;
    QPointer<QObject> guard(context);
    const bool hasContext = context != nullptr;
    ShortDetail cached;
    if (cache_.get(key, &cached)) {
        ShortDetailResult r;
        r.outcome = Outcome::Ok;
        r.videoId = videoId;
        r.detail = cached;
        r.fromCache = true;
        QTimer::singleShot(0, this, [guard, hasContext, done, r] {
            if ((!hasContext || guard) && done) done(r);
        });
        return true;
    }
    Waiter w;
    w.guard = guard;
    w.hasContext = hasContext;
    w.done = done;
    std::vector<Waiter> &waiters = waiting_[key];
    waiters.push_back(w);
    if (waiters.size() > 1) return true;   // joins the request in flight
    ++sent_;
    const QString hl = client_->locale().hl;
    std::function<ShortDetailResult(const InnerTubeReply &)> parseReply = [videoId, hl](const InnerTubeReply &r) {
        if (r.outcome != Outcome::Ok) {
            ShortDetailResult failed;
            failed.outcome = r.outcome;
            failed.videoId = videoId;
            failed.detail.videoId = s(videoId);
            return failed;
        }
        return parse(r.json, videoId, hl);
    };
    std::function<void(ShortDetailResult)> finished = [this](ShortDetailResult r) { finish(r); };
    client_->post<ShortDetailResult>(QLatin1String(innertube::kEndpointReelItemWatch), requestBody(videoId), this, parseReply, finished);
    return true;
}

void ShortDetailService::finish(const ShortDetailResult &result) {
    const std::string key = s(result.videoId);
    if (counters_) counters_->count(Area::WatchNext, result.outcome);
    if (result.outcome == Outcome::Ok) cache_.put(key, result.detail);
    const auto it = waiting_.find(key);
    if (it == waiting_.end()) return;
    const std::vector<Waiter> waiters = it->second;
    waiting_.erase(it);
    for (const Waiter &w : waiters)
        if ((!w.hasContext || w.guard) && w.done) w.done(result);
}

}}}
