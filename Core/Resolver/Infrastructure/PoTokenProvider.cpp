// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "PoTokenProvider.h"

#include <QCryptographicHash>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QHostAddress>
#include <QPointer>
#include <QProcess>
#include <QProcessEnvironment>
#include <QSysInfo>
#include <QTcpServer>
#include <QTimer>

#include <unistd.h>

namespace nordtile { namespace video { namespace resolver {

namespace {
// Socket tables (net/tcp, net/tcp6) and the helper's own descriptors (<pid>/fd), read only.
const char kernelProc[] = "/proc";

QString q(const std::string &s) { return QString::fromStdString(s); }
std::string s(const QString &v) { return v.toStdString(); }

QElapsedTimer &monotonic() {
    static QElapsedTimer timer;
    if (!timer.isValid()) timer.start();
    return timer;
}

QByteArray readSmall(const QString &path, qint64 limit) {
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) return QByteArray();
    return f.read(limit);
}

// /proc files report size 0: read until end of file, bounded.
bool readProc(const QString &path, std::string *out) {
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) return false;
    QByteArray all;
    while (all.size() < 4 * 1024 * 1024) {
        const QByteArray chunk = f.read(65536);
        if (chunk.isEmpty()) break;
        all += chunk;
    }
    *out = std::string(all.constData(), static_cast<std::size_t>(all.size()));
    return true;
}

QString sha256Hex(const QString &path) {
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) return QString();
    QCryptographicHash h(QCryptographicHash::Sha256);
    while (!f.atEnd()) {
        const QByteArray chunk = f.read(1024 * 1024);
        if (chunk.isEmpty()) break;
        h.addData(chunk);
    }
    return QString::fromLatin1(h.result().toHex());
}

void configure(QProcess *p, const potoken::Command &c) {
    QProcessEnvironment environment; // empty: the Domain gives the complete environment
    for (const auto &kv : c.environment) environment.insert(q(kv.first), q(kv.second));
    p->setProcessEnvironment(environment);
    p->setWorkingDirectory(q(c.workingDirectory));
    p->setStandardInputFile(QProcess::nullDevice());
    p->setStandardOutputFile(QProcess::nullDevice());
    p->setStandardErrorFile(QProcess::nullDevice());
}

QStringList arguments(const potoken::Command &c) {
    QStringList a;
    for (const std::string &v : c.arguments) a << q(v);
    return a;
}

void stopProcess(QProcess *p) {
    if (!p) return;
    p->disconnect();
    if (p->state() != QProcess::NotRunning) {
        p->terminate();
        if (!p->waitForFinished(1000)) {
            p->kill();
            p->waitForFinished(500);
        }
    }
    p->deleteLater();
}
}

// Slice-wise hashing of the staged files, one slice per event-loop turn.
struct PoTokenProvider::Hashing {
    struct Item { QString path; std::string *target; };
    QList<Item> items;
    QFile file;
    QCryptographicHash hash{QCryptographicHash::Sha256};
    potoken::Staged staged;
    bool open = false;
};

PoTokenProvider::PoTokenProvider(const PoTokenProviderOptions &options, QObject *parent)
    : QObject(parent), options_(options) {
    layout_.root = s(options_.root);
    machine_ = potoken::Machine(options_.timing);
    options_.heapMb = potoken::clampHeapMb(options_.heapMb);
    if (options_.procRoot.isEmpty()) options_.procRoot = QString::fromLatin1(kernelProc);
    pollTimer_ = new QTimer(this);
    pollTimer_->setSingleShot(true);
    connect(pollTimer_, &QTimer::timeout, this, [this]() { poll(); });
    retryTimer_ = new QTimer(this);
    retryTimer_->setSingleShot(true);
    connect(retryTimer_, &QTimer::timeout, this, [this]() { apply(machine_.tick(now())); });
    installDeadline_ = new QTimer(this);
    installDeadline_->setSingleShot(true);
    connect(installDeadline_, &QTimer::timeout, this, [this]() {
        if (!install_) return;
        stopProcess(install_);
        install_ = nullptr;
        installEnded(false);
    });
}

