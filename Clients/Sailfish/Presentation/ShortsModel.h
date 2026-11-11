// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#pragma once

// The Shorts section's list (context property `shortsModel`), over ShortsFeedService. Roles
// (agreed with the design component): videoId, title, channel, channelId, channelAvatar, thumbnail
// (vertical), viewCount, viewText (localized), likeCount, likeText, published,
// durationSeconds (-1: lockups carry none), source ("subscriptions" | "popular" | "account"),
// hints (the playback.open() map). setCurrent(row) tells playback the next two ids through
// tapIntent.likelyVisible and preloads the next page near the end.
//
// The Short's on-picture data comes from one details request per Short (ShortDetailService), made only for the Short on screen and the next one, one at
// a time, after the hold that protects the first video bytes: `playbackRunning` turning true for
// the Short, or kGraceMs at most. Roles filled from it: likeCount, likeText, commentCount,
// commentText, commentsDisabled, channelHandle, description, descriptionShort, relatedVideoId,
// relatedVideoTitle, relatedVideoThumbnail, relatedVideoIsShort, soundText; channel, channelId, channelAvatar, title,
// viewText and published when the list row had none. dislikeCount / dislikeText are the Return
// YouTube Dislike estimate of the Short on screen (only while that setting is on, and only while
// catalog::kShortsDislikeEstimate is true: it is not, see ShortDetail.h). subscribed
// follows the library's subscription list. detailsLoaded / detailsLoading / detailsFailed tell a
// skeleton from a Short with nothing to show. Unknown numbers are -1, unknown texts "".

#include "Core/Catalog/Domain/ShortDetail.h"

#include <QAbstractListModel>
#include <QHash>
#include <QPointer>
#include <QString>
#include <QVariantMap>

class QTimer;

namespace nordtile { namespace video {
namespace catalog { class ShortsFeedService; class ShortDetailService; class DislikeService; }
namespace sailfish {

class TapIntent;
class DislikeInfo;

class ShortsModel final : public QAbstractListModel {
    Q_OBJECT
    Q_PROPERTY(bool busy READ busy NOTIFY stateChanged)
    Q_PROPERTY(bool loaded READ loaded NOTIFY stateChanged)
    Q_PROPERTY(bool empty READ empty NOTIFY stateChanged)
    Q_PROPERTY(bool canLoadMore READ canLoadMore NOTIFY stateChanged)
    Q_PROPERTY(QString errorText READ errorText NOTIFY stateChanged)
    Q_PROPERTY(int count READ count NOTIFY stateChanged)
    Q_PROPERTY(bool signedIn READ signedIn NOTIFY stateChanged)
    // True while the Short on screen is playing (bind it to playback.playing). The details
    // requests wait for it, or kGraceMs, so they never compete with the first video bytes.
    Q_PROPERTY(bool playbackRunning READ playbackRunning WRITE setPlaybackRunning NOTIFY holdChanged)
    // Grows whenever any row's data changed (details arrived, subscriptions or the dislike setting
    // changed, rows added or reset). get(row) is a snapshot with no notification of its own, so a
    // QML binding over it must depend on this: `shortsModel.revision >= 0 ? shortsModel.get(i) : null`.
    // Delegates that read roles (model.likeText) update without it.
    Q_PROPERTY(int revision READ revision NOTIFY revisionChanged)
public:
    enum Role {
        VideoIdRole = Qt::UserRole + 1, TitleRole, ChannelRole, ChannelIdRole, ChannelAvatarRole, ThumbnailRole,
        ViewCountRole, ViewTextRole, LikeCountRole, LikeTextRole, PublishedRole, DurationSecondsRole, SourceRole, HintsRole,
        ThumbnailLargeRole,   // the vertical frame itself (1080x1920)
        CommentCountRole, CommentTextRole, CommentsDisabledRole, DislikeCountRole, DislikeTextRole, ChannelHandleRole,
        SubscribedRole, DescriptionRole, DescriptionShortRole, RelatedVideoIdRole, RelatedVideoTitleRole,
        RelatedVideoIsShortRole, SoundTextRole, DetailsLoadedRole, DetailsLoadingRole, DetailsFailedRole,
        RelatedVideoThumbnailRole
    };
    static const int kPreloadDistance = 5;
    static const int kGraceMs = 2500;

    explicit ShortsModel(catalog::ShortsFeedService *service, TapIntent *intent = nullptr, QObject *parent = nullptr);

    // The details service (without it no details are asked for), and the dislike estimate: the
    // service asks only while `info` says the setting is on (null = follow the service's own flag).
    void setDetails(catalog::ShortDetailService *details);
    void setDislikes(catalog::DislikeService *service, DislikeInfo *info);
    bool playbackRunning() const { return playbackRunning_; }
    void setPlaybackRunning(bool running);
    void setGraceMs(int ms);                 // tests
    bool released() const { return released_; }   // the hold is over for the Short on screen (tests)
    int detailRequests() const { return detailRequests_; }   // details requests sent (tests)
    int currentRow() const { return current_; }
    int revision() const { return revision_; }

    int rowCount(const QModelIndex &parent = QModelIndex()) const override;
    QVariant data(const QModelIndex &index, int role) const override;
    QHash<int, QByteArray> roleNames() const override;

    bool busy() const;
    bool loaded() const;
    bool empty() const { return loaded() && count() == 0 && !busy(); }
    bool canLoadMore() const;
    QString errorText() const;
    int count() const { return shown_; }
    bool signedIn() const;

    Q_INVOKABLE void refresh();
    Q_INVOKABLE void loadMore();
    Q_INVOKABLE QVariantMap get(int row) const;
    Q_INVOKABLE void setCurrent(int row);

signals:
    void stateChanged();
    void holdChanged();
    void revisionChanged();

private:
    struct Entry {
        catalog::ShortDetail detail;
        bool loaded = false;
        bool loading = false;
        bool failed = false;
        bool dislikeAsked = false;
        double dislike = -1;
        QString dislikeText;
    };
    void sync();
    void bump();
    QString idAt(int row) const;
    const Entry *entryFor(const QString &videoId) const;
    void announce(const QString &videoId);                  // dataChanged for every row of the Short
    void pump();                                            // asks for what the Short on screen and the next one lack
    void requestDetails(int row);
    void release();
    void askDislike();
    bool wantsDislike() const;
    void dislikeSettingChanged();
    QString channelIdOf(int row, const Entry *entry) const;
    QPointer<catalog::ShortsFeedService> service_;
    QPointer<TapIntent> intent_;
    QPointer<catalog::ShortDetailService> details_;
    QPointer<catalog::DislikeService> dislikes_;
    QPointer<DislikeInfo> dislikeInfo_;
    QHash<QString, Entry> entries_;
    QTimer *grace_;
    int shown_ = 0;
    int current_ = -1;
    int inFlight_ = 0;
    int detailRequests_ = 0;
    int revision_ = 0;
    bool playbackRunning_ = false;
    bool released_ = false;
};

}}}
