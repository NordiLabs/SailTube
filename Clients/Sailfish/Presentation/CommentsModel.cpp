// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "CommentsModel.h"
#include "Core/Catalog/Infrastructure/CommentsService.h"

#include <QCoreApplication>
#include <QDateTime>
#include <QSettings>
#include <QStandardPaths>
#include <QTimer>

namespace nordtile { namespace video { namespace sailfish {

using catalog::Comment;
using catalog::CommentsPage;
using catalog::CommentsEntry;
using catalog::Outcome;

namespace {
QString defaultSettingsFile() {
    return QStandardPaths::writableLocation(QStandardPaths::AppConfigLocation) + QStringLiteral("/SailTube.conf");
}
QString q(const std::string &s) { return QString::fromStdString(s); }
}

CommentsModel::CommentsModel(catalog::CommentsService *service, QObject *parent, const QString &settingsFile)
    : QAbstractListModel(parent), service_(service),
      settingsFile_(settingsFile.isEmpty() ? defaultSettingsFile() : settingsFile) {
    QSettings settings(settingsFile_, QSettings::IniFormat);
    settings.setFallbacksEnabled(false);
    const QString mode = settings.value(QLatin1String(settingsKey()), QStringLiteral("near")).toString();
    policy_.setMode(catalog::autoLoadModeFromName(mode.toStdString()));
}

CommentsModel::~CommentsModel() { abandonRequests(); }

qint64 CommentsModel::now() const { return clock_ ? clock_() : QDateTime::currentMSecsSinceEpoch(); }

void CommentsModel::schedule(int ms, std::function<void()> fire) {
    if (scheduler_) scheduler_(ms, fire);
    else QTimer::singleShot(ms, this, fire);
}

void CommentsModel::setBudgets(const catalog::CommentBudgets &budgets) {
    abandonRequests();
    beginResetModel();
    session_ = catalog::CommentsSession(budgets);
    reveal_.reset();
    ui_.clear();
    rows_.clear();
    endResetModel();
}

// ---------------------------------------------------------------- model

int CommentsModel::rowCount(const QModelIndex &parent) const {
    return parent.isValid() ? 0 : static_cast<int>(rows_.size());
}

QVariant CommentsModel::data(const QModelIndex &index, int role) const {
    if (!index.isValid() || index.row() < 0 || index.row() >= static_cast<int>(rows_.size())) return QVariant();
    const Row &r = rows_[static_cast<std::size_t>(index.row())];
    const std::size_t t = static_cast<std::size_t>(r.thread);
    if (t >= session_.threadCount()) return QVariant();
    const Comment &thread = session_.thread(t);
    const ThreadUi &ui = ui_[t];
    switch (role) {
    case RowTypeRole:
        return r.type == RowType::Thread ? QStringLiteral("thread") : r.type == RowType::Reply ? QStringLiteral("reply") : QStringLiteral("more");
    case RepliesExpandedRole: return ui.expanded;
    case CanLoadMoreRepliesRole: return session_.canLoadMoreReplies(t);
    case RepliesBusyRole: return ui.busy;
    case ReplyErrorTextRole: return ui.error;
    default: break;
    }
    if (r.type == RowType::More) {
        if (role == IsReplyRole) return true;
        if (role == ReplyCountRole) return static_cast<double>(thread.replyCount);
        return QVariant();
    }
    const Comment &c = r.type == RowType::Reply ? thread.replies[static_cast<std::size_t>(r.reply)] : thread;
    switch (role) {
    case CommentIdRole: return q(c.id);
    case AuthorRole: return q(c.author);
    case AuthorChannelIdRole: return q(c.authorChannelId);
    case AvatarRole: return q(c.avatarUrl);
    case TextRole: return q(c.text);
    case LikesRole: return q(c.likeText);
    case LikeCountRole: return static_cast<double>(c.likeCount);
    case PublishedRole: return q(c.publishedText);
    case IsReplyRole: return r.type == RowType::Reply;
    case ReplyCountRole: return static_cast<double>(c.replyCount);
    case IsCreatorRole: return c.creator;
    case PinnedRole: return c.pinned;
    case VerifiedRole: return c.verified;
    default: return QVariant();
    }
}

QHash<int, QByteArray> CommentsModel::roleNames() const {
    QHash<int, QByteArray> r;
    r.insert(RowTypeRole, "rowType");
    r.insert(CommentIdRole, "commentId");
    r.insert(AuthorRole, "author");
    r.insert(AuthorChannelIdRole, "authorChannelId");
    r.insert(AvatarRole, "avatar");
    r.insert(TextRole, "text");
    r.insert(LikesRole, "likes");
    r.insert(LikeCountRole, "likeCount");
    r.insert(PublishedRole, "published");
    r.insert(IsReplyRole, "isReply");
    r.insert(ReplyCountRole, "replyCount");
    r.insert(RepliesExpandedRole, "repliesExpanded");
    r.insert(CanLoadMoreRepliesRole, "canLoadMoreReplies");
    r.insert(RepliesBusyRole, "repliesBusy");
    r.insert(ReplyErrorTextRole, "replyErrorText");
    r.insert(IsCreatorRole, "isCreator");
    r.insert(PinnedRole, "pinned");
    r.insert(VerifiedRole, "verified");
    return r;
}

// ---------------------------------------------------------------- properties

QString CommentsModel::footerText() const {
    if (!started_ || (session_.threadCount() == 0 && totalText_.isEmpty())) return QString();
    if (!totalText_.isEmpty()) {
        //: Footer under the comments: "<shown> of <total>", e.g. "20 of 7.7K"
        return QCoreApplication::translate("CommentsModel", "%1 of %2").arg(reveal_.shown()).arg(totalText_);
    }
    //: Footer under the comments when the total is unknown
    return QCoreApplication::translate("CommentsModel", "%1 shown").arg(reveal_.shown());
}

bool CommentsModel::canLoadMore() const {
    if (!started_ || !error_.isEmpty()) return false;
    return static_cast<std::size_t>(reveal_.shown()) < session_.threadCount() || session_.canRequestMore();
}

void CommentsModel::setAutoLoadMode(const QString &mode) {
    const catalog::AutoLoadMode m = catalog::autoLoadModeFromName(mode.toStdString());
    if (m == policy_.mode()) return;
    policy_.setMode(m);
    QSettings settings(settingsFile_, QSettings::IniFormat);
    settings.setFallbacksEnabled(false);
    settings.setValue(QLatin1String(settingsKey()), QString::fromLatin1(catalog::autoLoadModeName(m)));
    emit autoLoadModeChanged();
}

void CommentsModel::setSort(const QString &name) {
    const catalog::CommentSort s = catalog::commentSortFromName(name.toStdString());
    if (s == sort_) return;
    sort_ = s;
    if (!started_ || commentsOff_) { emit stateChanged(); return; }
    abandonRequests();
    pendingReveal_ = true;
    const std::string token = tokenForSort(s);
    if (!token.empty()) {
        beginList(token);
        startTopLevel();
    } else {
        beginList(std::string());
        fetchEntry();   // the sort tokens live in watch-next
    }
    emit stateChanged();
}

std::string CommentsModel::tokenForSort(catalog::CommentSort sort) const {
    for (const catalog::SortOption &o : sorts_) if (o.sort == sort && !o.token.empty()) return o.token;
    return sort == catalog::CommentSort::Top ? firstToken_ : std::string();
}

// ---------------------------------------------------------------- video and entry

void CommentsModel::abandonRequests() {
    ++generation_;
    if (service_) {
        if (topOp_) service_->cancel(topOp_);
        if (entryOp_) service_->cancel(entryOp_);
        for (ThreadUi &u : ui_) if (u.op) service_->cancel(u.op);
    }
    for (ThreadUi &u : ui_) { u.op = 0; u.busy = false; }
    topOp_ = entryOp_ = 0;
    topBusy_ = entryBusy_ = false;
}

void CommentsModel::beginList(const std::string &token) {
    beginResetModel();
    session_.reset(token);
    reveal_.reset();
    ui_.clear();
    rows_.clear();
    endResetModel();
    error_.clear();
    emptyText_.clear();
    failed_ = Failed::None;
}

void CommentsModel::clear() {
    abandonRequests();
    beginList(std::string());
    videoId_.clear();
    firstToken_.clear();
    sorts_.clear();
    totalText_.clear();
    commentsOffText_.clear();
    started_ = commentsOff_ = pendingReveal_ = false;
    sort_ = catalog::CommentSort::Top;
    policy_.reset();
    ++videoGeneration_;
    emit stateChanged();
}

void CommentsModel::setVideo(const QString &videoId, const QString &token) {
    if (videoId == videoId_) {
        if (!started_ && !token.isEmpty()) firstToken_ = token.toStdString();
        return;
    }
    clear();
    videoId_ = videoId;
    firstToken_ = token.toStdString();
    emit stateChanged();
}

void CommentsModel::setEntry(const QString &videoId, const CommentsEntry &entry) {
    setVideo(videoId, q(entry.token));
    if (!started_) applyEntry(entry);
    emit stateChanged();
}

void CommentsModel::applyEntry(const CommentsEntry &entry) {
    if (!entry.token.empty()) firstToken_ = entry.token;
    if (!entry.sorts.empty()) sorts_ = entry.sorts;
    if (entry.total >= 0) totalText_ = q(entry.totalText);
    commentsOff_ = entry.commentsOff;
    commentsOffText_ = q(entry.message);
}

void CommentsModel::load(const QString &videoId, const QString &token) {
    setVideo(videoId, token);
    start();
}

void CommentsModel::start() {
    if (videoId_.isEmpty() || started_) return;
    policy_.markLoaded();
    started_ = true;
    pendingReveal_ = true;
    requestTimer_.start();
    if (commentsOff_) {
        //: Shown instead of comments when the uploader turned them off
        emptyText_ = commentsOffText_.isEmpty()
            ? QCoreApplication::translate("CommentsModel", "Comments are turned off.") : commentsOffText_;
        emit stateChanged();
        return;
    }
    const std::string token = tokenForSort(sort_);
    if (token.empty()) {
        fetchEntry();
    } else {
        beginList(token);
        startTopLevel();
    }
    emit stateChanged();
}

void CommentsModel::fetchEntry() {
    if (!service_ || entryBusy_) return;
    entryBusy_ = true;
    error_.clear();
    const quint64 gen = generation_;
    entryOp_ = service_->fetchEntry(videoId_, this, [this, gen](const CommentsEntry &entry) {
        if (gen != generation_) return;
        entryBusy_ = false;
        entryOp_ = 0;
        if (catalog::isFailure(entry.outcome)) { failList(Failed::Entry, entry.outcome); return; }
        applyEntry(entry);
        if (commentsOff_) {
            emptyText_ = commentsOffText_.isEmpty()
                ? QCoreApplication::translate("CommentsModel", "Comments are turned off.") : commentsOffText_;
            emit stateChanged();
            return;
        }
        std::string token = tokenForSort(sort_);
        if (token.empty()) {   // no sort menu: fall back to the default order
            sort_ = catalog::CommentSort::Top;
            token = tokenForSort(sort_);
        }
        beginList(token);
        startTopLevel();
        emit stateChanged();
    });
    emit stateChanged();
}

// ---------------------------------------------------------------- top level

void CommentsModel::startTopLevel() {
    if (topBusy_ || !service_) return;
    session_.clearFailure();
    failed_ = Failed::None;
    error_.clear();
    requestTimer_.start();
    session_.beginRequest();
    ui_.resize(session_.threadCount());
    if (pendingReveal_ && session_.threadCount() > static_cast<std::size_t>(reveal_.shown())) {
        revealStep();
        pendingReveal_ = false;
    }
    if (session_.requestNeedsAnotherPage()) fetchThreadPage();
    else finishTopLevel();
}

void CommentsModel::fetchThreadPage() {
    topBusy_ = true;
    const quint64 gen = generation_;
    topOp_ = service_->fetchThreads(q(session_.nextToken()), this, [this, gen](const CommentsPage &page) {
        if (gen != generation_) return;
        onThreadPage(page);
    });
    emit stateChanged();
}

void CommentsModel::onThreadPage(const CommentsPage &page) {
    topOp_ = 0;
    if (catalog::isFailure(page.outcome)) { failList(Failed::Threads, page.outcome); return; }
    if (page.total >= 0) totalText_ = q(page.totalText);
    if (sorts_.empty() && !page.sorts.empty()) sorts_ = page.sorts;
    session_.applyThreadPage(page);
    ui_.resize(session_.threadCount());
    if (pendingReveal_ && session_.threadCount() > static_cast<std::size_t>(reveal_.shown())) {
        revealStep();
        pendingReveal_ = false;
    }
    if (session_.requestNeedsAnotherPage()) { fetchThreadPage(); return; }
    finishTopLevel();
}

void CommentsModel::finishTopLevel() {
    topBusy_ = false;
    topOp_ = 0;
    if (pendingReveal_) { revealStep(); pendingReveal_ = false; }
    if (session_.threadCount() == 0 && !session_.canRequestMore()) {
        //: Shown when a video has no comments
        emptyText_ = QCoreApplication::translate("CommentsModel", "No comments yet.");
    }
    // NFR-PERF-08 is measured on the device from this line (one top-level request).
    qInfo("sailtube-timing comments %lld threads=%d pages=%d", static_cast<long long>(requestTimer_.elapsed()),
          session_.requestAdded(), session_.pagesFetched());
    emit stateChanged();
}

void CommentsModel::revealStep() {
    const int before = reveal_.shown();
    const int after = reveal_.grow(static_cast<int>(session_.threadCount()));
    if (after <= before) return;
    const int first = static_cast<int>(rows_.size());
    beginInsertRows(QModelIndex(), first, first + (after - before) - 1);
    for (int t = before; t < after; ++t) {
        Row r = { t, -1, RowType::Thread };
        rows_.push_back(r);
    }
    endInsertRows();
    emit stateChanged();
}

void CommentsModel::loadMore() {
    if (videoId_.isEmpty() || !started_) return;
    // Errors stay inline until an explicit retry; scrolling never retries.
    if (!error_.isEmpty() || !session_.scrollMayFetch()) return;
    if (static_cast<std::size_t>(reveal_.shown()) < session_.threadCount()) { revealStep(); return; }
    if (topBusy_ || entryBusy_) { pendingReveal_ = true; return; }
    if (session_.canRequestMore()) {
        pendingReveal_ = true;
        startTopLevel();
        emit stateChanged();
    }
}

void CommentsModel::failList(Failed kind, Outcome outcome) {
    if (kind == Failed::Entry) { entryBusy_ = false; entryOp_ = 0; }
    else { topBusy_ = false; topOp_ = 0; }
    failed_ = kind;
    session_.markFailed();
    error_ = outcomeText(outcome);
    pendingReveal_ = false;
    emit stateChanged();
}

void CommentsModel::retry() {
    if (failed_ == Failed::None) return;
    const Failed kind = failed_;
    failed_ = Failed::None;
    error_.clear();
    session_.clearFailure();
    pendingReveal_ = true;
    if (kind == Failed::Entry) fetchEntry();
    else startTopLevel();
    emit stateChanged();
}

// ---------------------------------------------------------------- replies

int CommentsModel::threadOf(int row) const {
    if (row < 0 || row >= static_cast<int>(rows_.size())) return -1;
    return rows_[static_cast<std::size_t>(row)].thread;
}

int CommentsModel::threadRowStart(int thread) const {
    for (std::size_t i = 0; i < rows_.size(); ++i)
        if (rows_[i].thread == thread && rows_[i].type == RowType::Thread) return static_cast<int>(i);
    return -1;
}

bool CommentsModel::hasMoreRow(int thread) const {
    const ThreadUi &u = ui_[static_cast<std::size_t>(thread)];
    return u.expanded && (u.busy || !u.error.isEmpty() || session_.canLoadMoreReplies(static_cast<std::size_t>(thread)));
}

void CommentsModel::syncThread(int thread) {
    const int start = threadRowStart(thread);
    if (start < 0) return;
    int oldReplies = 0;
    bool oldMore = false;
    for (std::size_t i = static_cast<std::size_t>(start) + 1; i < rows_.size() && rows_[i].thread == thread; ++i) {
        if (rows_[i].type == RowType::Reply) ++oldReplies;
        else if (rows_[i].type == RowType::More) oldMore = true;
    }
    const std::size_t t = static_cast<std::size_t>(thread);
    const int newReplies = ui_[t].expanded ? static_cast<int>(session_.thread(t).replies.size()) : 0;
    const bool newMore = hasMoreRow(thread);
    if (oldMore && !newMore) {
        const int r = start + 1 + oldReplies;
        beginRemoveRows(QModelIndex(), r, r);
        rows_.erase(rows_.begin() + r);
        endRemoveRows();
    }
    if (newReplies < oldReplies) {
        const int first = start + 1 + newReplies, last = start + oldReplies;
        beginRemoveRows(QModelIndex(), first, last);
        rows_.erase(rows_.begin() + first, rows_.begin() + last + 1);
        endRemoveRows();
    } else if (newReplies > oldReplies) {
        const int first = start + 1 + oldReplies, last = start + newReplies;
        beginInsertRows(QModelIndex(), first, last);
        std::vector<Row> added;
        for (int i = oldReplies; i < newReplies; ++i) {
            Row r = { thread, i, RowType::Reply };
            added.push_back(r);
        }
        rows_.insert(rows_.begin() + first, added.begin(), added.end());
        endInsertRows();
    }
    if (!oldMore && newMore) {
        const int r = start + 1 + newReplies;
        beginInsertRows(QModelIndex(), r, r);
        Row more = { thread, -1, RowType::More };
        rows_.insert(rows_.begin() + r, more);
        endInsertRows();
    } else if (oldMore && newMore) {
        const QModelIndex i = index(start + 1 + newReplies, 0);
        emit dataChanged(i, i);
    }
    const QModelIndex head = index(start, 0);
    emit dataChanged(head, head);
    emit stateChanged();   // count
}

void CommentsModel::toggleReplies(int row) {
    const int thread = threadOf(row);
    if (thread < 0) return;
    const std::size_t t = static_cast<std::size_t>(thread);
    ThreadUi &u = ui_[t];
    if (u.expanded) {
        u.expanded = false;
        syncThread(thread);
        return;
    }
    if (session_.thread(t).replies.empty() && !session_.canLoadMoreReplies(t)) return;  // nothing to show
    u.expanded = true;
    if (session_.thread(t).replies.empty() && !u.busy) {
        u.error.clear();
        startReplies(thread);
    }
    syncThread(thread);
}

void CommentsModel::loadMoreReplies(int row) {
    const int thread = threadOf(row);
    if (thread < 0) return;
    ThreadUi &u = ui_[static_cast<std::size_t>(thread)];
    if (u.busy) return;
    u.error.clear();
    u.expanded = true;
    if (session_.canLoadMoreReplies(static_cast<std::size_t>(thread))) startReplies(thread);
    syncThread(thread);
}

void CommentsModel::startReplies(int thread) {
    const std::size_t t = static_cast<std::size_t>(thread);
    session_.beginReplyRequest(t);
    if (session_.replyRequestNeedsAnotherPage(t)) fetchReplyPage(thread);
}

void CommentsModel::fetchReplyPage(int thread) {
    const std::size_t t = static_cast<std::size_t>(thread);
    ui_[t].busy = true;
    const quint64 gen = generation_;
    ui_[t].op = service_->fetchReplies(q(session_.replyToken(t)), this, [this, gen, thread](const CommentsPage &page) {
        if (gen != generation_) return;
        onReplyPage(thread, page);
    });
}

void CommentsModel::onReplyPage(int thread, const CommentsPage &page) {
    const std::size_t t = static_cast<std::size_t>(thread);
    ThreadUi &u = ui_[t];
    u.op = 0;
    if (catalog::isFailure(page.outcome)) {
        u.busy = false;
        u.error = outcomeText(page.outcome);
        syncThread(thread);
        return;
    }
    session_.applyReplyPage(t, page);
    if (session_.replyRequestNeedsAnotherPage(t)) {
        fetchReplyPage(thread);
        syncThread(thread);
        return;
    }
    u.busy = false;
    syncThread(thread);
    // The total budget may have run out: other expanded threads lose their "more" row.
    if (session_.totalReplies() >= session_.budgets().repliesTotal)
        for (std::size_t i = 0; i < ui_.size(); ++i)
            if (ui_[i].expanded && static_cast<int>(i) != thread) syncThread(static_cast<int>(i));
}

// ---------------------------------------------------------------- auto-load

void CommentsModel::playbackStarted() {
    if (videoId_.isEmpty() || started_) return;
    const qint64 delay = policy_.onPlaybackStarted(now());
    if (delay < 0) return;
    const quint64 vg = videoGeneration_;
    schedule(static_cast<int>(delay), [this, vg] { onAutoTimer(vg); });
}

void CommentsModel::onAutoTimer(quint64 videoGeneration) {
    if (videoGeneration != videoGeneration_ || started_) return;
    const qint64 t = now();
    if (policy_.onTimer(t)) { start(); return; }
    const qint64 remaining = policy_.remainingMs(t);
    if (remaining > 0) {
        const quint64 vg = videoGeneration_;
        schedule(static_cast<int>(remaining), [this, vg] { onAutoTimer(vg); });
    }
}

void CommentsModel::scrolledNear() {
    // Once started (loaded, loading or failed) scrolling near does nothing: no automatic retry.
    if (videoId_.isEmpty() || started_) return;
    if (policy_.onScrolledNear()) start();
}

// ---------------------------------------------------------------- texts

QString CommentsModel::outcomeText(Outcome outcome) {
    switch (outcome) {
    case Outcome::Network:
        //: Inline error in the comments section
        return QCoreApplication::translate("CommentsModel", "Comments could not be loaded. Check the connection.");
    case Outcome::RateLimited:
        return QCoreApplication::translate("CommentsModel", "YouTube is limiting requests. Try again later.");
    case Outcome::Http4xx:
    case Outcome::Http5xx:
        return QCoreApplication::translate("CommentsModel", "YouTube did not answer. Try again.");
    case Outcome::ShapeDrift:
        return QCoreApplication::translate("CommentsModel", "YouTube sent comments in an unknown format.");
    case Outcome::Unavailable:
        return QCoreApplication::translate("CommentsModel", "Comments are not available for this video.");
    case Outcome::Ok:
    case Outcome::ValidEmpty:
        break;
    }
    return QString();
}

}}}
