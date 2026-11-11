// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Extracted unchanged (apart from moving out of an anonymous namespace) from
// SailTune's Core/Streaming/Infrastructure/YtDlpStreamResolver.cpp at e190ac2.
// Proposed home of the shared runtime library; see docs/SHARING.md.
#pragma once

#include <QProcess>
#include <limits>
#ifdef Q_OS_UNIX
#include <cerrno>
#include <signal.h>
#include <unistd.h>
#endif

namespace nordtile { namespace runtime {

// A QProcess whose child leads its own process group, so the resolver and the
// JavaScript runtime it spawns can be stopped together.
class ResolverProcess final : public QProcess {
public:
    explicit ResolverProcess(QObject *parent) : QProcess(parent) {
        connect(this, &QProcess::started, this, [this]() {
            // setupChildProcess either establishes this group or exits before
            // exec. Retain its ID only to query quiescence after the leader dies.
            groupId_ = processId();
        });
    }
    void stopGroup() {
#ifdef Q_OS_UNIX
        const qint64 child = processId();
        // A separate process group also contains the JS runtime. Never signal
        // the application group if setpgid failed or the child already exited.
        if (child > 0 && child <= std::numeric_limits<pid_t>::max() &&
            ::getpgid(static_cast<pid_t>(child)) == static_cast<pid_t>(child))
            ::kill(-static_cast<pid_t>(child), SIGKILL);
#endif
        kill();
    }
    bool groupQuiet() const {
        if (state() != QProcess::NotRunning) return false;
#ifdef Q_OS_UNIX
        if (groupId_ > 0 && groupId_ <= std::numeric_limits<pid_t>::max()) {
            // Never signal a saved group: its ID might have been reused. A
            // live/unknown group keeps the directory and slot unavailable.
            if (::kill(-static_cast<pid_t>(groupId_), 0) == 0 || errno != ESRCH) return false;
        }
#endif
        return true;
    }
protected:
#ifdef Q_OS_UNIX
    void setupChildProcess() override { if (::setpgid(0, 0) != 0) ::_exit(125); }
#endif
private:
    qint64 groupId_ = 0;
};

}}
