// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "SearchModel.h"
#include "ErrorText.h"
#include "Core/Resolver/Infrastructure/YtDlpVideoService.h"

#include <QPointer>

namespace nordtile { namespace video { namespace sailfish {

namespace {
const int pageSize = 20;
const int maximumRows = 500;
}

QString formatDuration(qint64 seconds) {
    if (seconds < 0) return QString();
    const qint64 h = seconds / 3600, m = (seconds % 3600) / 60, s = seconds % 60;
    if (h > 0) return QStringLiteral("%1:%2:%3").arg(h).arg(m, 2, 10, QLatin1Char('0')).arg(s, 2, 10, QLatin1Char('0'));
    return QStringLiteral("%1:%2").arg(m).arg(s, 2, 10, QLatin1Char('0'));
}

SearchModel::SearchModel(resolver::YtDlpVideoService *service, QObject *parent)
    : QAbstractListModel(parent), service_(service) {}

SearchModel::~SearchModel() { if (operation_) service_->cancel(operation_); }

int SearchModel::rowCount(const QModelIndex &parent) const {
    return parent.isValid() ? 0 : static_cast<int>(items_.size());
}

QVariant SearchModel::data(const QModelIndex &index, int role) const {
    if (!index.isValid() || index.row() < 0 || index.row() >= static_cast<int>(items_.size())) return QVariant();
    const catalog::VideoSummary &v = items_[static_cast<std::size_t>(index.row())];
    switch (role) {
    case VideoIdRole: return QString::fromStdString(v.videoId);
    case TitleRole: return QString::fromStdString(v.title);
    case ChannelRole: return QString::fromStdString(v.channel);
    case ChannelIdRole: return QString::fromStdString(v.channelId);
    case DurationRole: return formatDuration(v.durationSeconds);
    case DurationSecondsRole: return static_cast<double>(v.durationSeconds);
    case ViewCountRole: return static_cast<double>(v.viewCount);
    case ThumbnailRole: return QString::fromStdString(v.thumbnailUrl);
    case LiveRole: return v.liveStatus == media::LiveStatus::IsLive;
    default: return QVariant();
    }
}

QHash<int, QByteArray> SearchModel::roleNames() const {
    return {
        { VideoIdRole, "videoId" }, { TitleRole, "title" }, { ChannelRole, "channel" },
        { ChannelIdRole, "channelId" }, { DurationRole, "duration" }, { DurationSecondsRole, "durationSeconds" },
        { ViewCountRole, "viewCount" }, { ThumbnailRole, "thumbnail" }, { LiveRole, "live" }
    };
}

QVariantMap SearchModel::get(int row) const {
    QVariantMap m;
    const QHash<int, QByteArray> roles = roleNames();
    for (auto it = roles.constBegin(); it != roles.constEnd(); ++it)
        m.insert(QString::fromLatin1(it.value()), data(index(row, 0), it.key()));
    return m;
}

void SearchModel::search(const QString &query) {
    const QString q = resolver::YtDlpVideoService::normalizedQuery(query);
    cancel();
    beginResetModel();
    items_.clear();
    endResetModel();
    query_ = q;
    errorText_.clear();
    canLoadMore_ = false;
    emit stateChanged();
    if (!q.isEmpty()) request(0);
}

void SearchModel::loadMore() {
    if (!canLoadMore()) return;
    request(static_cast<int>(items_.size()));
}

void SearchModel::cancel() {
    if (!operation_) return;
    service_->cancel(operation_);
    operation_ = 0;
    ++generation_;
    emit stateChanged();
}

void SearchModel::request(int offset) {
    const quint64 generation = ++generation_;
    errorText_.clear();
    QPointer<SearchModel> guard(this);
    operation_ = service_->search(query_, offset, pageSize,
                                  [this, guard, generation](resolver::ErrorCode error, const std::vector<catalog::VideoSummary> &found) {
        if (!guard || generation != generation_) return;
        operation_ = 0;
        if (error != resolver::ErrorCode::None) {
            errorText_ = sailfish::errorText(error);
            emit stateChanged();
            return;
        }
        std::vector<catalog::VideoSummary> fresh;
        for (const catalog::VideoSummary &v : found) {
            bool seen = false;
            for (const catalog::VideoSummary &old : items_) seen = seen || old.videoId == v.videoId;
            if (!seen) fresh.push_back(v);
        }
        if (!fresh.empty()) {
            const int first = static_cast<int>(items_.size());
            beginInsertRows(QModelIndex(), first, first + static_cast<int>(fresh.size()) - 1);
            items_.insert(items_.end(), fresh.begin(), fresh.end());
            endInsertRows();
        }
        canLoadMore_ = !found.empty() && static_cast<int>(items_.size()) < maximumRows;
        emit stateChanged();
    });
    emit stateChanged();
}

}}}
