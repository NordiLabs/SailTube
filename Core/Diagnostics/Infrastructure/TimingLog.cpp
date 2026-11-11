// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "TimingLog.h"

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QSaveFile>

namespace nordtile { namespace diagnostics {

TimingLog::TimingLog(const QString &directory, qint64 maxBytes) : directory_(directory), maxBytes_(maxBytes) {}

QString TimingLog::filePath() const { return QDir(directory_).filePath(QStringLiteral("timing.log")); }

QString TimingLog::sanitizeToken(const QString &token) {
    QString out;
    for (const QChar c : token.left(48)) {
        const bool ok = (c >= QLatin1Char('a') && c <= QLatin1Char('z')) || (c >= QLatin1Char('A') && c <= QLatin1Char('Z')) ||
                        (c >= QLatin1Char('0') && c <= QLatin1Char('9')) || c == QLatin1Char('_') || c == QLatin1Char('.') ||
                        c == QLatin1Char('-');
        out += ok ? c : QLatin1Char('_');
    }
    return out.isEmpty() ? QStringLiteral("_") : out;
}

bool TimingLog::record(const QString &runId, const QString &phase, qint64 milliseconds) {
    if (!enabled_) return false;
    if (!QDir().mkpath(directory_)) return false;
    QFile file(filePath());
    const bool existed = file.exists();
    if (!file.open(QIODevice::WriteOnly | QIODevice::Append | QIODevice::Text)) return false;
    if (!existed) file.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner);
    const QString line = QDateTime::currentDateTimeUtc().toString(QStringLiteral("yyyy-MM-ddTHH:mm:ss.zzzZ")) +
                         QStringLiteral(" run=") + sanitizeToken(runId) + QStringLiteral(" phase=") + sanitizeToken(phase) +
                         QStringLiteral(" ms=") + QString::number(milliseconds < 0 ? -1 : milliseconds) + QLatin1Char('\n');
    const bool ok = file.write(line.toUtf8()) > 0;
    file.close();
    trimIfNeeded();
    return ok;
}

void TimingLog::trimIfNeeded() {
    QFile file(filePath());
    if (file.size() <= maxBytes_ || !file.open(QIODevice::ReadOnly)) return;
    QByteArray data = file.readAll();
    file.close();
    const int cut = data.indexOf('\n', data.size() / 2);
    if (cut < 0) return;
    QSaveFile save(filePath());
    if (!save.open(QIODevice::WriteOnly)) return;
    save.write(data.mid(cut + 1));
    if (save.commit()) QFile::setPermissions(filePath(), QFileDevice::ReadOwner | QFileDevice::WriteOwner);
}

void TimingLog::clear() { QFile::remove(filePath()); }

}}