PoTokenProvider::~PoTokenProvider() {
    delete hashing_;
    hashing_ = nullptr;
    if (install_) { install_->disconnect(); install_->kill(); install_->waitForFinished(500); delete install_; }
    if (helper_) {
        helper_->disconnect();
        helper_->terminate();
        if (!helper_->waitForFinished(1000)) { helper_->kill(); helper_->waitForFinished(500); }
        delete helper_;
    }
}

qint64 PoTokenProvider::now() const { return options_.clock ? options_.clock() : monotonic().elapsed(); }

QString PoTokenProvider::runningArchitecture() {
    // The build's architecture is the one that matters: an armv7hl package cannot run the
    // aarch64 Deno even on a 64-bit kernel. Qt names aarch64 "arm64".
    const QString a = QSysInfo::buildCpuArchitecture();
    return a == QLatin1String("arm64") ? QStringLiteral("aarch64") : a;
}

void PoTokenProvider::setEnabled(bool on) {
    if (on == enabled_) return;
    enabled_ = on;
    if (on) {
        const QString arch = options_.architecture.isEmpty() ? runningArchitecture() : options_.architecture;
        apply(machine_.enable(now(), s(arch)));
        return;
    }
    delete hashing_;
    hashing_ = nullptr;
    installDeadline_->stop();
    pollTimer_->stop();
    retryTimer_->stop();
    if (install_) { stopProcess(install_); install_ = nullptr; }
    machine_.disable();
    stopHelper();
    changed();
}

void PoTokenProvider::setHeapMb(int mb) { options_.heapMb = potoken::clampHeapMb(mb); }

QString PoTokenProvider::stateName() const { return QString::fromLatin1(potoken::stateName(machine_.state())); }
QString PoTokenProvider::reasonText() const { return QString::fromLatin1(potoken::reasonText(machine_.reason())); }

QString PoTokenProvider::diagnostics() const {
    if (ready()) return QStringLiteral("ready ") + baseUrl();
    if (machine_.state() == potoken::State::Off) return QStringLiteral("off");
    return stateName() + QStringLiteral(": ") + reasonText();
}

QString PoTokenProvider::baseUrl() const { return ready() ? q(potoken::baseUrl(port_)) : QString(); }

qint64 PoTokenProvider::helperPid() const {
    return helper_ && helper_->state() == QProcess::Running ? helper_->processId() : 0;
}

QStringList PoTokenProvider::extractorArguments() const {
    QStringList out;
    for (const std::string &a : potoken::extractorArguments(ready(), true, layout_.pluginDir(), port_)) out << q(a);
    return out;
}

void PoTokenProvider::apply(potoken::Action action) {
    switch (action) {
    case potoken::Action::None: break;
    case potoken::Action::CheckFiles: checkFiles(); break;
    case potoken::Action::Install: runInstall(); break;
    case potoken::Action::Start: startHelper(); break;
    case potoken::Action::Poll: pollTimer_->start(options_.pollMs); break;
    case potoken::Action::Stop: stopHelper(); break;
    }
    changed();
}

void PoTokenProvider::changed() {
    if (machine_.state() == potoken::State::Failed) scheduleRetry();
    if (machine_.state() == potoken::State::Ready && !pollTimer_->isActive()) pollTimer_->start(options_.recheckMs);
    if (machine_.state() == lastState_ && machine_.reason() == lastReason_) return;
    lastState_ = machine_.state();
    lastReason_ = machine_.reason();
    emit stateChanged();
}

void PoTokenProvider::scheduleRetry() {
    if (retryTimer_->isActive()) return;
    const qint64 wait = machine_.retryAtMs() - now();
    retryTimer_->start(static_cast<int>(qBound(qint64(0), wait, qint64(3600000))));
}

