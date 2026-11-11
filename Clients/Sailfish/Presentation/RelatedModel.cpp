// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "RelatedModel.h"
#include "ChannelAvatars.h"
#include "LargeThumbnails.h"
#include "Core/Catalog/Domain/Images.h"
#include "Core/Catalog/Infrastructure/WatchNextService.h"

#include <QCoreApplication>

namespace nordtile { namespace video { namespace sailfish {

namespace {
QString q(const std::string &s) { return QString::fromStdString(s); }

QString durationText(qint64 seconds) {
    if (seconds < 0) return QString();
    const qint64 h = seconds / 3600, m = (seconds % 3600) / 60, s = seconds % 60;
    if (h > 0) return QStringLiteral("%1:%2:%3").arg(h).arg(m, 2, 10, QLatin1Char('0')).arg(s, 2, 10, QLatin1Char('0'));
    return QStringLiteral("%1:%2").arg(m).arg(s, 2, 10, QLatin1Char('0'));
}

}

QString watchFailureText(catalog::Outcome o) {
    using catalog::Outcome;
    switch (o) {
    case Outcome::Network: return QCoreApplication::translate("SailTubeWatch", "YouTube could not be reached. Check the connection.");
    case Outcome::RateLimited: return QCoreApplication::translate("SailTubeWatch", "YouTube is limiting requests. Try again later.");
    case Outcome::Http5xx: return QCoreApplication::translate("SailTubeWatch", "YouTube had a temporary problem. Try again.");
    case Outcome::Http4xx: return QCoreApplication::translate("SailTubeWatch", "YouTube refused the request. Try again.");
    case Outcome::ShapeDrift: return QCoreApplication::translate("SailTubeWatch", "YouTube changed its page layout; this part of the page cannot be read.");
    case Outcome::Unavailable: return QCoreApplication::translate("SailTubeWatch", "This video is not available.");
    default: return QString();
    }
}

RelatedModel::RelatedModel(catalog::WatchNextService *service, QObject *parent)
    : QAbstractListModel(parent) {
    ChannelAvatars::follow(this, ChannelAvatarRole);
    LargeThumbnails::follow(this, ThumbnailLargeRole);
    setService(service);
}

void RelatedModel::setService(catalog::WatchNextService *service) {
    if (service_ == service) return;
    if (service_) disconnect(service_, nullptr, this, nullptr);
    service_ = service;
    if (service_) connect(service_, &catalog::WatchNextService::relatedChanged, this, &RelatedModel::sync);
}

int RelatedModel::rowCount(const QModelIndex &parent) const {
    return parent.isValid() ? 0 : count();
}

QVariant RelatedModel::data(const QModelIndex &index, int role) const {
    if (!index.isValid() || index.row() < 0 || index.row() >= count()) return QVariant();
    const catalog::Item &v = rows_[static_cast<std::size_t>(index.row())];
    switch (role) {
    case VideoIdRole: return q(v.id);
    case TitleRole: return q(v.title);
    case ChannelRole: return q(v.channel);
    case ChannelIdRole: return q(v.channelId);
    case DurationRole: return v.durationSeconds >= 0 ? durationText(v.durationSeconds) : q(v.durationText);
    case DurationSecondsRole: return static_cast<double>(v.durationSeconds);
    case ViewCountRole: return static_cast<double>(v.viewCount);
    case ThumbnailRole: return q(catalog::listThumbnailUrl(v.id));
    case LiveRole: return v.liveStatus == media::LiveStatus::IsLive;
    case ViewTextRole: return q(v.viewText);
    case PublishedTextRole: return q(v.publishedText);
    case MembersOnlyRole: return v.membersOnly;
    case ShortRole: return catalog::isConfirmedShort(v.shortEvidence);
    case UpcomingRole: return v.liveStatus == media::LiveStatus::Upcoming;
    case ChannelAvatarRole: return ChannelAvatars::forRow(v);
    case ThumbnailLargeRole: return LargeThumbnails::forThumbnail(catalog::listThumbnailUrl(v.id));
    default: return QVariant();
    }
}

QHash<int, QByteArray> RelatedModel::roleNames() const {
    return {
        {VideoIdRole, "videoId"}, {TitleRole, "title"}, {ChannelRole, "channel"}, {ChannelIdRole, "channelId"},
        {DurationRole, "duration"}, {DurationSecondsRole, "durationSeconds"}, {ViewCountRole, "viewCount"},
        {ThumbnailRole, "thumbnail"}, {LiveRole, "live"}, {ViewTextRole, "viewText"},
        {PublishedTextRole, "publishedText"}, {MembersOnlyRole, "membersOnly"}, {ShortRole, "isShort"},
        {UpcomingRole, "upcoming"}, {ChannelAvatarRole, "channelAvatar"},
        {ThumbnailLargeRole, "thumbnailLarge"}};
}

void RelatedModel::setVideo(const QString &videoId) {
    if (videoId == videoId_) { sync(videoId); return; }
    beginResetModel();
    rows_.clear();
    videoId_ = videoId;
    endResetModel();
    busy_ = canLoadMore_ = false;
    hidden_ = 0;
    errorText_.clear();
    sync(videoId);
    emit stateChanged();
}

void RelatedModel::sync(const QString &videoId) {
    if (videoId != videoId_ || !service_) return;
    const std::shared_ptr<const catalog::WatchPage> page = service_->page(videoId);
    if (!page) return;
    const bool busy = page->nextPending || page->relatedPending;
    const bool more = !page->relatedContinuation.empty();
    const int hidden = page->hiddenShorts + page->hiddenMembersOnly + page->hiddenRestricted;
    QString error;
    if (page->nextDone && catalog::isFailure(page->next.relatedOutcome)) error = watchFailureText(page->next.relatedOutcome);
    else if (catalog::isFailure(page->relatedPageOutcome)) error = watchFailureText(page->relatedPageOutcome);
    if (page->related.size() < rows_.size()) {   // a refetched page: start over
        beginResetModel();
        rows_.clear();
        endResetModel();
    }
    if (page->related.size() > rows_.size()) {
        const int first = count();
        beginInsertRows(QModelIndex(), first, static_cast<int>(page->related.size()) - 1);
        rows_.insert(rows_.end(), page->related.begin() + static_cast<std::ptrdiff_t>(rows_.size()), page->related.end());
        endInsertRows();
    }
    busy_ = busy;
    canLoadMore_ = more;
    hidden_ = hidden;
    errorText_ = error;
    emit stateChanged();
}

void RelatedModel::loadMore() {
    if (!service_ || videoId_.isEmpty() || !canLoadMore()) return;
    if (service_->loadMoreRelated(videoId_)) {
        busy_ = true;
        emit stateChanged();
    }
}

QVariantMap RelatedModel::get(int row) const {
    QVariantMap m;
    const QHash<int, QByteArray> roles = roleNames();
    for (auto it = roles.constBegin(); it != roles.constEnd(); ++it)
        m.insert(QString::fromLatin1(it.value()), data(index(row, 0), it.key()));
    return m;
}

}}}
