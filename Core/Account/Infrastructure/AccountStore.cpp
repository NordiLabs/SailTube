// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "AccountStore.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMutexLocker>
#include <QUuid>

#include <cerrno>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

namespace nordtile { namespace video { namespace account {

namespace {

const int formatVersion = 1;
const qint64 maxSessionBytes = 1024 * 1024;
const QFileDevice::Permissions fileMode = QFileDevice::ReadOwner | QFileDevice::WriteOwner;
const QFileDevice::Permissions dirMode = QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner;

QString str(const std::string &s) { return QString::fromUtf8(s.data(), int(s.size())); }
std::string stdStr(const QString &s) { const QByteArray u = s.toUtf8(); return std::string(u.constData(), size_t(u.size())); }

// Compact JSON: {"v":1,"cookies":[{"n","v","d","p","e","s","h"}...],"sel":{"u","p"}}.
QByteArray encode(const SessionCookies &cookies, const AccountSelection &sel) {
    QJsonArray list;
    for (std::size_t i = 0; i < cookies.cookies.size(); ++i) {
        const Cookie &c = cookies.cookies[i];
        QJsonObject o;
        o.insert(QStringLiteral("n"), str(c.name));
        o.insert(QStringLiteral("v"), str(c.value));
        o.insert(QStringLiteral("d"), str(c.domain));
        o.insert(QStringLiteral("p"), str(c.path));
        o.insert(QStringLiteral("e"), QString::number(c.expiresSec));
        o.insert(QStringLiteral("s"), c.secure);
        o.insert(QStringLiteral("h"), c.httpOnly);
        list.append(o);
    }
    QJsonObject s;
    s.insert(QStringLiteral("u"), sel.authUser);
    s.insert(QStringLiteral("p"), str(sel.pageId));
    QJsonObject root;
    root.insert(QStringLiteral("v"), formatVersion);
    root.insert(QStringLiteral("cookies"), list);
    root.insert(QStringLiteral("sel"), s);
    return QJsonDocument(root).toJson(QJsonDocument::Compact);
}

bool decode(const QByteArray &blob, SessionCookies *cookies, AccountSelection *sel) {
    QJsonParseError err;
    const QJsonDocument doc = QJsonDocument::fromJson(blob, &err);
    if (err.error != QJsonParseError::NoError || !doc.isObject()) return false;
    const QJsonObject root = doc.object();
    if (root.value(QStringLiteral("v")).toInt(-1) != formatVersion) return false;
    const QJsonValue list = root.value(QStringLiteral("cookies"));
    if (!list.isArray()) return false;
    SessionCookies out;
    const QJsonArray a = list.toArray();
    for (int i = 0; i < a.size(); ++i) {
        if (!a.at(i).isObject()) return false;
        const QJsonObject o = a.at(i).toObject();
        Cookie c;
        c.name = stdStr(o.value(QStringLiteral("n")).toString());
        c.value = stdStr(o.value(QStringLiteral("v")).toString());
        c.domain = stdStr(o.value(QStringLiteral("d")).toString());
        c.path = stdStr(o.value(QStringLiteral("p")).toString(QStringLiteral("/")));
        c.expiresSec = o.value(QStringLiteral("e")).toString().toLongLong();
        c.secure = o.value(QStringLiteral("s")).toBool(true);
        c.httpOnly = o.value(QStringLiteral("h")).toBool(false);
        if (c.name.empty()) return false;
        out.cookies.push_back(c);
    }
    if (out.cookies.empty()) return false; // never a valid stored session
    const QJsonObject s = root.value(QStringLiteral("sel")).toObject();
    AccountSelection selection;
    selection.authUser = s.value(QStringLiteral("u")).toInt(0);
    selection.pageId = stdStr(s.value(QStringLiteral("p")).toString());
    if (selection.authUser < 0 || selection.authUser > limits::maxAuthUserProbe) selection = AccountSelection();
    *cookies = out;
    *sel = selection;
    return true;
}

// Creates `path` (and missing parents) and forces it to 0700.
bool ensurePrivateDir(const QString &path) {
    const QFileInfo info(path);
    if (!info.exists()) {
        if (!QDir().mkpath(info.absolutePath())) return false;
        if (::mkdir(QFile::encodeName(path).constData(), 0700) != 0 && !QFileInfo(path).isDir()) return false;
    }
    if (!QFileInfo(path).isDir()) return false;
    return QFile::setPermissions(path, dirMode);
}

// Atomic write: a fresh temp "<name>.tmp-XXXXXXXX" in the same directory, created 0600 with
// O_EXCL (never through a link, never wider than 0600 for even a moment), synced, then renamed
// over `path`. NFR-SEC-03 / ACC-10.
bool writePrivateFile(const QString &path, const QByteArray &data) {
    QString temp;
    int fd = -1;
    for (int attempt = 0; attempt < 8 && fd < 0; ++attempt) {
        temp = path + QStringLiteral(".tmp-") + QUuid::createUuid().toString().mid(1, 8);
        do {
            fd = ::open(QFile::encodeName(temp).constData(), O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
        } while (fd < 0 && errno == EINTR);
        if (fd < 0 && errno != EEXIST) return false;
    }
    if (fd < 0) return false;
    bool ok = ::fchmod(fd, 0600) == 0;   // the umask may only narrow it; make it exact
    struct stat st;
    ok = ok && ::fstat(fd, &st) == 0 && S_ISREG(st.st_mode) && (st.st_mode & 0777) == 0600;
    const char *p = data.constData();
    qint64 left = data.size();
    while (ok && left > 0) {
        const ssize_t w = ::write(fd, p, static_cast<size_t>(left));
        if (w < 0) { if (errno == EINTR) continue; ok = false; break; }
        p += w; left -= w;
    }
    ok = ok && ::fsync(fd) == 0;
    if (::close(fd) != 0) ok = false;
    ok = ok && ::rename(QFile::encodeName(temp).constData(), QFile::encodeName(path).constData()) == 0;
    if (!ok) ::unlink(QFile::encodeName(temp).constData());
    return ok;
}

// writePrivateFile leaves "<name>.tmp-XXXXXXXX" behind only if the process dies mid-write.
void removeLeftoverTemps(const QString &dir, const QString &name) {
    QDir d(dir);
    const QStringList left = d.entryList(QStringList() << name + QStringLiteral(".*"), QDir::Files | QDir::Hidden);
    for (int i = 0; i < left.size(); ++i) QFile::remove(d.filePath(left.at(i)));
}

bool jarSafe(const std::string &s) {
    return s.find_first_of("\t\r\n") == std::string::npos;
}

QByteArray netscapeJar(const SessionCookies &cookies) {
    QByteArray out("# Netscape HTTP Cookie File\n");
    for (std::size_t i = 0; i < cookies.cookies.size(); ++i) {
        const Cookie &c = cookies.cookies[i];
        if (c.name.empty() || c.domain.empty() || !jarSafe(c.name) || !jarSafe(c.value) || !jarSafe(c.domain)
            || !jarSafe(c.path)) continue;
        const bool subdomains = c.domain[0] == '.';
        out += QByteArray(c.domain.data(), int(c.domain.size()));
        out += subdomains ? "\tTRUE\t" : "\tFALSE\t";
        out += c.path.empty() ? QByteArray("/") : QByteArray(c.path.data(), int(c.path.size()));
        out += c.secure ? "\tTRUE\t" : "\tFALSE\t";
        out += QByteArray::number(qint64(c.expiresSec));
        out += '\t';
        out += QByteArray(c.name.data(), int(c.name.size()));
        out += '\t';
        out += QByteArray(c.value.data(), int(c.value.size()));
        out += '\n';
    }
    return out;
}

const char sessionFileName[] = "session.json";
const char disclosureFileName[] = "disclosure-shown";
const char jarsDirName[] = "jars";
const char jarFileName[] = "cookies.txt";

} // namespace

AccountStore::AccountStore(const AccountStoreOptions &options) : options_(options) {
    fileFallback_ = !vaultUsable() && options_.variant == BuildVariant::Full;
}

bool AccountStore::vaultUsable() const {
    return options_.vault && options_.vault->available();
}

QString AccountStore::sessionFilePath() const {
    return QDir(options_.privateDir).filePath(QString::fromLatin1(sessionFileName));
}

QString AccountStore::disclosureFilePath() const {
    return QDir(options_.privateDir).filePath(QString::fromLatin1(disclosureFileName));
}

QString AccountStore::jarsRootPath() const {
    return QDir(options_.privateDir).filePath(QString::fromLatin1(jarsDirName));
}

bool AccountStore::open() {
    QMutexLocker lock(&mutex_);
    session_ = SessionCookies();
    selection_ = AccountSelection();
    const bool vault = vaultUsable();
    fileFallback_ = !vault && options_.variant == BuildVariant::Full;

    SessionCookies cookies;
    AccountSelection sel;
    bool ok = false;
    QByteArray blob;
    if (vault && options_.vault->load(&blob)) ok = decode(blob, &cookies, &sel);
    if (!ok && options_.variant == BuildVariant::Full) {
        QFile f(sessionFilePath());
        if (f.exists()) {
            if (f.size() <= maxSessionBytes && f.open(QIODevice::ReadOnly)) {
                blob = f.readAll();
                f.close();
                ok = decode(blob, &cookies, &sel);
            }
            if (!ok) {
                // Unusable and possibly secret-bearing: do not keep it around.
                qWarning("account: stored session file is unreadable, signed out");
                QFile::remove(sessionFilePath());
            } else if (vault && options_.vault->store(blob)) {
                // The vault came back: move the session out of the file.
                QFile::remove(sessionFilePath());
            }
        }
    }
    if (!ok) return false;
    session_ = cookies;
    selection_ = sel;
    return true;
}

bool AccountStore::signedIn() const {
    QMutexLocker lock(&mutex_);
    return !session_.empty();
}

SessionCookies AccountStore::session() const {
    QMutexLocker lock(&mutex_);
    return session_;
}

AccountSelection AccountStore::selection() const {
    QMutexLocker lock(&mutex_);
    return selection_;
}

std::uint64_t AccountStore::generation() const {
    QMutexLocker lock(&mutex_);
    return generation_;
}

bool AccountStore::persistLocked(const SessionCookies &cookies, const AccountSelection &selection) {
    const QByteArray blob = encode(cookies, selection);
    if (vaultUsable()) {
        fileFallback_ = false;
        if (!options_.vault->store(blob)) {
            qWarning("account: the secrets vault refused the session");
            return false;
        }
        if (QFile::exists(sessionFilePath())) QFile::remove(sessionFilePath());
        return true;
    }
    if (options_.variant != BuildVariant::Full) return false; // STORE: fail closed
    fileFallback_ = true;
    if (!ensurePrivateDir(options_.privateDir) || !writePrivateFile(sessionFilePath(), blob)) {
        qWarning("account: cannot write the session file in %s", qPrintable(options_.privateDir));
        return false;
    }
    return true;
}

ImportRefusal AccountStore::importSession(const SessionCookies &cookies) {
    QMutexLocker lock(&mutex_);
    if (!hasSigningSecret(cookies)) return ImportRefusal::NoSigningSecret;
    if (options_.variant == BuildVariant::Store && !vaultUsable()) return ImportRefusal::NoSecureStorage;
    SessionCookies kept;
    for (std::size_t i = 0; i < cookies.cookies.size(); ++i) {
        if (!cookies.cookies[i].name.empty()) kept.cookies.push_back(cookies.cookies[i]);
    }
    const AccountSelection fresh;
    if (!persistLocked(kept, fresh)) return ImportRefusal::WriteFailed;
    session_ = kept;
    selection_ = fresh;
    ++generation_;
    return ImportRefusal::None;
}

bool AccountStore::setSelection(const AccountSelection &selection) {
    QMutexLocker lock(&mutex_);
    if (session_.empty()) return false;
    if (selection.authUser < 0 || selection.authUser > limits::maxAuthUserProbe) return false;
    if (selection == selection_) return true;
    if (!persistLocked(session_, selection)) return false;
    selection_ = selection;
    return true;
}

bool AccountStore::applyRotation(const std::vector<SetCookieValue> &rotated, std::uint64_t generation) {
    QMutexLocker lock(&mutex_);
    if (generation != generation_ || session_.empty() || rotated.empty()) return false;
    SessionCookies next = session_;
    if (!foldRotation(&next, rotated)) return false;
    // Memory follows the server even if the write fails: the old values may
    // already be invalid there.
    session_ = next;
    persistLocked(session_, selection_);
    return true;
}

void AccountStore::signOut() {
    QMutexLocker lock(&mutex_);
    if (options_.vault) options_.vault->erase();
    QFile::remove(sessionFilePath());
    removeLeftoverTemps(options_.privateDir, QString::fromLatin1(sessionFileName));
    QDir(jarsRootPath()).removeRecursively();
    session_ = SessionCookies();
    selection_ = AccountSelection();
    ++generation_;
}

bool AccountStore::disclosurePending() const {
    QMutexLocker lock(&mutex_);
    return fileFallback_ && !session_.empty() && !QFile::exists(disclosureFilePath());
}

void AccountStore::markDisclosed() {
    QMutexLocker lock(&mutex_);
    if (!fileFallback_ || QFile::exists(disclosureFilePath())) return;
    if (!ensurePrivateDir(options_.privateDir) || !writePrivateFile(disclosureFilePath(), QByteArray("1\n")))
        qWarning("account: cannot record the storage disclosure");
}

bool AccountStore::usingFileFallback() const {
    QMutexLocker lock(&mutex_);
    return fileFallback_;
}

QString AccountStore::writeExtractorJar() const {
    QMutexLocker lock(&mutex_);
    if (session_.empty()) return QString();
    const QString root = jarsRootPath();
    if (!ensurePrivateDir(options_.privateDir) || !ensurePrivateDir(root)) return QString();
    QString dir;
    for (int attempt = 0; attempt < 8 && dir.isEmpty(); ++attempt) {
        const QString name = QUuid::createUuid().toString().mid(1, 36).remove(QLatin1Char('-'));
        const QString candidate = QDir(root).filePath(name);
        // mkdir fails if it exists, so the directory is always fresh.
        if (::mkdir(QFile::encodeName(candidate).constData(), 0700) == 0) dir = candidate;
    }
    if (dir.isEmpty() || !QFile::setPermissions(dir, dirMode)) {
        if (!dir.isEmpty()) QDir().rmdir(dir);
        return QString();
    }
    const QString path = QDir(dir).filePath(QString::fromLatin1(jarFileName));
    if (!writePrivateFile(path, netscapeJar(session_))) {
        removeLeftoverTemps(dir, QString::fromLatin1(jarFileName));
        QDir().rmdir(dir);
        qWarning("account: cannot write the extractor cookie jar");
        return QString();
    }
    return path;
}

void AccountStore::removeExtractorJar(const QString &path) {
    if (path.isEmpty()) return;
    const QFileInfo info(path);
    if (info.fileName() != QLatin1String(jarFileName)) return;
    QFile::remove(path);
    const QString dir = info.absolutePath();
    removeLeftoverTemps(dir, QString::fromLatin1(jarFileName));
    QDir().rmdir(dir); // only when empty
}

}}}