void PoTokenProvider::checkFiles() {
    delete hashing_;
    hashing_ = new Hashing;
    potoken::Staged &staged = hashing_->staged;
    const bool rootOk = potoken::validRoot(layout_.root);
    const QString stampPath = q(layout_.stamp());
    staged.present = rootOk && QFileInfo(stampPath).isFile() && QFileInfo(q(layout_.denoBinary())).isFile()
        && QFileInfo(q(layout_.denoBinary())).isExecutable() && QFileInfo(q(layout_.lockfile())).isFile()
        && QFileInfo(q(layout_.mainModule())).isFile() && QFileInfo(q(layout_.pluginZip())).isFile();
    if (staged.present) {
        const potoken::Staged stamp = potoken::parseStamp(readSmall(stampPath, 4096).toStdString());
        staged.providerTag = stamp.providerTag;
        staged.providerCommit = stamp.providerCommit;
        staged.denoVersion = stamp.denoVersion;
        hashing_->items << Hashing::Item{q(layout_.lockfile()), &staged.lockfileSha256}
                        << Hashing::Item{q(layout_.pluginZip()), &staged.pluginSha256}
                        << Hashing::Item{q(layout_.denoBinary()), &staged.denoSha256};
    }
    QPointer<PoTokenProvider> guard(this);
    QTimer::singleShot(0, this, [this, guard]() { if (guard) hashNext(); });
}

void PoTokenProvider::hashNext() {
    Hashing *h = hashing_;
    if (!h) return; // cancelled
    if (h->items.isEmpty()) { filesHashed(); return; }
    Hashing::Item &item = h->items.first();
    if (!h->open) {
        h->file.setFileName(item.path);
        h->hash.reset();
        if (!h->file.open(QIODevice::ReadOnly)) {
            h->staged.present = false;
            h->items.clear();
            filesHashed();
            return;
        }
        h->open = true;
    }
    const QByteArray chunk = h->file.read(qMax(4096, options_.hashSliceBytes));
    if (!chunk.isEmpty()) h->hash.addData(chunk);
    if (chunk.isEmpty() || h->file.atEnd()) {
        *item.target = h->hash.result().toHex().toStdString();
        h->file.close();
        h->open = false;
        h->items.removeFirst();
    }
    QPointer<PoTokenProvider> guard(this);
    QTimer::singleShot(0, this, [this, guard, h]() { if (guard && hashing_ == h) hashNext(); });
}

void PoTokenProvider::filesHashed() {
    const potoken::Staged staged = hashing_->staged;
    delete hashing_;
    hashing_ = nullptr;
    const potoken::Reason verdict = potoken::checkPin(options_.pin, staged);
    const std::string marker = readSmall(q(layout_.installMarker()), 4096).toStdString();
    const bool current = verdict == potoken::Reason::None
        && QFileInfo(q(layout_.serverDir()) + QStringLiteral("/node_modules")).isDir()
        && potoken::installCurrent(marker, options_.pin);
    apply(machine_.filesChecked(verdict, current, now()));
}

void PoTokenProvider::runInstall() {
    QFile::remove(q(layout_.installMarker()));
    const potoken::Command c = potoken::installCommand(layout_);
    install_ = new QProcess(this);
    configure(install_, c);
    QProcess *p = install_;
    connect(p, static_cast<void (QProcess::*)(int, QProcess::ExitStatus)>(&QProcess::finished), this,
            [this, p](int code, QProcess::ExitStatus status) {
        if (install_ != p) return;
        install_ = nullptr;
        installDeadline_->stop();
        p->deleteLater();
        installEnded(status == QProcess::NormalExit && code == 0);
    });
    connect(p, &QProcess::errorOccurred, this, [this, p](QProcess::ProcessError error) {
        if (install_ != p || error != QProcess::FailedToStart) return;
        install_ = nullptr;
        installDeadline_->stop();
        p->deleteLater();
        installEnded(false);
    });
    installDeadline_->start(options_.installTimeoutMs);
    p->start(q(c.program), arguments(c), QIODevice::ReadOnly);
}

