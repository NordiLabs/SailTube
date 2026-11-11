// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#pragma once

// PLT-14 process discipline for helper children (resolver, JS runtime, future PO provider).
//
// - HelperProcess: a QProcess whose child leads its own process group and has a parent-death
//   signal (Linux PR_SET_PDEATHSIG = SIGKILL). The signal fires when the *thread* that forked the
//   child exits, so helpers must be started from a thread that lives as long as the app: the GUI
//   thread or HelperHost's owner thread. The child also exits at once if its parent already died
//   between fork and prctl.
// - Program paths must be absolute: GUI launches have a trimmed PATH, so nothing is looked up.
// - HelperHost: one long-lived owner thread. start() may be called from any thread (also from a
//   short-lived worker); the process is created and owned on the owner thread.
// Debug log format: set QT_MESSAGE_PATTERN in the environment, never the message-pattern API: SFOS
// exports the variable and it overrides the API. tests/process checks both.

#include <QHash>
#include <QMetaType>
#include <QMutex>
#include <QObject>
#include <QProcess>
#include <QProcessEnvironment>
#include <QStringList>

class QThread;

namespace nordtile { namespace process {

class HelperProcess : public QProcess {
public:
    explicit HelperProcess(QObject *parent = nullptr) : QProcess(parent) {}
    // Refuses relative program names (no PATH lookup). Returns false without starting.
    bool startHelper(const QString &absoluteProgram, const QStringList &arguments);
    // Kills the child's whole process group, then the child.
    void stopGroup();
    static bool isAbsoluteExecutable(const QString &program);

protected:
#ifdef Q_OS_UNIX
    void setupChildProcess() override;
#endif
};

struct HelperSpec {
    QString helperProgram;                 // absolute path
    QStringList arguments;
    QProcessEnvironment environment = QProcessEnvironment::systemEnvironment();
};

// Lives on HelperHost's owner thread and owns the helper processes there.
class HelperOwner : public QObject {
    Q_OBJECT
public:
    Q_INVOKABLE qint64 startHelper(const nordtile::process::HelperSpec &spec);
    Q_INVOKABLE bool stopHelper(qint64 pid);
    Q_INVOKABLE void stopAll();
    int running() const;

private:
    mutable QMutex mutex_;
    QHash<qint64, HelperProcess *> helpers_;
};

class HelperHost : public QObject {
    Q_OBJECT
public:
    explicit HelperHost(QObject *parent = nullptr);
    ~HelperHost() override;   // stops every helper, then the owner thread
    // Starts the helper on the owner thread; blocks the caller until it started or failed.
    // Returns the child pid, or -1 (relative path, missing program, start failure).
    qint64 start(const HelperSpec &spec);
    // Stops one helper's process group (any thread). Returns false if unknown.
    bool stop(qint64 pid);
    int running() const { return owner_->running(); }

private:
    QThread *thread_;
    HelperOwner *owner_;
};

}}

Q_DECLARE_METATYPE(nordtile::process::HelperSpec)
