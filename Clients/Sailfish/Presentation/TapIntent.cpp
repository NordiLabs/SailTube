// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "TapIntent.h"

#include <QDateTime>
#include <QRegularExpression>

namespace nordtile { namespace video { namespace sailfish {

TapIntent::TapIntent(QObject *parent, std::function<qint64()> nowMs) : QObject(parent), nowMs_(std::move(nowMs)) {
    if (!nowMs_) nowMs_ = [] { return QDateTime::currentMSecsSinceEpoch(); };
    settle_.setSingleShot(true);
    settle_.setInterval(kSettleMs);
    connect(&settle_, &QTimer::timeout, this, [this] { flushVisible(); });
}

void TapIntent::setTimings(int settleMs, int intervalMs) {
    settle_.setInterval(settleMs);
    intervalMs_ = intervalMs;
}

bool TapIntent::isVideoId(const QString &id) {
    static const QRegularExpression re(QStringLiteral("^[A-Za-z0-9_-]{11}$"));
    return re.match(id).hasMatch();
}

void TapIntent::rowsVisible(const QStringList &videoIds) {
    QStringList ids;
    for (const QString &id : videoIds) {
        if (isVideoId(id) && !ids.contains(id)) ids.append(id);
        if (ids.size() >= kMaxVisible) break;
    }
    pendingVisible_ = ids;
    settle_.start();   // restarts while the list keeps moving
}

void TapIntent::announceNext(const QStringList &videoIds) {
    QStringList ids;
    for (const QString &id : videoIds) {
        if (isVideoId(id) && !ids.contains(id)) ids.append(id);
        if (ids.size() >= kMaxVisible) break;
    }
    if (!active() || ids.isEmpty() || ids == lastVisible_) return;
    lastVisible_ = ids;
    lastVisibleMs_ = nowMs_();
    emit likelyVisible(ids);
}

void TapIntent::flushVisible() {
    if (!active() || pendingVisible_.isEmpty() || pendingVisible_ == lastVisible_) return;
    const qint64 now = nowMs_();
    if (lastVisibleMs_ >= 0 && now - lastVisibleMs_ < intervalMs_) {
        settle_.start(static_cast<int>(intervalMs_ - (now - lastVisibleMs_)));   // the newest set, later
        return;
    }
    lastVisible_ = pendingVisible_;
    lastVisibleMs_ = now;
    emit likelyVisible(lastVisible_);
}

void TapIntent::pressed(const QString &videoId, const QVariantMap &hints) {
    if (!isVideoId(videoId) || !active()) return;
    down_ = videoId;
    downId_ = ++nextId_;
    emit touchDown(videoId, hints, downId_);
}

void TapIntent::released(const QString &videoId, bool clicked) {
    if (videoId != down_) return;
    const int id = downId_;
    down_.clear();
    downId_ = 0;
    if (!clicked) emit touchCancelled(videoId, id);
}

void TapIntent::opening(const QString &videoId) {
    if (!isVideoId(videoId)) return;
    const qint64 now = nowMs_();
    if (videoId == lastOpened_ && lastOpenedMs_ >= 0 && now - lastOpenedMs_ < kOpenDedupMs) return;
    const int id = videoId == down_ && downId_ > 0 ? downId_ : ++nextId_;
    down_.clear();
    downId_ = 0;
    lastOpened_ = videoId;
    lastOpenedMs_ = now;
    emit opened(videoId, id);
}

}}}
