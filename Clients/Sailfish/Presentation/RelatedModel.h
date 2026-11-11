// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#pragma once

// Related videos of one watch page (DSC-13): rows from watch-next secondaryResults, extended by
// loadMore() through the continuation (60+ reachable). The seed, filtered and duplicate rows
// never appear (WatchNextService does the filtering). Role names match SearchModel so the same
// list delegate and playback.open(videoId, hints) work.

#include "Core/Catalog/Domain/Item.h"
#include "Core/Catalog/Domain/Outcome.h"

#include <QAbstractListModel>
#include <QPointer>
#include <QString>
#include <QVariantMap>
#include <vector>

namespace nordtile { namespace video {
namespace catalog { class WatchNextService; }
namespace sailfish {

// Fixed, translatable text for a failed watch-page call (provider text is never shown).
QString watchFailureText(catalog::Outcome outcome);

class RelatedModel final : public QAbstractListModel {
    Q_OBJECT
    Q_PROPERTY(QString videoId READ videoId NOTIFY stateChanged)
    Q_PROPERTY(int count READ count NOTIFY stateChanged)
    Q_PROPERTY(bool busy READ busy NOTIFY stateChanged)
    Q_PROPERTY(bool canLoadMore READ canLoadMore NOTIFY stateChanged)
    Q_PROPERTY(int hiddenCount READ hiddenCount NOTIFY stateChanged)
    Q_PROPERTY(QString errorText READ errorText NOTIFY stateChanged)
public:
    enum Role { VideoIdRole = Qt::UserRole + 1, TitleRole, ChannelRole, ChannelIdRole, DurationRole,
                DurationSecondsRole, ViewCountRole, ThumbnailRole, LiveRole, ViewTextRole, PublishedTextRole,
                MembersOnlyRole, ShortRole, UpcomingRole, ChannelAvatarRole, ThumbnailLargeRole };
    explicit RelatedModel(catalog::WatchNextService *service, QObject *parent = nullptr);

    int rowCount(const QModelIndex &parent = QModelIndex()) const override;
    QVariant data(const QModelIndex &index, int role) const override;
    QHash<int, QByteArray> roleNames() const override;

    QString videoId() const { return videoId_; }
    int count() const { return static_cast<int>(rows_.size()); }
    bool busy() const { return busy_; }
    bool canLoadMore() const { return canLoadMore_ && !busy_; }
    int hiddenCount() const { return hidden_; }
    QString errorText() const { return errorText_; }

    // Shows the related rows of `videoId` (the service request is started by WatchInfo).
    void setVideo(const QString &videoId);
    void setService(catalog::WatchNextService *service);

    Q_INVOKABLE void loadMore();
    Q_INVOKABLE QVariantMap get(int row) const;

signals:
    void stateChanged();

private:
    void sync(const QString &videoId);
    QPointer<catalog::WatchNextService> service_;
    QString videoId_;
    std::vector<catalog::Item> rows_;
    bool busy_ = false;
    bool canLoadMore_ = false;
    int hidden_ = 0;
    QString errorText_;
};

}}}
