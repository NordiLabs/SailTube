// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "BrowserCookieImport.h"

#include "Core/Account/Domain/AccountPolicy.h"
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QSqlDatabase>
#include <QSqlQuery>
#include <QUuid>
#include <QVariant>
#include <atomic>
#include <cerrno>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

namespace nordtile { namespace video { namespace account {

namespace {

std::atomic<int> connectionCounter(0);

// NFR-SEC-03 (core fix, edit outside the account area): the copy is created 0600 with O_EXCL and never
// follows a link; QFile::copy would give it the browser's mode (0644) until a later chmod.
bool copyPrivate(const QString &from, const QString &to) {
    QFile in(from);
    if (!in.open(QIODevice::ReadOnly)) return false;
    const QByteArray target = QFile::encodeName(to);
    const int fd = ::open(target.constData(), O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
    if (fd < 0) return false;
    bool ok = true;
    char buffer[64 * 1024];
    qint64 n;
    while (ok && (n = in.read(buffer, sizeof buffer)) > 0) {
        const char *p = buffer;
        while (n > 0) {
            const ssize_t w = ::write(fd, p, static_cast<size_t>(n));
            if (w < 0) { if (errno == EINTR) continue; ok = false; break; }
            p += w; n -= w;
        }
    }
    if (n < 0) ok = false;
    if (::close(fd) != 0) ok = false;
    if (!ok) QFile::remove(to);
    return ok;
}

// Directories of the private tree are created 0700 (not at the umask) and narrowed if they already exist.
bool makePrivateDir(const QString &path) {
    const mode_t previous = ::umask(077);
    const bool ok = QDir().mkpath(path);
    ::umask(previous);
    if (ok) QFile::setPermissions(path, QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner);
    return ok;
}

} // namespace

BrowserCookieImport::BrowserCookieImport(const QString &home, const QString &scratchRoot)
    : home_(home), scratchRoot_(scratchRoot) {}

QString BrowserCookieImport::findDatabase() const {
    for (const std::string &rel : browserCookieDatabases()) {
        const QString path = QDir(home_).filePath(QString::fromStdString(rel));
        if (QFileInfo(path).isFile()) return path;
    }
    return QString();
}

BrowserImportResult BrowserCookieImport::run() const {
    BrowserImportResult result;
    const QString source = findDatabase();
    if (source.isEmpty()) { result.error = BrowserImportError::NoBrowserDatabase; return result; }
    result.databasePath = source;

    // A fresh 0700 directory: the copy keeps the database's base name so SQLite
    // finds its -wal and -shm next to it.
    if (!makePrivateDir(scratchRoot_)) { result.error = BrowserImportError::Unreadable; return result; }
    const QString dir = QDir(scratchRoot_).filePath(QStringLiteral("browser-") + QUuid::createUuid().toString().mid(1, 8));
    if (!makePrivateDir(dir)) { result.error = BrowserImportError::Unreadable; return result; }
    const QString copy = QDir(dir).filePath(QStringLiteral("cookies.sqlite"));
    bool ok = copyPrivate(source, copy);
    for (const char *suffix : { "-wal", "-shm" }) {
        const QString side = source + QLatin1String(suffix);
        if (ok && QFileInfo(side).isFile()) ok = copyPrivate(side, copy + QLatin1String(suffix));
    }

    std::vector<Cookie> rows;
    if (ok) {
        const QString name = QStringLiteral("browser-import-%1").arg(++connectionCounter);
        {
            QSqlDatabase db = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), name);
            db.setDatabaseName(copy);
            // Read-write on the private copy: SQLite needs it to replay the WAL.
            ok = db.open();
            if (ok) {
                QSqlQuery q(db);
                ok = q.exec(QStringLiteral("SELECT host, name, value, path, expiry, isSecure, isHttpOnly FROM moz_cookies "
                                           "ORDER BY id"));
                while (ok && q.next()) {
                    Cookie c;
                    c.domain = q.value(0).toString().toStdString();
                    if (!importableCookieHost(c.domain)) continue;
                    c.name = q.value(1).toString().toStdString();
                    c.value = q.value(2).toString().toStdString();
                    c.path = q.value(3).toString().toStdString();
                    c.expiresSec = q.value(4).toLongLong();
                    c.secure = q.value(5).toInt() != 0;
                    c.httpOnly = q.value(6).toInt() != 0;
                    if (!c.name.empty()) rows.push_back(c);
                }
            }
            db.close();
        }
        QSqlDatabase::removeDatabase(name);
    }
    QDir(dir).removeRecursively();   // the copy never outlives the import
    if (!ok) { result.error = BrowserImportError::Unreadable; return result; }

    result.cookies = mergeImported(rows);
    if (!hasSigningSecret(result.cookies)) {
        result.cookies = SessionCookies();
        result.error = BrowserImportError::NoSession; // "sign in in a normal tab, not a private one"
    }
    return result;
}

}}}
