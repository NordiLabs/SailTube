// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#pragma once

// The watch page's video info (DSC-14): views, likes, date, subscriber count, owner, the
// description (5-line collapsed StyledText with tappable links, seeking timestamps and
// hashtags), share / copy-link / open-in-browser URLs, the chapter list (DSC-15) and the
// related rows (DSC-13).
//
// Setting videoId starts the watch-next + WEB player requests (WatchNextService); nothing here
// waits for them or touches PlaybackSession. A details-only view creates its own WatchInfo
// (registered QML type, using the default service) so the playing page stays untouched.
//
// QtCore only: opening URLs is the view's job (Qt.openUrlExternally on openUrlRequested), seeks
// go out as seekRequested(seconds) (PlaybackSession::seek takes milliseconds).

#include "ChapterModel.h"
#include "RelatedModel.h"
#include "Core/Catalog/Infrastructure/WatchNextService.h"

#include <QObject>
#include <QTimer>
#include <QPointer>
#include <QString>
#include <QVariantList>
#include <memory>

namespace nordtile { namespace video { namespace sailfish {

// Not final: qmlRegisterType derives from it.
class WatchInfo : public QObject {
    Q_OBJECT
    Q_PROPERTY(QString videoId READ videoId WRITE setVideoId NOTIFY videoIdChanged)
    // NFR-PERF: with waitForPlayback on, a new video's watch-next requests (info, related, the
    // comments token) wait until playbackRunning turns true for it, or kGraceMs at most, so they
    // never compete with the first video bytes. A cached answer is shown at once. Opening the
    // page on a video that is already playing loads at once.
    Q_PROPERTY(bool waitForPlayback READ waitForPlayback WRITE setWaitForPlayback NOTIFY holdChanged)
    Q_PROPERTY(bool playbackRunning READ playbackRunning WRITE setPlaybackRunning NOTIFY holdChanged)
    // DL-07: a downloaded video plays offline; no watch-next request is made while this is on.
    Q_PROPERTY(bool offline READ offline WRITE setOffline NOTIFY holdChanged)
    Q_PROPERTY(bool loading READ loading NOTIFY changed)
    Q_PROPERTY(bool loaded READ loaded NOTIFY changed)
    Q_PROPERTY(QString errorText READ errorText NOTIFY changed)
    Q_PROPERTY(QString title READ title NOTIFY changed)
    Q_PROPERTY(double viewCount READ viewCount NOTIFY changed)
    Q_PROPERTY(QString viewText READ viewText NOTIFY changed)
    Q_PROPERTY(double likeCount READ likeCount NOTIFY changed)
    Q_PROPERTY(QString likeText READ likeText NOTIFY changed)
    Q_PROPERTY(QString dateText READ dateText NOTIFY changed)
    Q_PROPERTY(double publishedEpoch READ publishedEpoch NOTIFY changed)
    Q_PROPERTY(double subscriberCount READ subscriberCount NOTIFY changed)
    Q_PROPERTY(QString subscriberText READ subscriberText NOTIFY changed)
    Q_PROPERTY(QString owner READ owner NOTIFY changed)
    Q_PROPERTY(QString ownerId READ ownerId NOTIFY changed)
    Q_PROPERTY(QString ownerHandle READ ownerHandle NOTIFY changed)
    Q_PROPERTY(QString avatar READ avatar NOTIFY changed)
    Q_PROPERTY(QString thumbnail READ thumbnail NOTIFY videoIdChanged)
    Q_PROPERTY(double durationSeconds READ durationSeconds NOTIFY changed)
    Q_PROPERTY(bool live READ live NOTIFY changed)
    // RES-19: a stream or premiere that has not started, and its scheduled start in ms since the
    // epoch (0 when unknown; the unit of playback.scheduledStart), from the player answer.
    Q_PROPERTY(bool upcoming READ upcoming NOTIFY changed)
    Q_PROPERTY(double scheduledStart READ scheduledStart NOTIFY changed)
    // RYD-01: the estimated dislikes of this video, -1 when unknown or while the setting is off;
    // dislikeText is its compact form ("1.2K", in the display language), "" when unknown. Filled
    // for the watch page's instance only (WatchDislikes); a details-only instance has none.
    Q_PROPERTY(double dislikeCount READ dislikeCount NOTIFY changed)
    Q_PROPERTY(QString dislikeText READ dislikeText NOTIFY changed)
    Q_PROPERTY(QString description READ description NOTIFY changed)
    Q_PROPERTY(QVariantList descriptionRuns READ descriptionRuns NOTIFY changed)
    Q_PROPERTY(QString descriptionStyled READ descriptionStyled NOTIFY changed)
    Q_PROPERTY(QString collapsedDescriptionStyled READ collapsedDescriptionStyled NOTIFY changed)
    Q_PROPERTY(QString shownDescription READ shownDescription NOTIFY collapsedChanged)
    Q_PROPERTY(bool descriptionTruncated READ descriptionTruncated NOTIFY changed)
    Q_PROPERTY(bool collapsed READ collapsed WRITE setCollapsed NOTIFY collapsedChanged)
    Q_PROPERTY(int collapsedLines READ collapsedLines CONSTANT)
    Q_PROPERTY(QString commentsContinuation READ commentsContinuation NOTIFY changed)
    Q_PROPERTY(double durationHint READ durationHint WRITE setDurationHint NOTIFY changed)
    Q_PROPERTY(QVariantList fallbackChapters READ fallbackChapters WRITE setFallbackChapters NOTIFY changed)
    Q_PROPERTY(QObject *related READ related CONSTANT)
    Q_PROPERTY(QObject *chapters READ chapters CONSTANT)
public:
    explicit WatchInfo(QObject *parent = nullptr);   // uses defaultService()
    explicit WatchInfo(catalog::WatchNextService *service, QObject *parent = nullptr);
    ~WatchInfo() override;

