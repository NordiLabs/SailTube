// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#pragma once

// Owner-thread glue for MemoryTrimPolicy: watches the player, runs the settle timer after
// "teardown-done" and the 15 s idle timer, and calls trimMalloc() only when the policy says so
// (nothing loading or playing). Logs "sailtube-timing: malloc-trim before=<kB> after=<kB> ms=<n> why=teardown|idle"
// (or "malloc-trim skipped why=disabled|settle-restarted|busy") when
// SAILTUBE_TIMING is on. No Q_OBJECT: lambdas on the timers' own thread.

#include "Core/Player/Domain/MemoryTrimPolicy.h"

#include <QObject>
#include <QTimer>

namespace nordtile { namespace player { class VideoPlayer; } }

namespace nordtile { namespace video { namespace sailfish {

class MemoryTrimScheduler : public QObject {
public:
    MemoryTrimScheduler(player::VideoPlayer *player, QObject *parent = nullptr);

private:
    void refresh();
    void trim(const char *why);
    void skipped(const char *why);

    player::VideoPlayer *player_;
    player::MemoryTrimPolicy policy_;
    QTimer settle_;
    QTimer idle_;
};

} } } // namespace nordtile::video::sailfish
