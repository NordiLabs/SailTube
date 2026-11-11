// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#pragma once

// Comments list for the video page and the full comments page (DSC-16). A flat list of rows:
// a "thread" row per revealed top-level comment, its "reply" rows when expanded, and a "more"
// row under expanded replies (more replies, loading, or an inline replies error).
//
// * setVideo() arms the auto-load policy for a video (setting catalog/comments/autoLoad:
//   "off" | "near" (default) | "playback"); playbackStarted() and scrolledNear() are the hooks.
//   load() / start() load at once (explicit, e.g. the full page or a header tap).
// * Threads are revealed in steps by loadMore(); the window only grows (no scrollbar flicker).
// * Errors are inline: errorText for the list (retry() repeats it), replyErrorText on a "more"
//   row (loadMoreReplies(row) repeats it). Scrolling never retries.
// * All network work goes through CommentsService on the background discovery client.

#include "Core/Catalog/Domain/Comments.h"

#include <QAbstractListModel>
#include <QElapsedTimer>
#include <QString>
#include <functional>
#include <vector>

namespace nordtile { namespace video { namespace catalog { class CommentsService; } } }

namespace nordtile { namespace video { namespace sailfish {

class CommentsModel final : public QAbstractListModel {
    Q_OBJECT
    Q_PROPERTY(QString videoId READ videoId NOTIFY stateChanged)
    Q_PROPERTY(bool started READ started NOTIFY stateChanged)
    Q_PROPERTY(bool busy READ busy NOTIFY stateChanged)
    Q_PROPERTY(QString errorText READ errorText NOTIFY stateChanged)
    Q_PROPERTY(QString emptyText READ emptyText NOTIFY stateChanged)
    Q_PROPERTY(QString totalText READ totalText NOTIFY stateChanged)
    Q_PROPERTY(int shownCount READ shownCount NOTIFY stateChanged)
    Q_PROPERTY(QString footerText READ footerText NOTIFY stateChanged)
    Q_PROPERTY(bool canLoadMore READ canLoadMore NOTIFY stateChanged)
    Q_PROPERTY(QString sort READ sort WRITE setSort NOTIFY stateChanged)
    Q_PROPERTY(bool canSort READ canSort NOTIFY stateChanged)
    Q_PROPERTY(QString autoLoadMode READ autoLoadMode WRITE setAutoLoadMode NOTIFY autoLoadModeChanged)
    Q_PROPERTY(int count READ count NOTIFY stateChanged)
public:
    enum Role {
        RowTypeRole = Qt::UserRole + 1, CommentIdRole, AuthorRole, AuthorChannelIdRole, AvatarRole, TextRole,
        LikesRole, LikeCountRole, PublishedRole, IsReplyRole, ReplyCountRole, RepliesExpandedRole,
        CanLoadMoreRepliesRole, RepliesBusyRole, ReplyErrorTextRole, IsCreatorRole, PinnedRole, VerifiedRole
    };
    using Clock = std::function<qint64()>;
    using Scheduler = std::function<void(int ms, std::function<void()> fire)>;

    static const char *settingsKey() { return "catalog/comments/autoLoad"; }

    // `settingsFile` empty: AppConfigLocation/SailTube.conf (the file AppSettings uses).
    explicit CommentsModel(catalog::CommentsService *service, QObject *parent = nullptr,
                           const QString &settingsFile = QString());
    ~CommentsModel() override;

    int rowCount(const QModelIndex &parent = QModelIndex()) const override;
    QVariant data(const QModelIndex &index, int role) const override;
    QHash<int, QByteArray> roleNames() const override;

    // Tests and composition.
    void setClock(Clock clock) { clock_ = clock; }
    void setScheduler(Scheduler scheduler) { scheduler_ = scheduler; }
    void setBudgets(const catalog::CommentBudgets &budgets);
    void setRevealStep(int step) { reveal_ = catalog::RevealWindow(step); }
    // The watch-next package may hand over the whole entry it parsed (token, count, sort
    // tokens) instead of a bare token; it arms the video like setVideo().
    void setEntry(const QString &videoId, const catalog::CommentsEntry &entry);
    const catalog::CommentsSession &session() const { return session_; }

