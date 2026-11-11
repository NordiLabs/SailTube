// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "TempDirSetup.h"

#include "Core/Player/Domain/TempDirPolicy.h"

#include <QDir>
#include <QFile>

namespace nordtile { namespace player {

QString prepareFlashTempDir(const QString &cacheDir, int *removed) {
    if (removed) *removed = 0;
    const std::string flash = flashTempDir(QDir::cleanPath(cacheDir).toStdString());
    if (flash.empty()) return QString();
    const QString path = QString::fromStdString(flash);
    QDir directory(path);
    if (kEmptyTempDirAtStartup && directory.exists()) {
        const QStringList entries = directory.entryList(QDir::AllEntries | QDir::Hidden | QDir::System | QDir::NoDotAndDotDot);
        if (removed) *removed = entries.size();
        directory.removeRecursively();
    }
    if (!QDir().mkpath(path)) return QString();
    QFile::setPermissions(path, QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner);
    qputenv("TMPDIR", QFile::encodeName(path));
    return path;
}

} } // namespace nordtile::player
