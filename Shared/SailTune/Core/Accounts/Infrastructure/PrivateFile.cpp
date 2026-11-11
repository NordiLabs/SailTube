// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "PrivateFile.h"
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QSaveFile>
#include <QString>

namespace nordtile { namespace music { namespace accounts { namespace infrastructure {
namespace {
domain::Result fail(const char *message) {
    domain::Result r; r.code = domain::ErrorCode::SecureStorageFailure; r.message = message; return r;
}
QString qpath(const std::string &p) { return QString::fromUtf8(p.c_str()); }
}

FileRead readPrivateFile(const std::string &path, std::size_t maxBytes, std::string *out) {
    if (out) out->clear();
    const QFileInfo info(qpath(path));
    if (info.isSymLink()) return FileRead::Unsafe;
    if (!info.exists()) return FileRead::Absent;
    if (!info.isFile()) return FileRead::Unsafe;
    if (std::size_t(info.size()) > maxBytes) return FileRead::TooLarge;
    QFile file(qpath(path));
    if (!file.open(QIODevice::ReadOnly)) return FileRead::IoError;
    const QByteArray bytes = file.read(qint64(maxBytes) + 1);
    if (std::size_t(bytes.size()) > maxBytes) return FileRead::TooLarge;
    if (out) out->assign(bytes.constData(), std::size_t(bytes.size()));
    return FileRead::Ok;
}

domain::Result writePrivateFile(const std::string &path, const std::string &bytes) {
    const QFileInfo info(qpath(path));
    if (info.isSymLink() || (info.exists() && !info.isFile())) return fail("Account metadata location is not safe.");
    const QString dir = info.absolutePath();
    if (!QDir().mkpath(dir)) return fail("Account metadata directory is unavailable.");
    QFile::setPermissions(dir, QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner);
    QSaveFile file(qpath(path));
    file.setDirectWriteFallback(false);
    if (!file.open(QIODevice::WriteOnly) ||
        !file.setPermissions(QFile::ReadOwner | QFile::WriteOwner) ||
        file.write(bytes.data(), qint64(bytes.size())) != qint64(bytes.size()) || !file.commit())
        return fail("Account metadata could not be saved.");
    return domain::Result();
}

domain::Result removePrivateFile(const std::string &path) {
    const QFileInfo info(qpath(path));
    if (info.isSymLink()) {
        // Remove only the link itself, never what it points to.
        if (!QFile::remove(qpath(path))) return fail("Account metadata could not be removed.");
        return domain::Result();
    }
    if (!info.exists()) return domain::Result();
    if (!info.isFile() || !QFile::remove(qpath(path))) return fail("Account metadata could not be removed.");
    return domain::Result();
}

}}}}
