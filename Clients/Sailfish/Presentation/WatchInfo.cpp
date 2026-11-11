// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "WatchInfo.h"
#include "Core/Catalog/Domain/Images.h"

#include <QDateTime>
#include <QLocale>
#include <QVariantMap>

namespace nordtile { namespace video { namespace sailfish {

namespace {
QString q(const std::string &s) { return QString::fromStdString(s); }
QPointer<catalog::WatchNextService> &sharedService() {
    static QPointer<catalog::WatchNextService> service;
    return service;
}
const char *kindName(catalog::RunKind k) {
    switch (k) {
    case catalog::RunKind::Text: return "text";
    case catalog::RunKind::Link: return "link";
    case catalog::RunKind::Timestamp: return "timestamp";
    case catalog::RunKind::Hashtag: return "hashtag";
    }
    return "text";
}
}

void WatchInfo::setDefaultService(catalog::WatchNextService *service) { sharedService() = service; }
catalog::WatchNextService *WatchInfo::defaultService() { return sharedService().data(); }

WatchInfo::WatchInfo(QObject *parent)
    : QObject(parent), related_(new RelatedModel(nullptr, this)), chapters_(new ChapterModel(this)) {
    attach(defaultService());
}

WatchInfo::WatchInfo(catalog::WatchNextService *service, QObject *parent)
    : QObject(parent), related_(new RelatedModel(nullptr, this)), chapters_(new ChapterModel(this)) {
    attach(service);
}

WatchInfo::~WatchInfo() {}

void WatchInfo::attach(catalog::WatchNextService *service) {
    if (!grace_) {
        grace_ = new QTimer(this);
        grace_->setSingleShot(true);
        grace_->setInterval(kGraceMs);
        connect(grace_, &QTimer::timeout, this, [this] { if (!offline_) release("grace"); });
    }
    service_ = service;
    related_->setService(service);
    if (service_) connect(service_, &catalog::WatchNextService::pageChanged, this, &WatchInfo::pageChanged);
}

void WatchInfo::setVideoId(const QString &videoId) {
    const QString id = videoId.trimmed();
    if (id == videoId_) return;
    const bool fromNothing = videoId_.isEmpty();
    videoId_ = id;
    page_.reset();
    collapsed_ = true;
    dislikeCount_ = -1;       // the previous video's estimate never shows on this one
    dislikeText_.clear();
    // durationHint / fallbackChapters stay: the watch page binds them to the player, which
    // switches videos together with this object.
    related_->setVideo(id);
    emit videoIdChanged();
    emit collapsedChanged();
    grace_->stop();
    if (offline_ && !id.isEmpty() && !(service_ && service_->page(id))) {
        deferred_ = true;   // released only when the video is online again (setOffline(false))
        refresh();
        return;
    }
    // Wait for the player unless the answer is cached, or the page opened on a running video.
    if (waitForPlayback_ && !id.isEmpty() && !(service_ && service_->page(id)) && !(fromNothing && playbackRunning_)) {
        deferred_ = true;
        grace_->start();
        refresh();
        return;
    }
    deferred_ = false;
    reload();
}

void WatchInfo::setWaitForPlayback(bool on) {
    if (on == waitForPlayback_) return;
    waitForPlayback_ = on;
    emit holdChanged();
    if (!on && deferred_ && !offline_) release("off");
}

void WatchInfo::setPlaybackRunning(bool running) {
    if (running == playbackRunning_) return;
    playbackRunning_ = running;
    emit holdChanged();
    if (running && deferred_ && !offline_) release("playing");
}

void WatchInfo::setGraceMs(int ms) { grace_->setInterval(ms); }

void WatchInfo::setOffline(bool offline) {
    if (offline == offline_) return;
    offline_ = offline;
    emit holdChanged();
    if (offline_) grace_->stop();
    else if (deferred_) release("online");
}

void WatchInfo::release(const char *why) {
    grace_->stop();
    if (!deferred_) return;
    deferred_ = false;
    qInfo("sailtube-timing watchnext released %s by %s", qPrintable(videoId_), why);
    reload();
}

void WatchInfo::reload() {
    if (service_ && !videoId_.isEmpty()) {
        service_->request(videoId_);   // fire-and-forget; joins a request in flight
        page_ = service_->page(videoId_);
        related_->setVideo(videoId_);
        emit requested(videoId_);
    }
    refresh();
}

void WatchInfo::pageChanged(const QString &videoId) {
    if (videoId != videoId_ || !service_) return;
    const std::shared_ptr<const catalog::WatchPage> p = service_->page(videoId);
    if (p) page_ = p;
    refresh();
}

void WatchInfo::refresh() {
    if (page_) {
        const std::vector<catalog::DescriptionRun> &runs = page_->info.descriptionRuns;
        bool truncated = false;
        const std::vector<catalog::DescriptionRun> shortRuns =
            catalog::collapseLines(runs, catalog::kCollapsedDescriptionLines, &truncated);
        styled_ = q(catalog::runsToStyledText(runs));
        collapsedStyled_ = q(catalog::runsToStyledText(shortRuns));
        truncated_ = truncated;
    } else {
        styled_.clear();
        collapsedStyled_.clear();
        truncated_ = false;
    }
    updateChapters();
    emit changed();
    emit collapsedChanged();
}

void WatchInfo::updateChapters() {
    std::vector<catalog::ChapterMark> list;
    bool fromResponse = false;
    if (page_) list = page_->chapters(durationHint_ > 0 ? static_cast<std::int64_t>(durationHint_) : -1, &fromResponse);
    if (list.empty() && !fallbackChapters_.isEmpty()) {
        for (const QVariant &v : fallbackChapters_) {
            const QVariantMap m = v.toMap();
            list.push_back(catalog::ChapterMark(m.value(QStringLiteral("title")).toString().toStdString(),
                                                static_cast<std::int64_t>(m.value(QStringLiteral("start")).toDouble() / 1000.0)));
        }
        const std::int64_t duration = page_ && page_->info.durationSeconds > 0 ? page_->info.durationSeconds
                                                                              : static_cast<std::int64_t>(durationHint_);
        list = catalog::finalizeChapters(list, duration);
        fromResponse = false;
    }
    chapters_->setChapters(list, fromResponse);
}

bool WatchInfo::loading() const { return page_ && page_->loading(); }
bool WatchInfo::loaded() const { return page_ && page_->nextDone && !catalog::isFailure(page_->next.outcome); }

QString WatchInfo::errorText() const {
    if (!page_ || !page_->nextDone || !catalog::isFailure(page_->next.outcome)) return QString();
    return watchFailureText(page_->next.outcome);
}

QString WatchInfo::title() const { return page_ ? q(page_->info.title) : QString(); }
double WatchInfo::viewCount() const { return page_ ? static_cast<double>(page_->info.viewCount) : -1; }
QString WatchInfo::viewText() const { return page_ ? q(page_->info.viewText) : QString(); }
double WatchInfo::likeCount() const { return page_ ? static_cast<double>(page_->info.likeCount) : -1; }
QString WatchInfo::likeText() const { return page_ ? q(page_->info.likeText) : QString(); }

QString WatchInfo::dateText() const {
    if (!page_) return QString();
    if (page_->info.publishedEpoch >= 0) {
        const QDate d = QDateTime::fromMSecsSinceEpoch(page_->info.publishedEpoch * 1000, Qt::UTC).date();
        return QLocale().toString(d, QLocale::ShortFormat);
    }
    if (!page_->info.dateText.empty()) return q(page_->info.dateText);
    return q(page_->info.relativeDateText);
}

bool WatchInfo::upcoming() const { return page_ && page_->info.upcoming; }

double WatchInfo::scheduledStart() const {
    if (!page_ || !page_->info.upcoming || page_->info.scheduledStartEpoch <= 0) return 0;
    return static_cast<double>(page_->info.scheduledStartEpoch) * 1000.0;
}

void WatchInfo::setDislikeEstimate(double count, const QString &text) {
    const double value = count < 0 ? -1 : count;
    const QString shown = value < 0 ? QString() : text;
    if (value == dislikeCount_ && shown == dislikeText_) return;
    dislikeCount_ = value;
    dislikeText_ = shown;
    emit changed();
}

double WatchInfo::publishedEpoch() const { return page_ ? static_cast<double>(page_->info.publishedEpoch) : -1; }
double WatchInfo::subscriberCount() const { return page_ ? static_cast<double>(page_->info.subscriberCount) : -1; }
QString WatchInfo::subscriberText() const { return page_ ? q(page_->info.subscriberText) : QString(); }
QString WatchInfo::owner() const { return page_ ? q(page_->info.ownerName) : QString(); }
QString WatchInfo::ownerId() const { return page_ ? q(page_->info.ownerId) : QString(); }
QString WatchInfo::ownerHandle() const { return page_ ? q(page_->info.ownerHandle) : QString(); }
QString WatchInfo::avatar() const { return page_ ? q(page_->info.ownerAvatar) : QString(); }
QString WatchInfo::thumbnail() const {
    return catalog::isVideoId(videoId_.toStdString()) ? q(catalog::detailThumbnailUrl(videoId_.toStdString())) : QString();
}
double WatchInfo::durationSeconds() const { return page_ ? static_cast<double>(page_->info.durationSeconds) : -1; }
bool WatchInfo::live() const { return page_ && page_->info.live; }
QString WatchInfo::description() const { return page_ ? q(page_->info.description) : QString(); }
QString WatchInfo::commentsContinuation() const { return page_ ? page_->commentsContinuation : QString(); }

QVariantList WatchInfo::descriptionRuns() const {
    QVariantList list;
    if (!page_) return list;
    for (const catalog::DescriptionRun &r : page_->info.descriptionRuns) {
        QVariantMap m;
        m.insert(QStringLiteral("kind"), QString::fromLatin1(kindName(r.kind)));
        m.insert(QStringLiteral("text"), q(r.text));
        if (r.kind == catalog::RunKind::Link) m.insert(QStringLiteral("url"), q(r.url));
        if (r.kind == catalog::RunKind::Timestamp) m.insert(QStringLiteral("seconds"), static_cast<double>(r.seekSeconds));
        if (r.kind == catalog::RunKind::Hashtag) m.insert(QStringLiteral("hashtag"), q(r.hashtag));
        list.append(m);
    }
    return list;
}

void WatchInfo::setCollapsed(bool collapsed) {
    if (collapsed == collapsed_) return;
    collapsed_ = collapsed;
    emit collapsedChanged();
}

void WatchInfo::setDurationHint(double seconds) {
    if (seconds == durationHint_) return;
    durationHint_ = seconds;
    updateChapters();
    emit changed();
}

void WatchInfo::setFallbackChapters(const QVariantList &chapters) {
    if (chapters == fallbackChapters_) return;
    fallbackChapters_ = chapters;
    updateChapters();
    emit changed();
}

QString WatchInfo::shareUrl(bool withTime, double seconds) const {
    const std::int64_t t = withTime && seconds > 0 ? static_cast<std::int64_t>(seconds) : 0;
    return q(catalog::shareUrl(videoId_.toStdString(), t));
}

QString WatchInfo::browserUrl(bool withTime, double seconds) const {
    const std::int64_t t = withTime && seconds > 0 ? static_cast<std::int64_t>(seconds) : 0;
    return q(catalog::watchUrl(videoId_.toStdString(), t));
}

QString WatchInfo::openInBrowser(bool withTime, double seconds) {
    const QString url = browserUrl(withTime, seconds);
    if (!url.isEmpty()) emit openUrlRequested(url);
    return url;
}

void WatchInfo::activateLink(const QString &href) {
    const catalog::LinkAction a = catalog::parseLinkAnchor(href.toStdString());
    switch (a.kind) {
    case catalog::RunKind::Timestamp: emit seekRequested(static_cast<double>(a.seekSeconds)); break;
    case catalog::RunKind::Hashtag: emit hashtagRequested(q(a.hashtag)); break;
    case catalog::RunKind::Link: emit openUrlRequested(q(a.url)); break;
    case catalog::RunKind::Text: break;
    }
}

}}}
