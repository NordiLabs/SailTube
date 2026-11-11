// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "PlaylistModel.h"
#include <QSet>
#include "ChannelModel.h"
#include "ChannelAvatars.h"
#include "LargeThumbnails.h"
#include "Core/Catalog/Infrastructure/PlaylistService.h"

#include <QCoreApplication>

namespace nordtile { namespace video { namespace sailfish {

using catalog::Item;
using catalog::Outcome;
using catalog::PlaylistPage;

namespace {
QPointer<catalog::PlaylistService> &defaultService() {
    static QPointer<catalog::PlaylistService> service;
    return service;
}
const int kMaxEmptyPages = 5;
}

void PlaylistModel::setDefaultService(catalog::PlaylistService *service) { defaultService() = service; }

namespace {
QPointer<catalog::PlaylistService> &personalService() {
    static QPointer<catalog::PlaylistService> service;
    return service;
}
// The live models (main thread), so a personal-list change can reach the open ones.
QSet<PlaylistModel *> &liveModels() {
    static QSet<PlaylistModel *> models;
    return models;
}
}

void PlaylistModel::setPersonalService(catalog::PlaylistService *service) {
    personalService() = service;
    invalidatePersonal();
}

bool PlaylistModel::isPersonalList(const QString &listId) {
    const QString id = catalog::PlaylistService::normalizeListId(listId);
    return id == QLatin1String("WL") || id == QLatin1String("LL");
}

void PlaylistModel::invalidatePersonal() {
    const QSet<PlaylistModel *> models = liveModels();
    for (PlaylistModel *m : models)
        if (liveModels().contains(m) && isPersonalList(m->playlistId())) m->open(m->playlistId());
}

PlaylistModel::PlaylistModel(QObject *parent) : PlaylistModel(defaultService().data(), parent) {}

PlaylistModel::PlaylistModel(catalog::PlaylistService *service, QObject *parent)
    : QAbstractListModel(parent), service_(service) {
    liveModels().insert(this);
    ChannelAvatars::follow(this, ChannelAvatarRole);
    LargeThumbnails::follow(this, ThumbnailLargeRole);
}

PlaylistModel::~PlaylistModel() { liveModels().remove(this); }

int PlaylistModel::rowCount(const QModelIndex &parent) const {
    return parent.isValid() ? 0 : static_cast<int>(rows_.size());
}

QVariant PlaylistModel::data(const QModelIndex &index, int role) const {
    if (!index.isValid() || index.row() < 0 || index.row() >= static_cast<int>(rows_.size())) return QVariant();
    const Item &v = rows_[static_cast<std::size_t>(index.row())];
    switch (role) {
    case VideoIdRole: return QString::fromStdString(v.id);
    case TitleRole: return QString::fromStdString(v.title);
    case ChannelRole: return QString::fromStdString(v.channel);
    case ChannelIdRole: return QString::fromStdString(v.channelId);
    case DurationRole: return v.liveStatus == media::LiveStatus::IsLive ? QString() : catalogDurationText(v);
    case DurationSecondsRole: return static_cast<double>(v.durationSeconds);
    case ViewCountRole: return static_cast<double>(v.viewCount);
    case ThumbnailRole: return QString::fromStdString(v.thumbnailUrl);
    case LiveRole: return v.liveStatus == media::LiveStatus::IsLive;
    case UpcomingRole: return v.liveStatus == media::LiveStatus::Upcoming;
    case MembersOnlyRole: return v.membersOnly;
    case RestrictedRole: return v.restricted;
    case IndexRole: return v.playlistIndex >= 0 ? v.playlistIndex : index.row();
    case PlaylistIdRole: return playlistId_;
    case ChannelAvatarRole: return ChannelAvatars::forRow(v);
    case ThumbnailLargeRole: return LargeThumbnails::forThumbnail(v.thumbnailUrl);
    default: return QVariant();
    }
}

QHash<int, QByteArray> PlaylistModel::roleNames() const {
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
    r.insert(MembersOnlyRole, "membersOnly");
    r.insert(RestrictedRole, "restricted");
    r.insert(IndexRole, "index");
    r.insert(PlaylistIdRole, "playlistId");
    r.insert(ChannelAvatarRole, "channelAvatar");
    r.insert(ThumbnailLargeRole, "thumbnailLarge");
    return r;
}

QVariantMap PlaylistModel::get(int row) const {
    QVariantMap m;
    const QHash<int, QByteArray> roles = roleNames();
    for (auto it = roles.constBegin(); it != roles.constEnd(); ++it)
        m.insert(QString::fromLatin1(it.value()), data(index(row, 0), it.key()));
    return m;
}

catalog::PlaylistService *PlaylistModel::serviceFor() const {
    return isPersonalList(playlistId_) ? personalService().data() : service_.data();
}

QString PlaylistModel::errorText() const {
    if (outcome_ == Outcome::Unavailable && isPersonalList(playlistId_) && !personalService())
        return QCoreApplication::translate("SailTubeCatalog", "Sign in to see this list.");
    if (outcome_ == Outcome::Unavailable)
        return QCoreApplication::translate("SailTubeCatalog", "This playlist does not exist or is private.");
    return catalogOutcomeText(outcome_);
}

void PlaylistModel::open(const QString &listId) {
    ++generation_;
    beginResetModel();
    rows_.clear();
    endResetModel();
    playlistId_ = catalog::PlaylistService::normalizeListId(listId);
    title_.clear(); owner_.clear(); ownerId_.clear(); videoCountText_.clear(); viewText_.clear(); thumbnail_.clear();
    videoCount_ = -1;
    continuation_.clear();
    outcome_ = Outcome::Ok;
    loaded_ = false;
    emptyPages_ = 0;
    emit headerChanged();
    catalog::PlaylistService *svc = serviceFor();
    if (!svc || playlistId_.isEmpty()) {
        outcome_ = Outcome::Unavailable;
        loaded_ = true;
        emit stateChanged();
        return;
    }
    loading_ = true;
    emit stateChanged();
    const quint64 generation = generation_;
    svc->open(playlistId_, this, [this, generation](PlaylistPage page) { onPage(generation, true, page); });
}

void PlaylistModel::loadMore() {
    catalog::PlaylistService *svc = serviceFor();
    if (!canLoadMore() || !svc) return;
    loading_ = true;
    const quint64 generation = generation_;
    const int next = rows_.empty() ? 0 : (rows_.back().playlistIndex >= 0 ? rows_.back().playlistIndex + 1 : count());
    svc->more(playlistId_, continuation_, next, this, [this, generation](PlaylistPage page) { onPage(generation, false, page); });
    emit stateChanged();
}

void PlaylistModel::onPage(quint64 generation, bool first, const PlaylistPage &page) {
    if (generation != generation_) return;
    loading_ = false;
    loaded_ = true;
    if (first) {
        title_ = page.title;
        owner_ = page.owner;
        ownerId_ = page.ownerId;
        videoCountText_ = page.videoCountText;
        videoCount_ = page.videoCount;
        viewText_ = page.viewText;
        thumbnail_ = page.thumbnailUrl;
        emit headerChanged();
    }
    if (catalog::isFailure(page.outcome)) {
        outcome_ = page.outcome;   // the token stays: loadMore() retries on request only
        emit stateChanged();
        return;
    }
    if (!page.items.empty()) {
        const int at = static_cast<int>(rows_.size());
        beginInsertRows(QModelIndex(), at, at + static_cast<int>(page.items.size()) - 1);
        rows_.insert(rows_.end(), page.items.begin(), page.items.end());
        endInsertRows();
    }
    outcome_ = rows_.empty() ? Outcome::ValidEmpty : Outcome::Ok;
    continuation_ = page.continuation;
    // A page without rows that still links on (rare) is followed, a bounded number of times.
    emptyPages_ = page.items.empty() ? emptyPages_ + 1 : 0;
    if (page.items.empty() && emptyPages_ >= kMaxEmptyPages) continuation_.clear();
    emit stateChanged();
    if (page.items.empty() && !continuation_.isEmpty()) loadMore();
}

}}}
