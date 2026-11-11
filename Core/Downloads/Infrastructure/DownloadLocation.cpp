// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "DownloadLocation.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QStandardPaths>
#include <QUuid>

namespace nordtile { namespace video { namespace downloads {

DownloadLocation::DownloadLocation(const LocationRoots &roots) : roots_(roots) {}

LocationRoots DownloadLocation::defaultRoots() {
    LocationRoots r;
    r.appData = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
    const QStandardPaths::StandardLocation user[] = { QStandardPaths::MoviesLocation, QStandardPaths::MusicLocation,
                                                      QStandardPaths::DownloadLocation };
    for (QStandardPaths::StandardLocation l : user) {
        const QString p = QStandardPaths::writableLocation(l);
        if (!p.isEmpty() && !r.userFolders.contains(p)) r.userFolders << p;
    }
    QString user0 = QString::fromLocal8Bit(qgetenv("USER"));
    if (user0.isEmpty()) user0 = QDir::home().dirName();
    r.removableRoot = QStringLiteral("/run/media/") + user0;
    return r;
}

QString DownloadLocation::defaultDirectory() const {
    return QDir::cleanPath(roots_.appData + QStringLiteral("/downloads"));
}

bool DownloadLocation::ensureDefaultDirectory() const {
    const QString dir = defaultDirectory();
    if (!QDir().mkpath(dir)) return false;
    QFile::setPermissions(dir, QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner);
    return true;
}

QStringList DownloadLocation::candidateFolders() const {
    QStringList out;
    for (const QString &p : roots_.userFolders)
        if (QFileInfo(p).isDir()) out << QDir::cleanPath(p);
    if (!roots_.removableRoot.isEmpty()) {
        QDir media(roots_.removableRoot);
        for (const QFileInfo &mount : media.entryInfoList(QDir::Dirs | QDir::NoDotAndDotDot, QDir::Name))
            out << QDir::cleanPath(mount.absoluteFilePath());
    }
    return out;
}

bool DownloadLocation::writable(const QString &dir) {
    if (dir.isEmpty() || !QFileInfo(dir).isDir()) return false;
    const QString probe = dir + QStringLiteral("/.sailtube-probe-") + QUuid::createUuid().toString().mid(1, 8);
    QFile f(probe);
    if (f.exists() || !f.open(QIODevice::WriteOnly)) return false;
    const bool wrote = f.write("x", 1) == 1;
    f.close();
    const bool removed = QFile::remove(probe);
    return wrote && removed;
}

FolderCheck DownloadLocation::checkChosen(const QString &path) const {
    FolderCheck c;
    const QString clean = path.isEmpty() ? QString() : QDir::cleanPath(path);
    QFileInfo info(clean);
    // Symlinks into /tmp count as /tmp.
    const QString canonical = info.exists() ? info.canonicalFilePath() : clean;
    const bool exists = info.isDir();
    std::string key = validateChosenFolder(path.toStdString(), exists, exists && writable(clean));
    if (key.empty() && isTemporaryPath(canonical.toStdString())) key = "downloads_error_folder_temporary";
    if (!roots_.refuseTemporary && key == "downloads_error_folder_temporary")
        key = validateChosenFolder("/x", exists, exists && writable(clean));
    if (!key.empty()) {
        c.errorKey = QString::fromStdString(key);
        return c;
    }
    c.ok = true;
    c.path = clean;
    return c;
}

LocationDecision DownloadLocation::resolve(const QString &preferred) const {
    LocationInput in;
    in.defaultDirectory = defaultDirectory().toStdString();
    in.preferred = preferred.toStdString();
    in.preferredUsable = !preferred.isEmpty() && checkChosen(preferred).ok;
    LocationDecision d = chooseLocation(in);
    if (d.directory == in.defaultDirectory) ensureDefaultDirectory();
    return d;
}

}}}
