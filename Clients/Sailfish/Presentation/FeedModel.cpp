// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "FeedModel.h"
#include "ChannelAvatars.h"
#include "LargeThumbnails.h"
#include "Core/Catalog/Domain/LocalizedText.h"

#include "Core/Catalog/Domain/FeedSchedule.h"
#include "Core/Catalog/Infrastructure/FeedService.h"
#include "Core/Catalog/Infrastructure/HomeService.h"

#include <QCoreApplication>
#include <QDateTime>

namespace nordtile { namespace video { namespace sailfish {

using catalog::Item;

namespace {

QString durationText(const Item &it) {
    const qint64 seconds = it.durationSeconds;
    if (seconds < 0) return QString::fromStdString(it.durationText);
    const qint64 h = seconds / 3600, m = (seconds % 3600) / 60, s = seconds % 60;
    if (h > 0) return QStringLiteral("%1:%2:%3").arg(h).arg(m, 2, 10, QLatin1Char('0')).arg(s, 2, 10, QLatin1Char('0'));
    return QStringLiteral("%1:%2").arg(m).arg(s, 2, 10, QLatin1Char('0'));
}

bool sameContent(const Item &a, const Item &b) {
    return a.id == b.id && a.kind == b.kind && a.title == b.title && a.channel == b.channel &&
           a.durationSeconds == b.durationSeconds && a.viewCount == b.viewCount && a.liveStatus == b.liveStatus &&
           a.publishedEpoch == b.publishedEpoch && a.publishedText == b.publishedText && a.thumbnailUrl == b.thumbnailUrl &&
           a.membersOnly == b.membersOnly;
}

}  // namespace

CatalogRowsModel::CatalogRowsModel(QObject *parent) : QAbstractListModel(parent) {
    ChannelAvatars::follow(this, ChannelAvatarRole);
    LargeThumbnails::follow(this, ThumbnailLargeRole);
}

qint64 CatalogRowsModel::now() const { return clock_ ? clock_() : QDateTime::currentMSecsSinceEpoch() / 1000; }

int CatalogRowsModel::rowCount(const QModelIndex &parent) const {
    return parent.isValid() ? 0 : static_cast<int>(rows_.size());
}

void CatalogRowsModel::setLanguage(const QString &hl) { catalog::setDisplayLanguage(hl.toStdString()); }
QString CatalogRowsModel::language() { return QString::fromStdString(catalog::displayLanguage()); }

QString CatalogRowsModel::relativeAge(qint64 epoch, qint64 now) {
    if (epoch < 0) return QString();
    // In the discovery language, as YouTube writes it in the same lists ("hace 3 días").
    return QString::fromStdString(catalog::formatRelativeAge(qMax<qint64>(0, now - epoch), language().toStdString()));
}

QVariant CatalogRowsModel::data(const QModelIndex &index, int role) const {
    if (!index.isValid() || index.row() < 0 || index.row() >= static_cast<int>(rows_.size())) return QVariant();
    const Item &it = rows_[static_cast<std::size_t>(index.row())];
    const bool live = it.liveStatus == media::LiveStatus::IsLive;
    const bool upcoming = it.liveStatus == media::LiveStatus::Upcoming;
    switch (role) {
    case VideoIdRole: return QString::fromStdString(it.id);
    case TitleRole: return QString::fromStdString(it.title);
    case ChannelRole: return QString::fromStdString(it.channel);
    case ChannelIdRole: return QString::fromStdString(it.channelId);
    case DurationRole: return live || upcoming ? QString() : durationText(it);
    case DurationSecondsRole: return static_cast<double>(it.durationSeconds);
    case ViewCountRole: return static_cast<double>(it.viewCount);
    case ThumbnailRole: return QString::fromStdString(it.thumbnailUrl);
    case LiveRole: return live;
    case UpcomingRole: return upcoming;
    case PublishedTextRole:
        if (live) return QString();
        if (upcoming) {
            if (it.publishedEpoch > now()) {
                //: Start time of an upcoming live stream or premiere, %1 is a date and time
                return QCoreApplication::translate("FeedModel", "Scheduled for %1")
                    .arg(QDateTime::fromMSecsSinceEpoch(it.publishedEpoch * 1000).toString(Qt::DefaultLocaleShortDate));
            }
            return QCoreApplication::translate("FeedModel", "Upcoming");
        }
        if (it.publishedEpoch >= 0) return relativeAge(it.publishedEpoch, now());
        return QString::fromStdString(it.publishedText);
    case PublishedEpochRole: return static_cast<double>(it.publishedEpoch);
    case ShortRole: return it.kind == catalog::ItemKind::Short;
    case MembersOnlyRole: return it.membersOnly;
    case ChannelAvatarRole: return ChannelAvatars::forRow(it);
    case ThumbnailLargeRole: return LargeThumbnails::forThumbnail(it.thumbnailUrl);
    default: return QVariant();
    }
}

QHash<int, QByteArray> CatalogRowsModel::roleNames() const {
    QHash<int, QByteArray> r;
    r.insert(VideoIdRole, "videoId");
    r.insert(TitleRole, "title");
    r.insert(ChannelRole, "channel");
    r.insert(ChannelIdRole, "channelId");
    r.insert(DurationRole, "duration");
    r.insert(DurationSecondsRole, "durationSeconds");
    r.insert(ViewCountRole, "viewCount");
    r.insert(ThumbnailRole, "thumbnail");
    r.insert(LiveRole, "live");
    r.insert(UpcomingRole, "upcoming");
    r.insert(PublishedTextRole, "publishedText");
    r.insert(PublishedEpochRole, "publishedEpoch");
    r.insert(ShortRole, "short");
    r.insert(MembersOnlyRole, "membersOnly");
    r.insert(ChannelAvatarRole, "channelAvatar");
    r.insert(ThumbnailLargeRole, "thumbnailLarge");
    return r;
}

QVariantMap CatalogRowsModel::get(int row) const {
    QVariantMap m;
    const QHash<int, QByteArray> roles = roleNames();
    for (auto it = roles.constBegin(); it != roles.constEnd(); ++it)
        m.insert(QString::fromLatin1(it.value()), data(index(row, 0), it.key()));
    return m;
}

void CatalogRowsModel::applyRows(const std::vector<Item> &next) {
    const catalog::SequenceDiff d = catalog::diffIdSequences(rows_, next);
    if (d.changed) {
        ++resets_;
        const int prefix = static_cast<int>(d.prefix);
        const int oldMid = static_cast<int>(rows_.size() - d.prefix - d.suffix);
        const int newMid = static_cast<int>(next.size() - d.prefix - d.suffix);
        if (oldMid > 0) {
            beginRemoveRows(QModelIndex(), prefix, prefix + oldMid - 1);
            rows_.erase(rows_.begin() + prefix, rows_.begin() + prefix + oldMid);
            endRemoveRows();
        }
        if (newMid > 0) {
            beginInsertRows(QModelIndex(), prefix, prefix + newMid - 1);
            rows_.insert(rows_.begin() + prefix, next.begin() + prefix, next.begin() + prefix + newMid);
            endInsertRows();
        }
    }
    // Same id sequence now: update changed contents in place.
    int first = -1, last = -1;
    for (std::size_t i = 0; i < rows_.size() && i < next.size(); ++i) {
        if (sameContent(rows_[i], next[i])) continue;
        rows_[i] = next[i];
        if (first < 0) first = static_cast<int>(i);
        last = static_cast<int>(i);
    }
    if (first >= 0) emit dataChanged(index(first, 0), index(last, 0));
}

// ---------------------------------------------------------------------------------------------

FeedModel::FeedModel(catalog::FeedService *service, QObject *parent) : CatalogRowsModel(parent), service_(service) {
    connect(service_, &catalog::FeedService::feedChanged, this, [this] { sync(); });
    connect(service_, &catalog::FeedService::stateChanged, this, [this] { emit stateChanged(); });
    sync();
}

void FeedModel::sync() {
    applyRows(service_->merged().rows);
    emit stateChanged();
}

bool FeedModel::busy() const { return service_->refreshing() || service_->extending() || !service_->cacheLoaded(); }
bool FeedModel::refreshing() const { return service_->refreshing(); }
bool FeedModel::stale() const { return service_->stale(); }
bool FeedModel::canLoadMore() const { return service_->canExtend(); }
int FeedModel::hiddenCount() const { return service_->merged().hiddenTotal(); }
int FeedModel::channelCount() const { return service_->channelCount(); }
bool FeedModel::loaded() const { return service_->cacheLoaded(); }

QString FeedModel::errorText() const {
    if (!service_->refreshing() && service_->lastRoundAllFailed())
        return QCoreApplication::translate("FeedModel", "The feed could not be refreshed. Check the connection.");
    return QString();
}

void FeedModel::refresh(bool force) { service_->refresh(force); }

void FeedModel::setChannels(const QVariantList &channels) {
    catalog::ChannelList list;
    for (const QVariant &v : channels) {
        const QVariantMap m = v.toMap();
        list.push_back(catalog::ChannelRef(m.value(QStringLiteral("id")).toString().toStdString(),
                                           m.value(QStringLiteral("name")).toString().toStdString()));
    }
    service_->setChannels(list);
}

void FeedModel::loadMore() { service_->extend(); }

// ---------------------------------------------------------------------------------------------

RecommendedModel::RecommendedModel(catalog::HomeService *service, QObject *parent) : CatalogRowsModel(parent), service_(service) {
    connect(service_, &catalog::HomeService::changed, this, [this] { sync(); });
    sync();
}

void RecommendedModel::sync() {
    applyRows(service_->items());
    emit stateChanged();
}

bool RecommendedModel::busy() const { return service_->busy(); }
bool RecommendedModel::canLoadMore() const { return service_->canLoadMore(); }
bool RecommendedModel::signedIn() const { return service_->signedIn(); }
bool RecommendedModel::popular() const { return service_->popular(); }

QString RecommendedModel::errorText() const {
    const catalog::Outcome o = service_->outcome();
    if (service_->busy() || o == catalog::Outcome::Ok || o == catalog::Outcome::ValidEmpty) return QString();
    return QCoreApplication::translate("FeedModel", "Recommendations could not be loaded. Try again.");
}

void RecommendedModel::reload() { service_->load(); }
void RecommendedModel::loadMore() { service_->loadMore(); }

}}}
