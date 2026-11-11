// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "StartupTiming.h"

#include <QCoreApplication>
#include <QFile>
#include <QMetaObject>
#include <QtGlobal>

#ifdef Q_OS_LINUX
#include <unistd.h>
#endif

namespace nordtile { namespace video { namespace sailfish {

StartupTiming::StartupTiming(const QElapsedTimer &clock, QObject *parent)
    : QObject(parent), clock_(clock) {}

void StartupTiming::log(const QString &line) {
    lines_ << line;
    qInfo("%s", qPrintable(line));
}

void StartupTiming::mark(const QString &stage) {
    if (stage.isEmpty() || marked_.contains(stage)) return;
    marked_.insert(stage);
    log(QStringLiteral("sailtube-timing startup %1 %2").arg(stage).arg(clock_.elapsed()));
    emit marked(stage);
    if (!exitStage_.isEmpty() && stage == exitStage_) {
        if (QCoreApplication *app = QCoreApplication::instance())
            QMetaObject::invokeMethod(app, "quit", Qt::QueuedConnection);
    }
}

qint64 StartupTiming::processAgeMs() {
#ifdef Q_OS_LINUX
    QFile stat(QStringLiteral("/proc/self/stat"));
    QFile uptime(QStringLiteral("/proc/uptime"));
    if (!stat.open(QIODevice::ReadOnly) || !uptime.open(QIODevice::ReadOnly)) return 0;
    const QByteArray s = stat.readAll();
    // Fields after the command name, which is in parentheses and may contain spaces.
    const QList<QByteArray> f = s.mid(s.lastIndexOf(')') + 2).split(' ');
    const long hz = ::sysconf(_SC_CLK_TCK);
    if (f.size() < 20 || hz <= 0) return 0;
    const double startedS = f.at(19).toDouble() / hz;   // field 22: start time, clock ticks since boot
    const double nowS = uptime.readAll().split(' ').value(0).toDouble();
    return qMax<qint64>(0, static_cast<qint64>((nowS - startedS) * 1000));
#else
    return 0;
#endif
}

void StartupTiming::setPreMainMs(qint64 ms) {
    preMainMs_ = qMax<qint64>(0, ms);
    log(QStringLiteral("sailtube-timing startup pre-main %1").arg(preMainMs_));
}

void StartupTiming::exitAfter(const QString &stage) {
    exitStage_ = stage;
    if (!stage.isEmpty() && marked_.contains(stage)) {
        if (QCoreApplication *app = QCoreApplication::instance())
            QMetaObject::invokeMethod(app, "quit", Qt::QueuedConnection);
    }
}

void StartupTiming::begin(const QString &name) {
    if (name.isEmpty()) return;
    intervals_.insert(name, clock_.elapsed());
    ended_.remove(name);
}

void StartupTiming::end(const QString &name, const QString &stage) {
    if (!intervals_.contains(name) || stage.isEmpty()) return;
    QSet<QString> &done = ended_[name];
    if (done.contains(stage)) return;
    done.insert(stage);
    log(QStringLiteral("sailtube-timing %1 %2 %3").arg(name, stage).arg(clock_.elapsed() - intervals_.value(name)));
}

}}}
