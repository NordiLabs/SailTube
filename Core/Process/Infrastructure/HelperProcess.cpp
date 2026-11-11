// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "HelperProcess.h"

#include <QFileInfo>
#include <QMetaObject>
#include <QMutexLocker>
#include <QThread>
#include <limits>

#ifdef Q_OS_UNIX
#include <csignal>
#include <unistd.h>
#endif
#ifdef Q_OS_LINUX
#include <sys/prctl.h>
#endif

namespace nordtile { namespace process {

bool HelperProcess::isAbsoluteExecutable(const QString &program) {
    const QFileInfo info(program);
    return !program.isEmpty() && info.isAbsolute() && info.isFile() && info.isExecutable();
}

bool HelperProcess::startHelper(const QString &absoluteProgram, const QStringList &arguments) {
    if (!isAbsoluteExecutable(absoluteProgram)) return false;
    start(absoluteProgram, arguments);
    return waitForStarted(5000);
}

void HelperProcess::stopGroup() {
#ifdef Q_OS_UNIX
    const qint64 child = processId();
    if (child > 0 && child <= std::numeric_limits<pid_t>::max() &&
        ::getpgid(static_cast<pid_t>(child)) == static_cast<pid_t>(child))
        ::kill(-static_cast<pid_t>(child), SIGKILL);
#endif
    kill();
    waitForFinished(2000);
}

#ifdef Q_OS_UNIX
void HelperProcess::setupChildProcess() {
    // Runs in the child between fork and exec: async-signal-safe calls only.
    const pid_t parent = ::getppid();
    if (::setpgid(0, 0) != 0) ::_exit(125);
#ifdef Q_OS_LINUX
    if (::prctl(PR_SET_PDEATHSIG, SIGKILL) != 0) ::_exit(125);
#endif
    if (::getppid() != parent) ::_exit(125);   // the parent died before the signal was armed
}
#endif

qint64 HelperOwner::startHelper(const HelperSpec &spec) {
    HelperProcess *p = new HelperProcess(this);
    p->setProcessEnvironment(spec.environment);
    p->setProcessChannelMode(QProcess::ForwardedErrorChannel);
    if (!p->startHelper(spec.helperProgram, spec.arguments)) { delete p; return -1; }
    const qint64 pid = p->processId();
    {
        QMutexLocker lock(&mutex_);
        helpers_.insert(pid, p);
    }
    QObject::connect(p, static_cast<void (QProcess::*)(int, QProcess::ExitStatus)>(&QProcess::finished), this, [this, pid, p]() {
        { QMutexLocker lock(&mutex_); helpers_.remove(pid); }
        p->deleteLater();
    });
    return pid;
}

bool HelperOwner::stopHelper(qint64 pid) {
    HelperProcess *p = nullptr;
    { QMutexLocker lock(&mutex_); p = helpers_.take(pid); }
    if (!p) return false;
    p->disconnect(this);
    p->stopGroup();
    delete p;
    return true;
}

void HelperOwner::stopAll() {
    QList<HelperProcess *> all;
    { QMutexLocker lock(&mutex_); all = helpers_.values(); helpers_.clear(); }
    for (HelperProcess *p : all) { p->disconnect(this); p->stopGroup(); delete p; }
}

int HelperOwner::running() const { QMutexLocker lock(&mutex_); return helpers_.size(); }

HelperHost::HelperHost(QObject *parent) : QObject(parent), thread_(new QThread), owner_(new HelperOwner) {
    qRegisterMetaType<nordtile::process::HelperSpec>("nordtile::process::HelperSpec");
    thread_->setObjectName(QStringLiteral("helper-owner"));
    owner_->moveToThread(thread_);
    QObject::connect(thread_, &QThread::finished, owner_, &QObject::deleteLater);
    thread_->start();
}

HelperHost::~HelperHost() {
    QMetaObject::invokeMethod(owner_, "stopAll", Qt::BlockingQueuedConnection);
    thread_->quit();
    thread_->wait();
    delete thread_;
}

qint64 HelperHost::start(const HelperSpec &spec) {
    if (!HelperProcess::isAbsoluteExecutable(spec.helperProgram)) return -1;
    qint64 pid = -1;
    if (QThread::currentThread() == thread_) return owner_->startHelper(spec);
    QMetaObject::invokeMethod(owner_, "startHelper", Qt::BlockingQueuedConnection, Q_RETURN_ARG(qint64, pid),
                              Q_ARG(nordtile::process::HelperSpec, spec));
    return pid;
}

bool HelperHost::stop(qint64 pid) {
    if (QThread::currentThread() == thread_) return owner_->stopHelper(pid);
    bool ok = false;
    QMetaObject::invokeMethod(owner_, "stopHelper", Qt::BlockingQueuedConnection, Q_RETURN_ARG(bool, ok), Q_ARG(qint64, pid));
    return ok;
}

}}
