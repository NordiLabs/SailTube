// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#pragma once

// The remote playlist preview (LIB-03, LNK-04): header (title, owner, count) and rows in
// playlist order, paged with loadMore() and never capped. Nothing is saved here: Save belongs
// to the library component. Rows are not filtered (positions must match the playlist); members-only
// and restricted rows carry badges. get(row) includes {playlistId, index} for the queue hint
// of PlaybackSession::open().
//
// QML creates one per page (qmlRegisterType); the app sets the shared PlaylistService once with
// setDefaultService().

#include "Core/Catalog/Domain/Item.h"
#include "Core/Catalog/Domain/Outcome.h"

#include <QAbstractListModel>
#include <QPointer>
#include <QString>
#include <QVariantMap>
#include <string>
#include <vector>

namespace nordtile { namespace video { namespace catalog { class PlaylistService; struct PlaylistPage; } } }

namespace nordtile { namespace video { namespace sailfish {

class PlaylistModel : public QAbstractListModel {
    Q_OBJECT
    Q_PROPERTY(QString playlistId READ playlistId WRITE open NOTIFY headerChanged)
    Q_PROPERTY(QString title READ title NOTIFY headerChanged)
    Q_PROPERTY(QString owner READ owner NOTIFY headerChanged)
    Q_PROPERTY(QString ownerId READ ownerId NOTIFY headerChanged)
    Q_PROPERTY(QString videoCountText READ videoCountText NOTIFY headerChanged)
    Q_PROPERTY(double videoCount READ videoCount NOTIFY headerChanged)
    Q_PROPERTY(QString viewText READ viewText NOTIFY headerChanged)
    Q_PROPERTY(QString thumbnail READ thumbnail NOTIFY headerChanged)
    Q_PROPERTY(bool busy READ busy NOTIFY stateChanged)
    Q_PROPERTY(bool canLoadMore READ canLoadMore NOTIFY stateChanged)
    Q_PROPERTY(QString errorText READ errorText NOTIFY stateChanged)
    Q_PROPERTY(QString outcome READ outcome NOTIFY stateChanged)
    Q_PROPERTY(bool empty READ empty NOTIFY stateChanged)
    Q_PROPERTY(int count READ count NOTIFY stateChanged)
public:
    enum Role {
        VideoIdRole = Qt::UserRole + 1, TitleRole, ChannelRole, ChannelIdRole, DurationRole, DurationSecondsRole,
        ViewCountRole, ThumbnailRole, LiveRole, UpcomingRole, MembersOnlyRole, RestrictedRole, IndexRole, PlaylistIdRole,
        ChannelAvatarRole, ThumbnailLargeRole
    };

    static void setDefaultService(catalog::PlaylistService *service);
    // ACC-11: the account's own lists ("WL" Watch later, "LL" Liked) go through the signed
    // service; null (signed out) makes them unavailable with a sign-in hint.
    static void setPersonalService(catalog::PlaylistService *service);
    static bool isPersonalList(const QString &listId);
    // A sign-out or account switch: open personal lists reload (or empty) themselves.
    static void invalidatePersonal();

    explicit PlaylistModel(QObject *parent = nullptr);
    explicit PlaylistModel(catalog::PlaylistService *service, QObject *parent = nullptr);
    ~PlaylistModel() override;

    int rowCount(const QModelIndex &parent = QModelIndex()) const override;
    QVariant data(const QModelIndex &index, int role) const override;
    QHash<int, QByteArray> roleNames() const override;

    QString playlistId() const { return playlistId_; }
    QString title() const { return title_; }
    QString owner() const { return owner_; }
    QString ownerId() const { return ownerId_; }
    QString videoCountText() const { return videoCountText_; }
    double videoCount() const { return static_cast<double>(videoCount_); }
    QString viewText() const { return viewText_; }
    QString thumbnail() const { return thumbnail_; }
    bool busy() const { return loading_; }
    bool canLoadMore() const { return !loading_ && !continuation_.isEmpty(); }
    QString errorText() const;
    QString outcome() const { return QString::fromLatin1(catalog::outcomeName(outcome_)); }
    bool empty() const { return loaded_ && !loading_ && rows_.empty() && outcome_ == catalog::Outcome::ValidEmpty; }
    int count() const { return static_cast<int>(rows_.size()); }

    Q_INVOKABLE void open(const QString &listId);
    Q_INVOKABLE void loadMore();
    Q_INVOKABLE QVariantMap get(int row) const;

    const std::vector<catalog::Item> &rows() const { return rows_; }

signals:
    void headerChanged();
    void stateChanged();

private:
    void onPage(quint64 generation, bool first, const catalog::PlaylistPage &page);
    catalog::PlaylistService *serviceFor() const;
    QPointer<catalog::PlaylistService> service_;
    quint64 generation_ = 0;
    QString playlistId_, title_, owner_, ownerId_, videoCountText_, viewText_, thumbnail_;
    qint64 videoCount_ = -1;
    std::vector<catalog::Item> rows_;
    QString continuation_;
    catalog::Outcome outcome_ = catalog::Outcome::Ok;
    bool loading_ = false;
    bool loaded_ = false;
    int emptyPages_ = 0;
};

}}}
