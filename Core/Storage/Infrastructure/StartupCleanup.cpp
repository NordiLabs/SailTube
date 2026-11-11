// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "StartupCleanup.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QRegularExpression>

#ifdef Q_OS_UNIX
#include <cerrno>
#include <signal.h>
#endif

namespace nordtile { namespace storage {

namespace {
bool processAlive(qint64 pid) {
#ifdef Q_OS_UNIX
    if (pid <= 0) return false;
    return ::kill(static_cast<pid_t>(pid), 0) == 0 || errno == EPERM;
#else
    Q_UNUSED(pid);
    return true;   // unknown: keep it
#endif
}

bool staleLock(const QString &path) {
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) return false;
    const QByteArray first = f.readLine(64).trimmed();
    bool ok = false;
    const qint64 pid = first.toLongLong(&ok);
    return !ok || !processAlive(pid);   // unreadable content: QLockFile cannot use it either
}
}

bool isAtomicWriteLeftover(const QString &fileName, const QStringList &siblings) {
    static const QRegularExpression leftover(QStringLiteral("^(.+)\\.[A-Za-z0-9]{6}$"));
    static const QRegularExpression storeLike(QStringLiteral("\\.(conf|ini|json|db|sqlite|mpd|txt)$"));
    const QRegularExpressionMatch m = leftover.match(fileName);
    if (!m.hasMatch()) return false;
    // Temp names are random [A-Za-z0-9]; a word suffix such as ".backup" is a real file.
    const QString suffix = fileName.right(6);
    if (suffix == suffix.toLower() && !suffix.contains(QRegularExpression(QStringLiteral("[0-9]")))) return false;
    const QString base = m.captured(1);
    return storeLike.match(base).hasMatch() || siblings.contains(base);
}

CleanupReport removeOrphans(const QStringList &directories) {
    CleanupReport report;
    for (const QString &path : directories) {
        if (path.isEmpty()) continue;
        QDir dir(path);
        if (!dir.exists()) continue;
        const QStringList files = dir.entryList(QDir::Files | QDir::Hidden | QDir::NoDotAndDotDot);
        for (const QString &name : files) {
            const QString full = dir.filePath(name);
            bool remove = false;
            if (name.endsWith(QLatin1String(".tmp")) || name.endsWith(QLatin1String(".spool"))) {
                remove = true;
            } else if (name.endsWith(QLatin1String(".lock"))) {
                if (staleLock(full)) remove = true; else ++report.kept;
            } else if (!name.endsWith(QLatin1String(".part")) && isAtomicWriteLeftover(name, files)) {
                remove = true;
            }
            if (remove && QFile::remove(full)) report.removed << name;
        }
    }
    return report;
}

}}
