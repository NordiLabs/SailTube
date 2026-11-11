// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#pragma once

// RES-14 [FULL] optional PO-token provider manager. Compiled only in the FULL build: the STORE
// build has no runtime helper (§1.5), so this header refuses to compile there.
//
// Flow (decisions in Core/Resolver/Domain/PoTokenPolicy.h):
//  1. setEnabled(true): aarch64 only. The staged copy under root (put there by the user's opt-in
//     helper install, NFR-SEC-04) is hashed in small slices on this thread and compared with the
//     build's pin (tag, commit, lockfile, plugin zip, Deno binary).
//  2. Unless the marker says this exact pin is installed: `deno install --frozen`; the lockfile
//     is re-hashed afterwards. Never an unlocked install.
//  3. The server starts on a free 127.0.0.1 port with the Domain's Deno command line and a clean
//     environment; its output is discarded.
//  4. Verifying: /proc/net/tcp and tcp6 are read (and /proc/<pid>/fd for the helper's own
//     sockets) until it listens; any listening socket outside 127.0.0.1 stops it and refuses.
//     The check repeats while ready.
//  5. Ready: extractorArguments() hands the yt-dlp runner the plugin directory and the bgutil
//     HTTP provider arguments. Nobody waits on the helper: until ready, resolves stay token-free.
// Failures back off (Domain backoffMs); refusals stay until the user toggles the setting.

#ifdef SAILTUBE_STORE
#error "PoTokenProvider is FULL only (RES-14); the STORE build has no runtime helper"
#endif

#include "../Domain/PoTokenPolicy.h"

#include <QByteArray>
#include <QObject>
#include <QString>
#include <QStringList>
#include <functional>

class QProcess;
class QTimer;

namespace nordtile { namespace video { namespace resolver {

struct PoTokenProviderOptions {
    QString root;                       // absolute helper root, e.g. <AppData>/potoken
    int heapMb = potoken::defaultHeapMb;
    potoken::Pin pin = potoken::builtInPin();
    QString procRoot;                   // empty: the kernel's proc file system (tests: a fixture tree)
    QString architecture;               // empty: the running kernel's (uname machine)
    potoken::Timing timing;
    int pollMs = 250;                   // bind checks while verifying
    int recheckMs = 30000;              // bind checks while ready
    int installTimeoutMs = 300000;
    int hashSliceBytes = 1024 * 1024;   // per event-loop turn
    std::function<qint64()> clock;      // monotonic ms; default QElapsedTimer
};

class PoTokenProvider final : public QObject {
    Q_OBJECT
public:
    explicit PoTokenProvider(const PoTokenProviderOptions &, QObject *parent = nullptr);
    ~PoTokenProvider() override;

    // RES-14: the user's opt-in (AppSettings::poTokenProvider). Off stops the helper.
    void setEnabled(bool);
    bool enabled() const { return enabled_; }
    // RES-14 configurable heap ceiling; applies from the next start.
    void setHeapMb(int);

    potoken::State state() const { return machine_.state(); }
    bool ready() const { return machine_.ready(); }
    // Fixed texts only (RES-14 "show the status in diagnostics").
    QString stateName() const;
    QString reasonText() const;
    QString diagnostics() const;       // e.g. "ready http://127.0.0.1:40123" / "refused: <reason>"
    int port() const { return ready() ? port_ : 0; }
    QString baseUrl() const;           // empty unless ready
    qint64 helperPid() const;          // 0 when not running

    // RES-08 step 4 / RES-14: the yt-dlp arguments for a request with poProvider = true;
    // empty unless ready. Never a cookie option.
    QStringList extractorArguments() const;

    // Detected architecture name ("aarch64", "armv7l", ...).
    static QString runningArchitecture();

signals:
    void stateChanged();

private:
    struct Hashing;
    qint64 now() const;
    void apply(potoken::Action);
    void checkFiles();
    void hashNext();
    void filesHashed();
    void runInstall();
    void installEnded(bool ok);
    void startHelper();
    void stopHelper();
    void poll();
    void helperEnded();
    void scheduleRetry();
    void changed();
    potoken::BindVerdict readBind() const;
    int freeLoopbackPort() const;

    PoTokenProviderOptions options_;
    potoken::Layout layout_;
    potoken::Machine machine_;
    bool enabled_ = false;
    int port_ = 0;
    QProcess *install_ = nullptr;
    QProcess *helper_ = nullptr;
    QTimer *installDeadline_ = nullptr;
    QTimer *pollTimer_ = nullptr;
    QTimer *retryTimer_ = nullptr;
    Hashing *hashing_ = nullptr;
    potoken::State lastState_ = potoken::State::Off;
    potoken::Reason lastReason_ = potoken::Reason::Disabled;
};

}}}