    // The service QML-created instances use (set once by the composition).
    static void setDefaultService(catalog::WatchNextService *service);
    static catalog::WatchNextService *defaultService();

    QString videoId() const { return videoId_; }
    void setVideoId(const QString &videoId);
    bool loading() const;
    bool loaded() const;
    QString errorText() const;
    QString title() const;
    double viewCount() const;
    QString viewText() const;
    double likeCount() const;
    QString likeText() const;
    QString dateText() const;
    double publishedEpoch() const;
    double subscriberCount() const;
    QString subscriberText() const;
    QString owner() const;
    QString ownerId() const;
    QString ownerHandle() const;
    QString avatar() const;
    QString thumbnail() const;
    double durationSeconds() const;
    bool live() const;
    bool upcoming() const;
    double scheduledStart() const;
    double dislikeCount() const { return dislikeCount_; }
    QString dislikeText() const { return dislikeText_; }
    // The estimate for the current video (count < 0 = unknown).
    void setDislikeEstimate(double count, const QString &text);
    QString description() const;
    QVariantList descriptionRuns() const;
    QString descriptionStyled() const { return styled_; }
    QString collapsedDescriptionStyled() const { return collapsedStyled_; }
    QString shownDescription() const { return collapsed_ && truncated_ ? collapsedStyled_ : styled_; }
    bool descriptionTruncated() const { return truncated_; }
    bool collapsed() const { return collapsed_; }
    void setCollapsed(bool collapsed);
    int collapsedLines() const { return catalog::kCollapsedDescriptionLines; }
    QString commentsContinuation() const;
    double durationHint() const { return durationHint_; }
    void setDurationHint(double seconds);
    QVariantList fallbackChapters() const { return fallbackChapters_; }
    void setFallbackChapters(const QVariantList &chapters);   // [{title, start(ms)}] from PlaybackSession
    QObject *related() const { return related_; }
    QObject *chapters() const { return chapters_; }
    RelatedModel *relatedModel() const { return related_; }
    ChapterModel *chapterModel() const { return chapters_; }

    Q_INVOKABLE void load(const QString &videoId) { setVideoId(videoId); }
    Q_INVOKABLE void reload();
    Q_INVOKABLE void toggleCollapsed() { setCollapsed(!collapsed_); }
    // https://youtu.be/<id>[?t=<s>] (share and copy link).
    Q_INVOKABLE QString shareUrl(bool withTime = false, double seconds = 0) const;
    // https://www.youtube.com/watch?v=<id>[&t=<s>s]
    Q_INVOKABLE QString browserUrl(bool withTime = false, double seconds = 0) const;
    // Emits openUrlRequested with the browser URL and returns it.
    Q_INVOKABLE QString openInBrowser(bool withTime = false, double seconds = 0);
    // An anchor of the description was tapped: seek, hashtag search or external link.
    Q_INVOKABLE void activateLink(const QString &href);

    static const int kGraceMs = 2500;
    bool waitForPlayback() const { return waitForPlayback_; }
    void setWaitForPlayback(bool on);
    bool playbackRunning() const { return playbackRunning_; }
    void setPlaybackRunning(bool running);
    bool deferred() const { return deferred_; }   // tests
    bool offline() const { return offline_; }
    void setOffline(bool offline);
    void setGraceMs(int ms);                      // tests

signals:
    void videoIdChanged();
    void holdChanged();
    // The watch data of `videoId` was asked for (after the hold, or at once for a cached page):
    // what else belongs to an opened video (its caption track list) is asked for with it.
    void requested(const QString &videoId);
    void changed();
    void collapsedChanged();
    void seekRequested(double seconds);
    void hashtagRequested(const QString &tag);
    void openUrlRequested(const QString &url);

private:
    void attach(catalog::WatchNextService *service);
    void pageChanged(const QString &videoId);
    void refresh();
    void updateChapters();

    QPointer<catalog::WatchNextService> service_;
    QString videoId_;
    std::shared_ptr<const catalog::WatchPage> page_;
    QString styled_;
    QString collapsedStyled_;
    bool truncated_ = false;
    bool collapsed_ = true;
    void release(const char *why);
    bool waitForPlayback_ = false;
    bool playbackRunning_ = false;
    bool deferred_ = false;
    bool offline_ = false;
    QTimer *grace_ = nullptr;
    double durationHint_ = -1;
    double dislikeCount_ = -1;
    QString dislikeText_;
    QVariantList fallbackChapters_;
    RelatedModel *related_;
    ChapterModel *chapters_;
};

}}}
