// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#pragma once

// The channel page (DSC-12, DSC-17, DSC-18): header properties, the current tab and its rows.
//
//  * open(reference) resolves the reference, paints the channel's RSS rows at once (Videos tab),
//    then merges InnerTube page 1 by id: rows already shown keep their place and are updated in
//    place, new rows are appended, so the scroll position never moves.
//  * Tabs: videos, shorts, streams ("live" is accepted), playlists, releases. Each tab keeps its
//    rows and continuation while the page lives; switching back does not refetch.
//  * A channel without a Videos tab opens Playlists, else Releases (`fallbackTab`).
//  * Rows go through the one row filter: hideShorts hides confirmed Shorts only (never on the
//    Shorts tab), hideMembersOnly / hideRestricted hide badged rows, and every hidden row is
//    counted (hiddenCount). When the background Shorts check settles candidates the list is
//    re-filtered in place.
//  * loadMore() follows the tab's continuation; a page that adds no visible row fetches the
//    next one automatically, up to 5 in a row (DSC-09).
//
// QML creates one per page (qmlRegisterType); the app sets the shared ChannelService once with
// setDefaultService().

#include "Core/Catalog/Domain/Item.h"
#include "Core/Catalog/Domain/Outcome.h"
#include "Core/Catalog/Domain/RowFilter.h"

#include <QAbstractListModel>
#include <QMap>
#include <QPointer>
#include <QString>
#include <QStringList>
#include <QVariantMap>
#include <map>
#include <string>
#include <vector>

namespace nordtile { namespace video { namespace catalog { class ChannelService; struct ChannelPage; struct ChannelHeader; } } }

namespace nordtile { namespace video { namespace sailfish {

// Translatable text for a failed discovery outcome ("" for Ok / ValidEmpty).
QString catalogOutcomeText(catalog::Outcome outcome);
// "1:05", "1:05:56", "" for unknown.
QString catalogDurationText(const catalog::Item &item);

class ChannelModel : public QAbstractListModel {
    Q_OBJECT
    Q_PROPERTY(QString reference READ reference WRITE open NOTIFY referenceChanged)
    Q_PROPERTY(QString channelId READ channelId NOTIFY headerChanged)
    Q_PROPERTY(QString title READ title NOTIFY headerChanged)
    Q_PROPERTY(QString handle READ handle NOTIFY headerChanged)
    Q_PROPERTY(QString avatarUrl READ avatarUrl NOTIFY headerChanged)
    Q_PROPERTY(QString subscriberText READ subscriberText NOTIFY headerChanged)
    Q_PROPERTY(double subscriberCount READ subscriberCount NOTIFY headerChanged)
    Q_PROPERTY(QString videoCountText READ videoCountText NOTIFY headerChanged)
    Q_PROPERTY(double videoCount READ videoCount NOTIFY headerChanged)
    Q_PROPERTY(QString description READ description NOTIFY headerChanged)
    Q_PROPERTY(QString tab READ tab WRITE setTab NOTIFY stateChanged)
    Q_PROPERTY(QStringList tabs READ tabs NOTIFY stateChanged)
    Q_PROPERTY(bool fallbackTab READ fallbackTab NOTIFY stateChanged)
    Q_PROPERTY(bool busy READ busy NOTIFY stateChanged)
    Q_PROPERTY(bool canLoadMore READ canLoadMore NOTIFY stateChanged)
    Q_PROPERTY(QString errorText READ errorText NOTIFY stateChanged)
    Q_PROPERTY(QString outcome READ outcome NOTIFY stateChanged)
    Q_PROPERTY(bool empty READ empty NOTIFY stateChanged)
    Q_PROPERTY(bool rssPainted READ rssPainted NOTIFY stateChanged)
    Q_PROPERTY(int count READ count NOTIFY stateChanged)
    Q_PROPERTY(int hiddenShorts READ hiddenShorts NOTIFY stateChanged)
    Q_PROPERTY(int hiddenMembersOnly READ hiddenMembersOnly NOTIFY stateChanged)
    Q_PROPERTY(int hiddenRestricted READ hiddenRestricted NOTIFY stateChanged)
    Q_PROPERTY(int hiddenWatched READ hiddenWatched NOTIFY stateChanged)
    Q_PROPERTY(int hiddenCount READ hiddenCount NOTIFY stateChanged)
    Q_PROPERTY(bool hideShorts READ hideShorts WRITE setHideShorts NOTIFY settingsChanged)
    Q_PROPERTY(bool hideMembersOnly READ hideMembersOnly WRITE setHideMembersOnly NOTIFY settingsChanged)
    Q_PROPERTY(bool hideRestricted READ hideRestricted WRITE setHideRestricted NOTIFY settingsChanged)
public:
    enum Role {
        ItemIdRole = Qt::UserRole + 1, VideoIdRole, KindRole, TitleRole, ChannelRole, ChannelIdRole, DurationRole,
        DurationSecondsRole, ViewCountRole, ViewTextRole, PublishedRole, ThumbnailRole, LiveRole, UpcomingRole,
        MembersOnlyRole, RestrictedRole, ShortRole, ShortCandidateRole, VideoCountRole, VideoCountTextRole, PlaylistIdRole,
        ChannelAvatarRole,  // the row's own, else this channel's header avatar, else the known one
        ThumbnailLargeRole  // the 1280x720 picture for cards (LargeThumbnails)
    };