    QString videoId() const { return videoId_; }
    bool started() const { return started_; }
    bool busy() const { return topBusy_ || entryBusy_; }
    QString errorText() const { return error_; }
    QString emptyText() const { return emptyText_; }
    QString totalText() const { return totalText_; }
    int shownCount() const { return reveal_.shown(); }
    QString footerText() const;
    bool canLoadMore() const;
    QString sort() const { return QString::fromLatin1(catalog::commentSortName(sort_)); }
    void setSort(const QString &sort);
    bool canSort() const { return !sorts_.empty() || !videoId_.isEmpty(); }
    QString autoLoadMode() const { return QString::fromLatin1(catalog::autoLoadModeName(policy_.mode())); }
    void setAutoLoadMode(const QString &mode);
    int count() const { return static_cast<int>(rows_.size()); }

    // Arms a video without loading (the auto-load setting decides). An empty token means the
    // model fetches watch-next itself when the load starts.
    Q_INVOKABLE void setVideo(const QString &videoId, const QString &token = QString());
    // setVideo() + start().
    Q_INVOKABLE void load(const QString &videoId, const QString &token = QString());
    // Explicit load of the armed video (no-op once started).
    Q_INVOKABLE void start();
    // Reveals the next step of threads, fetching the next request when needed. Does nothing
    // while an error is shown.
    Q_INVOKABLE void loadMore();
    Q_INVOKABLE void toggleReplies(int row);
    // More replies for the thread of `row` (also the explicit retry after a replies error).
    Q_INVOKABLE void loadMoreReplies(int row);
    // Repeats the failed list request (tap to retry).
    Q_INVOKABLE void retry();
    Q_INVOKABLE void playbackStarted();
    Q_INVOKABLE void scrolledNear();
    Q_INVOKABLE void clear();

signals:
    void stateChanged();
    void autoLoadModeChanged();

private:
    enum class RowType { Thread, Reply, More };
    struct Row { int thread; int reply; RowType type; };
    struct ThreadUi {
        ThreadUi() : expanded(false), busy(false), op(0) {}
        bool expanded;
        bool busy;
        quint64 op;
        QString error;
    };
    enum class Failed { None, Entry, Threads };

    qint64 now() const;
    void schedule(int ms, std::function<void()> fire);
    void abandonRequests();
    void beginList(const std::string &token);
    std::string tokenForSort(catalog::CommentSort sort) const;
    void applyEntry(const catalog::CommentsEntry &entry);
    void fetchEntry();
    void startTopLevel();
    void fetchThreadPage();
    void onThreadPage(const catalog::CommentsPage &page);
    void finishTopLevel();
    void revealStep();
    void startReplies(int thread);
    void fetchReplyPage(int thread);
    void onReplyPage(int thread, const catalog::CommentsPage &page);
    void failList(Failed kind, catalog::Outcome outcome);
    void onAutoTimer(quint64 videoGeneration);
    int threadOf(int row) const;
    int threadRowStart(int thread) const;
    bool hasMoreRow(int thread) const;
    void syncThread(int thread);
    static QString outcomeText(catalog::Outcome outcome);

    catalog::CommentsService *service_;
    QString settingsFile_;
    Clock clock_;
    Scheduler scheduler_;
    catalog::CommentsSession session_;
    catalog::RevealWindow reveal_;
    catalog::AutoLoadPolicy policy_;
    std::vector<ThreadUi> ui_;
    std::vector<Row> rows_;
    std::vector<catalog::SortOption> sorts_;
    catalog::CommentSort sort_ = catalog::CommentSort::Top;
    QString videoId_;
    std::string firstToken_;
    QString totalText_;
    QString error_;
    QString emptyText_;
    QString commentsOffText_;
    Failed failed_ = Failed::None;
    bool started_ = false;
    bool commentsOff_ = false;
    bool topBusy_ = false;
    bool entryBusy_ = false;
    bool pendingReveal_ = false;
    quint64 topOp_ = 0;
    quint64 entryOp_ = 0;
    quint64 generation_ = 0;       // drops answers of abandoned requests
    quint64 videoGeneration_ = 0;  // drops auto-load timers of a previous video
    QElapsedTimer requestTimer_;
};

}}}
