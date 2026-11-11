// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "GeckoCookieJarReader.h"
#include "../Domain/CookieJarSelection.h"
#include "PrivateScratch.h"
#include <QDir>
#include <QFile>
#include <QSqlDatabase>
#include <QSqlError>
#include <QSqlQuery>
#include <QString>
#include <QStringList>
#include <QVariant>
#include <atomic>

namespace nordtile { namespace music { namespace ytm { namespace sources { namespace infrastructure {

namespace sc = scratch;

namespace {

struct QueryOut {
    domain::ReadStatus status = domain::ReadStatus::NotADatabase;
    int matching = 0;
    std::vector<ports::RawCookieRow> rows;
};

std::string stdFrom(const QVariant &v) {
    if (v.isNull()) return std::string();
    if (v.type() == QVariant::ByteArray) {
        const QByteArray b = v.toByteArray();
        return std::string(b.constData(), static_cast<std::size_t>(b.size()));
    }
    const QByteArray u = v.toString().toUtf8();
    return std::string(u.constData(), static_cast<std::size_t>(u.size()));
}

bool intFrom(const QVariant &v, std::int64_t *out) {
    if (v.isNull()) return false;
    bool ok = false;
    const qlonglong n = v.toLongLong(&ok);
    if (!ok) return false;
    *out = static_cast<std::int64_t>(n);
    return true;
}

domain::ReadStatus statusFor(sc::FileState state) {
    switch (state) {
    case sc::FileState::Absent: return domain::ReadStatus::NotFound;
    case sc::FileState::NotReadable: return domain::ReadStatus::NotReadable;
    case sc::FileState::Unsafe: return domain::ReadStatus::Unsafe;
    case sc::FileState::TooLarge: return domain::ReadStatus::TooLarge;
    case sc::FileState::IoError: return domain::ReadStatus::CopyFailed;
    case sc::FileState::Ok: break;
    }
    return domain::ReadStatus::CopyFailed;
}

// The queries, on an already open connection to the PRIVATE COPY.
domain::ReadStatus runQueries(QSqlDatabase &db, int maxRows, QueryOut *out) {
    {
        QSqlQuery pragma(db);
        pragma.exec(QStringLiteral("PRAGMA query_only = ON"));        // best effort; the copy is ours anyway
        pragma.finish();
    }
    // The columns that exist (a legacy layout may lack some). Column names in the SQL come from a fixed whitelist.
    QStringList present;
    {
        QSqlQuery q(db);
        q.setForwardOnly(true);
        if (!q.exec(QStringLiteral("PRAGMA table_info(moz_cookies)"))) return domain::ReadStatus::NotADatabase;    // also: "file is not a database"
        while (q.next()) present << q.value(1).toString();
        q.finish();
    }
    if (!(present.contains(QStringLiteral("host")) && present.contains(QStringLiteral("name")) && present.contains(QStringLiteral("value"))))
        return domain::ReadStatus::UnexpectedSchema;                  // no moz_cookies table, or not the Gecko layout

    // Whole host names only: "youtube.com" or any "....youtube.com" (so "notyoutube.com" is not YouTube); same for google.com.
    const QString hostFilter = QStringLiteral("(host = ? OR host LIKE ? OR host = ? OR host LIKE ?)");
    const auto bindHosts = [](QSqlQuery &q) {
        q.addBindValue(QStringLiteral("youtube.com")); q.addBindValue(QStringLiteral("%.youtube.com"));
        q.addBindValue(QStringLiteral("google.com")); q.addBindValue(QStringLiteral("%.google.com"));
    };
    {   // how many YouTube/Google rows of ANY name exist: a count only; it separates "no cookies" from "not signed in"
        QSqlQuery q(db);
        q.setForwardOnly(true);
        q.prepare(QStringLiteral("SELECT COUNT(*) FROM moz_cookies WHERE ") + hostFilter);
        bindHosts(q);
        if (!q.exec() || !q.next()) return domain::ReadStatus::NotADatabase;
        bool good = false;
        const int n = q.value(0).toInt(&good);
        out->matching = (good && n > 0) ? n : 0;
        q.finish();
    }
    if (out->matching == 0) return domain::ReadStatus::NoCookies;

    QString placeholders;
    for (std::size_t i = 0; i < domain::CookieJarSelection::orderSize(); ++i) placeholders += (i ? QStringLiteral(",?") : QStringLiteral("?"));
    const QString sql = QStringLiteral("SELECT host, name, value, %1, %2, %3, %4 FROM moz_cookies "
                                       "WHERE %8 AND name IN (%5) "
                                       "AND value IS NOT NULL AND length(value) <= %6 LIMIT %7")
                            .arg(present.contains(QStringLiteral("path")) ? QStringLiteral("path") : QStringLiteral("'/'"))
                            .arg(present.contains(QStringLiteral("expiry")) ? QStringLiteral("expiry") : QStringLiteral("NULL"))
                            .arg(present.contains(QStringLiteral("originAttributes")) ? QStringLiteral("originAttributes") : QStringLiteral("''"))
                            .arg(present.contains(QStringLiteral("lastAccessed")) ? QStringLiteral("lastAccessed") : QStringLiteral("0"))
                            .arg(placeholders)
                            .arg(int(domain::Limits::kMaxValueBytes))
                            .arg(maxRows + 1)
                            .arg(hostFilter);
    QSqlQuery q(db);
    q.setForwardOnly(true);
    q.prepare(sql);
    bindHosts(q);
    for (std::size_t i = 0; i < domain::CookieJarSelection::orderSize(); ++i)
        q.addBindValue(QString::fromLatin1(domain::CookieJarSelection::orderAt(i)));
    if (!q.exec()) return domain::ReadStatus::NotADatabase;
    while (q.next()) {
        if (static_cast<int>(out->rows.size()) >= maxRows) return domain::ReadStatus::TooManyRows;
        ports::RawCookieRow row;
        row.host = stdFrom(q.value(0));
        row.name = stdFrom(q.value(1));
        row.value = stdFrom(q.value(2));
        row.path = stdFrom(q.value(3));
        row.hasExpiry = intFrom(q.value(4), &row.expiry);
        row.originAttributes = stdFrom(q.value(5));
        std::int64_t accessed = 0;
        if (intFrom(q.value(6), &accessed)) row.lastAccessed = accessed;
        out->rows.push_back(row);
    }
    q.finish();
    return domain::ReadStatus::Ok;
}

// Opens the PRIVATE COPY, never the original.
QueryOut queryCopy(const std::string &copyPath, int maxRows) {
    QueryOut out;
    static std::atomic<int> counter(0);
    const QString connection = QStringLiteral("ytm_sources_gecko_%1").arg(++counter);
    {
        QSqlDatabase db = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), connection);
        db.setDatabaseName(QFile::decodeName(copyPath.c_str()));
        out.status = db.open() ? runQueries(db, maxRows, &out) : domain::ReadStatus::NotADatabase;
        db.close();
    }
    QSqlDatabase::removeDatabase(connection);
    if (out.status != domain::ReadStatus::Ok) {
        for (std::size_t i = 0; i < out.rows.size(); ++i) out.rows[i].wipe();
        out.rows.clear();
    }
    return out;
}

}  // namespace

GeckoCookieJarReader::GeckoCookieJarReader(const Config &config) : cfg_(config) {
    if (cfg_.copyAttempts < 1) cfg_.copyAttempts = 1;
    if (cfg_.maxRows < 1) cfg_.maxRows = 1;
    sweepStale();
}

std::string GeckoCookieJarReader::home() const {
    const QString h = cfg_.homeDir.isEmpty() ? QDir::homePath() : cfg_.homeDir;
    if (h.isEmpty()) return std::string();
    return std::string(QFile::encodeName(QDir::cleanPath(h)).constData());
}

std::size_t GeckoCookieJarReader::candidateCount() const {
    return cfg_.candidates.empty() ? static_cast<std::size_t>(domain::kCandidateCount) : cfg_.candidates.size();
}

std::string GeckoCookieJarReader::candidatePath(const std::string &homeDir, int index) const {
    const std::string rel = cfg_.candidates.empty() ? std::string(domain::candidateLocation(index)) : cfg_.candidates[static_cast<std::size_t>(index)];
    return homeDir + "/" + rel;
}

int GeckoCookieJarReader::sweepStale() {
    if (cfg_.scratchRoot.isEmpty()) return 0;
    return sc::sweepStale(std::string(QFile::encodeName(QDir::cleanPath(cfg_.scratchRoot)).constData()));
}

domain::Availability GeckoCookieJarReader::probe() const {
    const std::string h = home();
    if (h.empty()) return domain::Availability::NoHome;
    int worst = 0;      // 0 not found, 1 unsafe, 2 not readable
    for (std::size_t i = 0; i < candidateCount(); ++i) {
        const sc::FileState state = sc::inspect(candidatePath(h, static_cast<int>(i)), cfg_.maxDbBytes, nullptr);
        // TooLarge counts as readable: the file is an ordinary one of ours, read() reports its size honestly.
        if (state == sc::FileState::Ok || state == sc::FileState::TooLarge) return domain::Availability::Available;
        if (state == sc::FileState::NotReadable) worst = 2;
        else if (state == sc::FileState::Unsafe && worst < 1) worst = 1;
    }
    return worst == 2 ? domain::Availability::NotReadable : worst == 1 ? domain::Availability::Unsafe : domain::Availability::NotFound;
}

ports::JarRead GeckoCookieJarReader::read() {
    ports::JarRead out;
    const std::string h = home();
    if (h.empty() || cfg_.scratchRoot.isEmpty()) { out.status = h.empty() ? domain::ReadStatus::NotFound : domain::ReadStatus::CopyFailed; return out; }
    sweepStale();

    bool sawEmpty = false, haveFailure = false;
    domain::ReadStatus failure = domain::ReadStatus::NotFound;
    for (std::size_t i = 0; i < candidateCount(); ++i) {
        const std::string path = candidatePath(h, static_cast<int>(i));
        const sc::FileState state = sc::inspect(path, cfg_.maxDbBytes, nullptr);
        if (state == sc::FileState::Absent) continue;
        domain::ReadStatus status;
        ports::JarRead one;
        if (state == sc::FileState::Ok) {
            one = readCandidate(path, static_cast<int>(i));
            status = one.status;
            if (status == domain::ReadStatus::Ok) return one;
        } else status = statusFor(state);
        if (status == domain::ReadStatus::NoCookies) { sawEmpty = true; continue; }
        if (!haveFailure) { haveFailure = true; failure = status; }
    }
    out.status = haveFailure ? failure : (sawEmpty ? domain::ReadStatus::NoCookies : domain::ReadStatus::NotFound);
    return out;
}

ports::JarRead GeckoCookieJarReader::readCandidate(const std::string &path, int index) {
    ports::JarRead out;
    out.candidate = index;
    const std::string root(QFile::encodeName(QDir::cleanPath(cfg_.scratchRoot)).constData());
    const std::string walPath = path + "-wal";

    for (int attempt = 0; attempt < cfg_.copyAttempts; ++attempt) {
        sc::Stamp dbBefore, walBefore;
        const sc::FileState dbState = sc::inspect(path, cfg_.maxDbBytes, &dbBefore);
        const sc::FileState walState = sc::inspect(walPath, cfg_.maxWalBytes, &walBefore);
        if (dbState != sc::FileState::Ok) { out.status = dbState == sc::FileState::Absent ? domain::ReadStatus::ChangedWhileReading : statusFor(dbState); return out; }
        if (walState != sc::FileState::Ok && walState != sc::FileState::Absent) { out.status = statusFor(walState); return out; }

        std::string dir;
        if (!sc::makeScratchDirectory(root, &dir)) { out.status = domain::ReadStatus::CopyFailed; return out; }
        sc::ScratchGuard guard(dir);        // removed on every path out of this scope
        const std::string copy = dir + "/cookies-copy.sqlite";

        sc::FileState c = sc::copyPrivate(path, copy, cfg_.maxDbBytes);
        if (c == sc::FileState::Absent) continue;               // vanished between the check and the copy: look again
        if (c != sc::FileState::Ok) { out.status = statusFor(c); return out; }
        if (walState == sc::FileState::Ok) {
            c = sc::copyPrivate(walPath, copy + "-wal", cfg_.maxWalBytes);
            if (c == sc::FileState::Absent) continue;
            if (c != sc::FileState::Ok) { out.status = statusFor(c); return out; }
        }

        if (cfg_.inspectScratch) cfg_.inspectScratch(dir, Stage::AfterCopy);
        sc::Stamp dbAfter, walAfter;
        sc::inspect(path, cfg_.maxDbBytes, &dbAfter);
        sc::inspect(walPath, cfg_.maxWalBytes, &walAfter);
        if (dbBefore != dbAfter || walBefore != walAfter) continue;      // the browser wrote in between: copy again

        QueryOut q = queryCopy(copy, cfg_.maxRows);
        if (cfg_.inspectScratch) cfg_.inspectScratch(dir, Stage::BeforeCleanup);
        guard.release();

        out.status = q.status;
        out.matchingRows = q.matching;
        out.rows.swap(q.rows);
        return out;
    }
    out.status = domain::ReadStatus::ChangedWhileReading;
    return out;
}

}}}}}