void PoTokenProvider::installEnded(bool ok) {
    // RES-14: the lock is frozen; an install that changed it is refused, never retried unlocked.
    const bool lockUnchanged = sha256Hex(q(layout_.lockfile())) == q(options_.pin.lockfileSha256);
    if (ok && lockUnchanged) {
        QFile marker(q(layout_.installMarker()));
        if (marker.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
            marker.write(QByteArray::fromStdString(potoken::formatInstallMarker(options_.pin)));
            marker.close();
        }
    }
    apply(machine_.installFinished(ok, lockUnchanged, now()));
}

int PoTokenProvider::freeLoopbackPort() const {
    QTcpServer probe;
    if (!probe.listen(QHostAddress(QHostAddress::LocalHost), 0)) return 0;
    const int port = probe.serverPort();
    probe.close();
    return port;
}

void PoTokenProvider::startHelper() {
    stopHelper();
    port_ = freeLoopbackPort();
    const QString stubPath = q(layout_.webGpuStub());
    QDir().mkpath(QFileInfo(stubPath).absolutePath());
    QFile stub(stubPath);
    const bool stubOk = stub.open(QIODevice::WriteOnly | QIODevice::Truncate)
        && stub.write(potoken::webGpuStubSource()) > 0;
    stub.close();
    if (port_ <= 0 || !stubOk) { apply(machine_.started(false, now())); return; }
    const potoken::Command c = potoken::serveCommand(layout_, port_, options_.heapMb);
    helper_ = new QProcess(this);
    configure(helper_, c);
    QProcess *p = helper_;
    connect(p, &QProcess::started, this, [this, p]() {
        if (helper_ == p) apply(machine_.started(true, now()));
    });
    connect(p, &QProcess::errorOccurred, this, [this, p](QProcess::ProcessError error) {
        if (helper_ != p || error != QProcess::FailedToStart) return;
        helper_ = nullptr;
        p->deleteLater();
        apply(machine_.started(false, now()));
    });
    connect(p, static_cast<void (QProcess::*)(int, QProcess::ExitStatus)>(&QProcess::finished), this,
            [this, p](int, QProcess::ExitStatus) {
        if (helper_ != p) return;
        helper_ = nullptr;
        p->deleteLater();
        helperEnded();
    });
    p->start(q(c.program), arguments(c), QIODevice::ReadOnly);
}

void PoTokenProvider::stopHelper() {
    pollTimer_->stop();
    QProcess *p = helper_;
    helper_ = nullptr;
    stopProcess(p);
}

void PoTokenProvider::helperEnded() {
    pollTimer_->stop();
    apply(machine_.exited(now()));
}

void PoTokenProvider::poll() {
    if (!helper_) return;
    apply(machine_.bindChecked(readBind(), now()));
}

potoken::BindVerdict PoTokenProvider::readBind() const {
    std::string v4, v6;
    const bool have4 = readProc(options_.procRoot + QStringLiteral("/net/tcp"), &v4);
    const bool have6 = readProc(options_.procRoot + QStringLiteral("/net/tcp6"), &v6);
    if (!have4) return potoken::BindVerdict::Unavailable; // fail closed (tcp6 may be absent without IPv6)
    std::vector<potoken::SocketEntry> entries = potoken::parseProcNetTcp(v4, false);
    if (have6) {
        const std::vector<potoken::SocketEntry> e6 = potoken::parseProcNetTcp(v6, true);
        entries.insert(entries.end(), e6.begin(), e6.end());
    }
    std::set<unsigned long long> inodes;
    const qint64 pid = helperPid();
    if (pid > 0) {
        const QString fdDir = options_.procRoot + QStringLiteral("/%1/fd").arg(pid);
        for (const QString &name : QDir(fdDir).entryList(QDir::AllEntries | QDir::System | QDir::NoDotAndDotDot)) {
            char target[128];
            const QByteArray link = QFile::encodeName(fdDir + QLatin1Char('/') + name);
            const ssize_t n = ::readlink(link.constData(), target, sizeof(target) - 1);
            if (n <= 0) continue;
            target[n] = '\0';
            const unsigned long long inode = potoken::socketInode(target);
            if (inode) inodes.insert(inode);
        }
    }
    return potoken::checkBind(entries, port_, inodes);
}

}}}