    static void setDefaultService(catalog::ChannelService *service);

    explicit ChannelModel(QObject *parent = nullptr);
    explicit ChannelModel(catalog::ChannelService *service, QObject *parent = nullptr);

    int rowCount(const QModelIndex &parent = QModelIndex()) const override;
    QVariant data(const QModelIndex &index, int role) const override;
    QHash<int, QByteArray> roleNames() const override;

    QString reference() const { return reference_; }
    QString channelId() const { return channelId_; }
    QString title() const { return title_; }
    QString handle() const { return handle_; }
    QString avatarUrl() const { return avatarUrl_; }
    QString subscriberText() const { return subscriberText_; }
    double subscriberCount() const { return static_cast<double>(subscriberCount_); }
    QString videoCountText() const { return videoCountText_; }
    double videoCount() const { return static_cast<double>(videoCount_); }
    QString description() const { return description_; }
    QString tab() const { return tab_; }
    QStringList tabs() const { return tabs_; }
    bool fallbackTab() const { return fallbackTab_; }
    bool busy() const;
    bool canLoadMore() const;
    QString errorText() const;
    QString outcome() const;
    bool empty() const;
    bool rssPainted() const;
    int count() const { return static_cast<int>(visible_.size()); }
    int hiddenShorts() const { return hidden_.hiddenShorts; }
    int hiddenMembersOnly() const { return hidden_.hiddenMembersOnly; }
    int hiddenRestricted() const { return hidden_.hiddenRestricted; }
    int hiddenWatched() const { return hidden_.hiddenWatched; }
    int hiddenCount() const { return hidden_.hiddenTotal(); }
    bool hideShorts() const { return settings_.hideShorts; }
    bool hideMembersOnly() const { return settings_.hideMembersOnly; }
    bool hideRestricted() const { return settings_.hideRestricted; }
    void setHideShorts(bool on);
    void setHideMembersOnly(bool on);
    void setHideRestricted(bool on);

    Q_INVOKABLE void open(const QString &reference);
    Q_INVOKABLE void setTab(const QString &name);
    Q_INVOKABLE void loadMore();
    Q_INVOKABLE void refresh();
    Q_INVOKABLE QVariantMap get(int row) const;

    // Rows of the current tab after filtering (tests).
    const std::vector<catalog::Item> &visibleRows() const { return visible_; }

signals:
    void referenceChanged();
    void headerChanged();
    void stateChanged();
    void settingsChanged();

private:
    struct TabState {
        std::vector<catalog::Item> rows;          // merged, unfiltered, display order
        std::map<std::string, std::size_t> index; // id -> position in rows
        QString continuation;
        catalog::Outcome outcome = catalog::Outcome::Ok;
        bool requested = false;
        bool loading = false;
        bool loaded = false;                      // InnerTube page 1 arrived (or failed)
        bool rssRequested = false;
        bool rssPainted = false;
        bool missing = false;                     // the channel has no such tab
        int autoPages = 0;
    };
    void reset();
    TabState &state(const QString &tab) { return states_[tab]; }
    const TabState *currentState() const;
    void startTab(const QString &tab);
    void onRss(quint64 generation, catalog::Outcome outcome, std::vector<catalog::Item> rows);
    void onFirstPage(quint64 generation, const QString &tab, const catalog::ChannelPage &page);
    void onMore(quint64 generation, const QString &tab, const catalog::ChannelPage &page);
    void applyHeader(const catalog::ChannelHeader &header);
    void applyTabs(const catalog::ChannelPage &page);
    void merge(TabState &s, const std::vector<catalog::Item> &items);
    void sync();
    static QString normalizeTab(const QString &tab);
    static bool supportedTab(const QString &tab);

    QPointer<catalog::ChannelService> service_;
    quint64 generation_ = 0;
    QString reference_;
    QString channelId_;
    QString title_, handle_, avatarUrl_, subscriberText_, videoCountText_, description_;
    qint64 subscriberCount_ = -1;
    qint64 videoCount_ = -1;
    QString tab_;
    QStringList tabs_;
    bool fallbackTab_ = false;
    bool resolving_ = false;
    catalog::Outcome resolveOutcome_ = catalog::Outcome::Ok;
    QMap<QString, TabState> states_;
    catalog::RowFilterSettings settings_;
    catalog::RowFilterResult hidden_;          // counts only (visible rows live in visible_)
    std::vector<catalog::Item> visible_;
    QMetaObject::Connection storeConnection_;
};

}}}
