// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#pragma once

// "Likely next tap" signals for the playback component's speculative resolve (E12). The catalog's
// lists report what is on screen and where a finger went down; playback alone decides whether
// to resolve anything. Nothing here touches the network. Exposed to QML as `tapIntent`.
//
//  * likelyVisible(ids): up to kMaxVisible video ids fully on screen, top first, once a list has
//    been still for kSettleMs; only when the set changed, at most once per kVisibleIntervalMs.
//  * touchDown(id, hints, pressId): a finger went down on a video row (usually 100-300 ms before
//    the click); hints = the row's map as handed to PlaybackSession::open().
//  * touchCancelled(id, pressId): the press turned into a scroll or ended elsewhere.
//  * opened(id, pressId): the tap completed. Emitted on the same event-loop turn, strictly
//    before the page calls playback.open(); once per tap (a repeat for the same id within
//    kOpenDedupMs is dropped). pressId is the matching press, or a fresh one without a press.
// pressId increases monotonically from 1 and is never reused.
// Ids must be plain 11-character video ids; anything else is ignored. Nothing is emitted while
// the application is not active. QtCore only (the active state is injected).

#include <QObject>
#include <QStringList>
#include <QTimer>
#include <QVariantMap>
#include <functional>

namespace nordtile { namespace video { namespace sailfish {

class TapIntent final : public QObject {
    Q_OBJECT
public:
    static const int kSettleMs = 400;
    static const int kVisibleIntervalMs = 2000;
    static const int kMaxVisible = 3;
    static const int kOpenDedupMs = 1000;

    explicit TapIntent(QObject *parent = nullptr, std::function<qint64()> nowMs = std::function<qint64()>());

    // Whether the application is in front (default: always).
    void setActive(const std::function<bool()> &active) { active_ = active; }
    void setTimings(int settleMs, int intervalMs);   // tests

    // From the lists (QML).
    Q_INVOKABLE void rowsVisible(const QStringList &videoIds);
    // A swipe feed (Shorts) knows what comes next: emits likelyVisible(ids) at once (no settle,
    // no interval), when the set changed and the app is active.
    Q_INVOKABLE void announceNext(const QStringList &videoIds);
    Q_INVOKABLE void pressed(const QString &videoId, const QVariantMap &hints = QVariantMap());
    Q_INVOKABLE void released(const QString &videoId, bool clicked);
    Q_INVOKABLE void opening(const QString &videoId);

    static bool isVideoId(const QString &id);

signals:
    void likelyVisible(const QStringList &videoIds);
    void touchDown(const QString &videoId, const QVariantMap &hints, int pressId);
    void touchCancelled(const QString &videoId, int pressId);
    void opened(const QString &videoId, int pressId);

private:
    bool active() const { return !active_ || active_(); }
    void flushVisible();

    std::function<qint64()> nowMs_;
    std::function<bool()> active_;
    QTimer settle_;
    QStringList pendingVisible_;
    QStringList lastVisible_;
    qint64 lastVisibleMs_ = -1;
    int intervalMs_ = kVisibleIntervalMs;
    QString down_;
    int downId_ = 0;
    int nextId_ = 0;
    QString lastOpened_;
    qint64 lastOpenedMs_ = -1;
};

}}}
