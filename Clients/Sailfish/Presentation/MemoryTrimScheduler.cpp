// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "Clients/Sailfish/Presentation/MemoryTrimScheduler.h"
#include "Clients/Sailfish/Presentation/PlaybackTiming.h"
#include "Core/Player/Infrastructure/MemoryTrim.h"
#include "Core/Player/Infrastructure/VideoPlayer.h"

#include <QDebug>

namespace nordtile { namespace video { namespace sailfish {

MemoryTrimScheduler::MemoryTrimScheduler(player::VideoPlayer *player, QObject *parent)
    : QObject(parent), player_(player) {
    if (!player::kMemoryTrimEnabled || !player_) return;
    settle_.setSingleShot(true);
    settle_.setInterval(player::kTrimSettleMs);
    idle_.setSingleShot(true);
    idle_.setInterval(player::kTrimIdleMs);
    connect(&settle_, &QTimer::timeout, this, [this]() {
        refresh();
        const bool pending = policy_.teardownPending();
        if (policy_.shouldTrimAfterTeardown()) trim("teardown");
        else if (pending) skipped("busy");   // the next video is already loading/playing
    });
    connect(&idle_, &QTimer::timeout, this, [this]() {
        refresh();
        if (policy_.shouldTrimIdle()) trim("idle");
    });
    connect(player_, &player::VideoPlayer::stateChanged, this, [this]() { refresh(); });
    connect(player_, &player::VideoPlayer::timing, this, [this](const QString &marker, qint64) {
        if (marker != QLatin1String("teardown-done")) return;
        if (!policy_.teardownTrimEnabled()) {
            skipped("disabled");
            return;
        }
        // The settle timer is single-shot and teardownPending is one flag: a teardown inside the
        // window of the previous one restarts the timer and folds into it (one trim, not two).
        if (settle_.isActive()) skipped("settle-restarted");
        policy_.teardownDone();
        settle_.start();
    });
    refresh();
}

void MemoryTrimScheduler::refresh() {
    const player::VideoPlayer::State st = player_->state();
    policy_.setBusy(st == player::VideoPlayer::Loading || st == player::VideoPlayer::Playing);
    if (policy_.busy()) idle_.stop();
    else if (policy_.idleTimerWanted() && !idle_.isActive()) idle_.start();
}

void MemoryTrimScheduler::trim(const char *why) {
    long before = 0, after = 0;
    double ms = 0;
    if (!player::trimMalloc(&before, &after, &ms)) return;
    if (timingEnabled())
        qInfo("sailtube-timing: malloc-trim before=%ld after=%ld ms=%.2f why=%s", before, after, ms, why);
}

void MemoryTrimScheduler::skipped(const char *why) {
    if (timingEnabled()) qInfo("sailtube-timing: malloc-trim skipped why=%s", why);
}

} } }
