// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "ShortsModel.h"

#include "TapIntent.h"
#include "ChannelAvatars.h"
#include "DislikeInfo.h"
#include "Core/Catalog/Domain/LocalizedText.h"
#include "Core/Catalog/Infrastructure/DislikeService.h"
#include "Core/Catalog/Infrastructure/ShortDetailService.h"
#include "Core/Catalog/Infrastructure/ShortsFeedService.h"

#include <QCoreApplication>
#include <QStringList>
#include <QTimer>

namespace nordtile { namespace video { namespace sailfish {

using catalog::Outcome;
using catalog::ShortsRow;

namespace {
QString q(const std::string &s) { return QString::fromStdString(s); }
const char *sourceName(catalog::ShortsSource s) {
    switch (s) {
    case catalog::ShortsSource::Subscriptions: return "subscriptions";
    case catalog::ShortsSource::Account: return "account";
    case catalog::ShortsSource::Popular: return "popular";
    }
    return "popular";
}
}

ShortsModel::ShortsModel(catalog::ShortsFeedService *service, TapIntent *intent, QObject *parent)
    : QAbstractListModel(parent), service_(service), intent_(intent), grace_(new QTimer(this)) {
    grace_->setSingleShot(true);
    grace_->setInterval(kGraceMs);
    connect(grace_, &QTimer::timeout, this, [this] { release(); });
    if (service_) {
        connect(service_, &catalog::ShortsFeedService::changed, this, [this] { sync(); });
        connect(service_, &catalog::ShortsFeedService::subscriptionsChanged, this, [this] {
            if (shown_ > 0) emit dataChanged(index(0), index(shown_ - 1), QVector<int>() << SubscribedRole);
            bump();
        });
    }
    ChannelAvatars::follow(this, ChannelAvatarRole);
    sync();
}

void ShortsModel::setDetails(catalog::ShortDetailService *details) { details_ = details; }

void ShortsModel::setDislikes(catalog::DislikeService *service, DislikeInfo *info) {
    if (dislikeInfo_) disconnect(dislikeInfo_, nullptr, this, nullptr);
    dislikes_ = service;
    dislikeInfo_ = info;
    if (info) connect(info, &DislikeInfo::enabledChanged, this, [this] { dislikeSettingChanged(); });
}

void ShortsModel::setGraceMs(int ms) { grace_->setInterval(ms); }

void ShortsModel::setPlaybackRunning(bool running) {
    if (running == playbackRunning_) return;
    playbackRunning_ = running;
    emit holdChanged();
    if (running && !released_ && current_ >= 0) release();
}

void ShortsModel::bump() {
    ++revision_;
    emit revisionChanged();
}

void ShortsModel::release() {
    grace_->stop();
    if (released_) return;
    released_ = true;
    pump();
}

void ShortsModel::sync() {
    const int now = service_ ? static_cast<int>(service_->rows().size()) : 0;
    if (now < shown_) {
        // A refresh or an account switch: the list starts over.
        beginResetModel();
        shown_ = now;
        current_ = -1;
        released_ = false;
        grace_->stop();
        endResetModel();
    } else if (now > shown_) {
        beginInsertRows(QModelIndex(), shown_, now - 1);
        shown_ = now;
        endInsertRows();
    }
    bump();
    emit stateChanged();
}

int ShortsModel::rowCount(const QModelIndex &parent) const { return parent.isValid() ? 0 : shown_; }

QVariant ShortsModel::data(const QModelIndex &index, int role) const {
    if (!service_ || !index.isValid() || index.row() < 0 || index.row() >= shown_) return QVariant();
    const ShortsRow &r = service_->rows()[static_cast<std::size_t>(index.row())];
    const catalog::Item &it = r.item;
    const Entry *e = entryFor(q(it.id));
    const catalog::ShortDetail empty;
    const catalog::ShortDetail &d = e && e->loaded ? e->detail : empty;
    // The list row's own value wins; the details fill what the row did not carry.
    const std::string &channelName = !it.channel.empty() ? it.channel : d.channelName;
    switch (role) {
    case VideoIdRole: return q(it.id);
    case TitleRole: return q(!it.title.empty() ? it.title : d.title);
    case ChannelRole: return q(channelName);
    case ChannelIdRole: return channelIdOf(index.row(), e);
    case ChannelAvatarRole: {
        if (!r.channelAvatar.empty()) return q(r.channelAvatar);
        const QString known = ChannelAvatars::forRow(it);
        if (!known.isEmpty()) return known;
        if (!d.channelAvatar.empty()) return q(d.channelAvatar);
        return it.channelId.empty() && !d.channelId.empty() ? ChannelAvatars::instance()->get(q(d.channelId)) : QString();
    }
    case ThumbnailRole: return q(it.thumbnailUrl);
    case ThumbnailLargeRole: return q(it.thumbnailUrl);
    case ViewCountRole: return static_cast<double>(it.viewCount >= 0 ? it.viewCount : d.viewCount);
    case ViewTextRole:
        // YouTube's localized text first, else generated in the discovery language.
        if (!it.viewText.empty()) return q(it.viewText);
        if (it.viewCount >= 0) return q(catalog::formatViews(it.viewCount, catalog::displayLanguage()));
        return q(d.viewText);
    case LikeCountRole: return static_cast<double>(d.likeCount);
    case LikeTextRole: return q(!r.likeText.empty() ? r.likeText : d.likeText);
    case PublishedRole: return q(!it.publishedText.empty() ? it.publishedText : d.publishedText);
    case DurationSecondsRole: return static_cast<double>(it.durationSeconds > 0 ? it.durationSeconds : -1);
    case SourceRole: return QString::fromLatin1(sourceName(r.source));
    case CommentCountRole: return static_cast<double>(d.commentCount);
    case CommentTextRole: return q(d.commentText);
    case CommentsDisabledRole: return d.commentsDisabled;
    case DislikeCountRole: return e ? e->dislike : -1.0;
    case DislikeTextRole: return e ? e->dislikeText : QString();
    case ChannelHandleRole: return q(d.channelHandle);
    case SubscribedRole: {
        const QString id = channelIdOf(index.row(), e);
        return !id.isEmpty() && service_->isSubscribed(id.toStdString());
    }
    case DescriptionRole: return q(d.description);
    case DescriptionShortRole: return q(d.descriptionShort());
    case RelatedVideoIdRole: return q(d.relatedVideoId);
    case RelatedVideoTitleRole: return q(d.relatedVideoTitle);
    case RelatedVideoIsShortRole: return d.relatedVideoIsShort;
    case RelatedVideoThumbnailRole: return q(d.relatedVideoThumbnail());
    case SoundTextRole: return q(d.soundText);
    case DetailsLoadedRole: return e && e->loaded;
    case DetailsLoadingRole: return e && e->loading;
    case DetailsFailedRole: return e && e->failed;
    case HintsRole: {
        QVariantMap h;
        h.insert(QStringLiteral("title"), q(!it.title.empty() ? it.title : d.title));
        h.insert(QStringLiteral("channel"), q(channelName));
        h.insert(QStringLiteral("channelId"), channelIdOf(index.row(), e));
        h.insert(QStringLiteral("thumbnail"), q(it.thumbnailUrl));
        h.insert(QStringLiteral("durationSeconds"), static_cast<double>(it.durationSeconds > 0 ? it.durationSeconds : -1));
        h.insert(QStringLiteral("viewCount"), static_cast<double>(it.viewCount));
        h.insert(QStringLiteral("live"), false);
        return h;
    }
    }
    return QVariant();
}

QHash<int, QByteArray> ShortsModel::roleNames() const {
    QHash<int, QByteArray> r;
    r.insert(VideoIdRole, "videoId");
    r.insert(TitleRole, "title");
    r.insert(ChannelRole, "channel");
    r.insert(ChannelIdRole, "channelId");
    r.insert(ChannelAvatarRole, "channelAvatar");
    r.insert(ThumbnailRole, "thumbnail");
    r.insert(ThumbnailLargeRole, "thumbnailLarge");
    r.insert(ViewCountRole, "viewCount");
    r.insert(ViewTextRole, "viewText");
    r.insert(LikeCountRole, "likeCount");
    r.insert(LikeTextRole, "likeText");
    r.insert(PublishedRole, "published");
    r.insert(DurationSecondsRole, "durationSeconds");
    r.insert(SourceRole, "source");
    r.insert(HintsRole, "hints");
    r.insert(CommentCountRole, "commentCount");
    r.insert(CommentTextRole, "commentText");
    r.insert(CommentsDisabledRole, "commentsDisabled");
    r.insert(DislikeCountRole, "dislikeCount");
    r.insert(DislikeTextRole, "dislikeText");
    r.insert(ChannelHandleRole, "channelHandle");
    r.insert(SubscribedRole, "subscribed");
    r.insert(DescriptionRole, "description");
    r.insert(DescriptionShortRole, "descriptionShort");
    r.insert(RelatedVideoIdRole, "relatedVideoId");
    r.insert(RelatedVideoTitleRole, "relatedVideoTitle");
    r.insert(RelatedVideoIsShortRole, "relatedVideoIsShort");
    r.insert(RelatedVideoThumbnailRole, "relatedVideoThumbnail");
    r.insert(SoundTextRole, "soundText");
    r.insert(DetailsLoadedRole, "detailsLoaded");
    r.insert(DetailsLoadingRole, "detailsLoading");
    r.insert(DetailsFailedRole, "detailsFailed");
    return r;
}

QString ShortsModel::idAt(int row) const {
    if (!service_ || row < 0 || row >= shown_) return QString();
    return q(service_->rows()[static_cast<std::size_t>(row)].item.id);
}

const ShortsModel::Entry *ShortsModel::entryFor(const QString &videoId) const {
    const auto it = entries_.constFind(videoId);
    return it == entries_.constEnd() ? nullptr : &it.value();
}

QString ShortsModel::channelIdOf(int row, const Entry *entry) const {
    const std::string &own = service_->rows()[static_cast<std::size_t>(row)].item.channelId;
    if (!own.empty()) return q(own);
    return entry && entry->loaded ? q(entry->detail.channelId) : QString();
}

void ShortsModel::announce(const QString &videoId) {
    for (int row = 0; row < shown_; ++row)
        if (service_->rows()[static_cast<std::size_t>(row)].item.id == videoId.toStdString())
            emit dataChanged(index(row), index(row));
    bump();
}

bool ShortsModel::busy() const { return service_ && service_->busy(); }
bool ShortsModel::loaded() const { return service_ && service_->loaded(); }
bool ShortsModel::canLoadMore() const { return service_ && service_->canLoadMore(); }
bool ShortsModel::signedIn() const { return service_ && service_->signedIn(); }

QString ShortsModel::errorText() const {
    if (!service_ || service_->busy() || shown_ > 0) return QString();
    const Outcome o = service_->outcome();
    if (o == Outcome::Ok || o == Outcome::ValidEmpty) return QString();
    return QCoreApplication::translate("FeedModel", "The feed could not be refreshed. Check the connection.");
}

void ShortsModel::refresh() { if (service_) service_->refresh(); }
void ShortsModel::loadMore() { if (service_) service_->loadMore(); }

QVariantMap ShortsModel::get(int row) const {
    QVariantMap m;
    const QHash<int, QByteArray> roles = roleNames();
    for (auto it = roles.constBegin(); it != roles.constEnd(); ++it)
        m.insert(QString::fromLatin1(it.value()), data(index(row, 0), it.key()));
    return m;
}

void ShortsModel::setCurrent(int row) {
    if (row < 0 || row >= shown_ || !service_) return;
    if (intent_) {
        QStringList next;
        for (int k = row + 1; k < shown_ && next.size() < 2; ++k)
            next.append(q(service_->rows()[static_cast<std::size_t>(k)].item.id));
        if (!next.isEmpty()) intent_->announceNext(next);
    }
    if (row >= shown_ - kPreloadDistance && canLoadMore()) loadMore();
    if (row != current_) {
        current_ = row;
        released_ = false;       // a new Short: the hold starts over
        grace_->stop();
        // A failed Short is tried again on a new visit, never in a loop within one.
        for (int k = 0; k < 2; ++k) {
            const auto it = entries_.find(idAt(row + k));
            if (it != entries_.end() && it->failed) it->failed = false;
        }
    }
    pump();
}

bool ShortsModel::wantsDislike() const {
    if (!catalog::shortsDislikeEstimate()) return false;
    if (!dislikes_ || current_ < 0 || current_ >= shown_) return false;
    if (!(dislikeInfo_ ? dislikeInfo_->enabled() : dislikes_->enabled())) return false;
    const Entry *e = entryFor(idAt(current_));
    return !e || !e->dislikeAsked;
}

void ShortsModel::pump() {
    if (current_ < 0 || current_ >= shown_ || inFlight_ > 0) return;
    if (!details_ && !dislikes_) return;
    // The Short on screen first, then the next one; each only once, and only after the hold.
    for (int k = 0; details_ && k < 2; ++k) {
        const int row = current_ + k;
        if (row >= shown_) break;
        const QString id = idAt(row);
        const Entry *e = entryFor(id);
        if (e && (e->loaded || e->failed)) {
            if (k == 0 && released_) askDislike();   // the estimate follows the Short on screen at once
            continue;
        }
        if (!released_) {
            if (!grace_->isActive()) grace_->start();
            return;
        }
        requestDetails(row);
        return;
    }
    // Everything the details need is known; the estimate waits for the same hold.
    if (wantsDislike()) {
        if (!released_) {
            if (!grace_->isActive()) grace_->start();
            return;
        }
        askDislike();
    }
}

void ShortsModel::requestDetails(int row) {
    const QString id = idAt(row);
    Entry &e = entries_[id];
    e.loading = true;
    ++inFlight_;
    ++detailRequests_;
    announce(id);
    const bool sent = details_->fetch(id, this, [this, id](const catalog::ShortDetailResult &r) {
        --inFlight_;
        Entry &entry = entries_[id];
        entry.loading = false;
        if (r.outcome == catalog::Outcome::Ok) {
            entry.detail = r.detail;
            entry.loaded = true;
        } else {
            entry.failed = true;
        }
        announce(id);
        pump();
    });
    if (!sent) {
        --inFlight_;
        e.loading = false;
        e.failed = true;
        announce(id);
    }
}

void ShortsModel::askDislike() {
    if (!catalog::shortsDislikeEstimate() || !dislikes_ || current_ < 0 || current_ >= shown_) return;
    const bool on = dislikeInfo_ ? dislikeInfo_->enabled() : dislikes_->enabled();
    if (!on) return;
    const QString id = idAt(current_);
    Entry &e = entries_[id];
    if (e.dislikeAsked) return;
    e.dislikeAsked = true;
    dislikes_->fetch(id, this, [this, id](const catalog::DislikeResult &r) {
        Entry &entry = entries_[id];
        const bool ok = r.outcome == catalog::Outcome::Ok && r.votes.dislikes >= 0;
        entry.dislike = ok ? static_cast<double>(r.votes.dislikes) : -1;
        entry.dislikeText = ok ? q(catalog::formatCompactCount(r.votes.dislikes, catalog::displayLanguage())) : QString();
        announce(id);
    });
}

void ShortsModel::dislikeSettingChanged() {
    // On: ask for the Short on screen. Off: the numbers go and nothing more is asked.
    const bool on = dislikeInfo_ && dislikeInfo_->enabled();
    for (auto it = entries_.begin(); it != entries_.end(); ++it) {
        it->dislikeAsked = false;
        it->dislike = -1;
        it->dislikeText.clear();
    }
    if (shown_ > 0) emit dataChanged(index(0), index(shown_ - 1), QVector<int>() << DislikeCountRole << DislikeTextRole);
    bump();
    if (on) pump();   // the estimate of the Short on screen, after the hold
}

}}}
