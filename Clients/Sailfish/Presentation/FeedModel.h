// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#pragma once

// List models for the subscriptions feed (DSC-05/06/07) and Recommended (DSC-04). Roles are
// compatible with VideoListItem and the search model (videoId, title, channel, channelId,
// duration, durationSeconds, viewCount, thumbnail, live) plus upcoming, publishedText,
// publishedEpoch, short, membersOnly and channelAvatar (the row's own avatar, else the one
// known for its channel, else ""; see ChannelAvatars) and thumbnailLarge (the 1280x720 picture
// for full-width cards, or the 480x360 one when that is missing; see LargeThumbnails).
//
// The shown rows are replaced only when the id sequence changed (DSC-06); a change is applied
// as a splice of the differing middle part so the view keeps its scroll position.

#include "Core/Catalog/Domain/Item.h"

#include <QAbstractListModel>
#include <QString>
#include <QVariantList>
#include <QVariantMap>
#include <functional>
#include <vector>

namespace nordtile { namespace video { namespace catalog { class FeedService; class HomeService; } } }

namespace nordtile { namespace video { namespace sailfish {

class CatalogRowsModel : public QAbstractListModel {
    Q_OBJECT
public:
    enum Role { VideoIdRole = Qt::UserRole + 1, TitleRole, ChannelRole, ChannelIdRole, DurationRole,
                DurationSecondsRole, ViewCountRole, ThumbnailRole, LiveRole, UpcomingRole, PublishedTextRole,
                PublishedEpochRole, ShortRole, MembersOnlyRole, ChannelAvatarRole, ThumbnailLargeRole };
    explicit CatalogRowsModel(QObject *parent = nullptr);

    int rowCount(const QModelIndex &parent = QModelIndex()) const override;
    QVariant data(const QModelIndex &index, int role) const override;
    QHash<int, QByteArray> roleNames() const override;
    Q_INVOKABLE QVariantMap get(int row) const;

    const std::vector<catalog::Item> &rows() const { return rows_; }
    int resets() const { return resets_; }   // full replacements (tests)
    // Epoch seconds used for relative dates (tests inject it).
    void setClock(const std::function<qint64()> &clock) { clock_ = clock; }

    // Relative text for an epoch ("3 hours ago", "hace 3 horas") in the discovery language.
    static QString relativeAge(qint64 epoch, qint64 now);
    static void setLanguage(const QString &hl);
    static QString language();

protected:
    // Applies `next`: nothing when the id sequence and contents are unchanged, dataChanged
    // when only contents changed, else a splice of the differing middle part.
    void applyRows(const std::vector<catalog::Item> &next);
    qint64 now() const;

private:
    std::vector<catalog::Item> rows_;
    std::function<qint64()> clock_;
    int resets_ = 0;
};

class FeedModel final : public CatalogRowsModel {
    Q_OBJECT
    Q_PROPERTY(bool busy READ busy NOTIFY stateChanged)
    Q_PROPERTY(bool refreshing READ refreshing NOTIFY stateChanged)
    Q_PROPERTY(bool stale READ stale NOTIFY stateChanged)
    Q_PROPERTY(bool canLoadMore READ canLoadMore NOTIFY stateChanged)
    Q_PROPERTY(QString errorText READ errorText NOTIFY stateChanged)
    Q_PROPERTY(int count READ count NOTIFY stateChanged)
    Q_PROPERTY(int hiddenCount READ hiddenCount NOTIFY stateChanged)
    Q_PROPERTY(int channelCount READ channelCount NOTIFY stateChanged)
    Q_PROPERTY(bool loaded READ loaded NOTIFY stateChanged)
public:
    explicit FeedModel(catalog::FeedService *service, QObject *parent = nullptr);

    bool busy() const;
    bool refreshing() const;
    bool stale() const;
    bool canLoadMore() const;
    QString errorText() const;
    int count() const { return static_cast<int>(rows().size()); }
    int hiddenCount() const;
    int channelCount() const;
    bool loaded() const;

    // Pull-to-refresh passes force = true (every channel); otherwise only due channels.
    Q_INVOKABLE void refresh(bool force = false);
    // [{id: "UC...", name: "..."}] from the library component.
    Q_INVOKABLE void setChannels(const QVariantList &channels);
    // Infinite scroll (DSC-07).
    Q_INVOKABLE void loadMore();

signals:
    void stateChanged();

private:
    void sync();
    catalog::FeedService *service_;
};

class RecommendedModel final : public CatalogRowsModel {
    Q_OBJECT
    Q_PROPERTY(bool busy READ busy NOTIFY stateChanged)
    Q_PROPERTY(bool canLoadMore READ canLoadMore NOTIFY stateChanged)
    Q_PROPERTY(bool signedIn READ signedIn NOTIFY stateChanged)
    // DSC-04a: signed out the rows are the public "Popular" list.
    Q_PROPERTY(bool popular READ popular NOTIFY stateChanged)
    Q_PROPERTY(QString errorText READ errorText NOTIFY stateChanged)
    Q_PROPERTY(int count READ count NOTIFY stateChanged)
public:
    explicit RecommendedModel(catalog::HomeService *service, QObject *parent = nullptr);

    bool busy() const;
    bool canLoadMore() const;
    bool signedIn() const;
    bool popular() const;
    QString errorText() const;
    int count() const { return static_cast<int>(rows().size()); }

    Q_INVOKABLE void reload();
    Q_INVOKABLE void loadMore();

signals:
    void stateChanged();

private:
    void sync();
    catalog::HomeService *service_;
};

}}}
