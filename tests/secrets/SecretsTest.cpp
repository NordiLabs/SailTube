// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// NFR-SEC-03 / ACC-10: secrets never reach logs, diagnostics, backups or a
// file outside the private directory, and secret-bearing temp files are 0600
// and gone straight after use.
//
// The account runs as in the app (AccountStore + AccountService +
// AccountController + the pinned paste parser + YtDlpRunner with the cookie
// hooks wired as in main.cpp) against a fake transport and a fake extractor
// (a shell script standing in for yt-dlp). Every cookie value is synthetic and
// unique to this run; every value that enters the account, every rotated value
// and every SAPISIDHASH the service computes is collected as a secret.
//
//  * Steps: refused paste, paste sign-in (FULL file fallback, no vault),
//    verification with a Set-Cookie rotation, account switch, a signed browse
//    with a rotation, extractor calls (success with a jar rotation, failure,
//    timeout, cancel, three concurrent, runner destroyed mid-call, direct
//    begin/end with a damaged or vanished jar), a backup export, a diagnostics
//    report, sign-out; then the browser import (vault variant) and sign-out.
//  * After each step a snapshot of the whole test root (listing, modes, and the
//    content of every file) must show: secrets only inside the private dir, at
//    0600, in 0700 directories, only in the session file or a jar whose call is
//    still open; nothing secret in TMPDIR; nothing at all after sign-out.
//  * All qDebug/qInfo/qWarning/qCritical output (qInstallMessageHandler), every
//    controller message, every extractor result, the diagnostics report and the
//    backup are scanned for every secret; none may appear. The jar path and
//    "--cookies" must not be logged either.
//  * Linux: with tests/secrets/fs_trace.so preloaded (SEC_TRACE), the file
//    system calls of this process are checked as well, so a file that holds a
//    secret for a moment only is seen: no bytes written to a file under the
//    private dir or TMPDIR while its mode grants group/other access, no chmod
//    of a non-empty file there to such a mode, no file created in TMPDIR or
//    elsewhere outside the private dir (test-owned inputs aside), no rename out
//    of the private dir.
#include "Clients/Sailfish/Presentation/AccountController.h"
#include "Core/Account/Domain/AccountPolicy.h"
#include "Core/Account/Infrastructure/AccountService.h"
#include "Core/Account/Infrastructure/AccountStore.h"
#include "Core/Account/Infrastructure/PinnedSessionParser.h"
#include "Core/Diagnostics/Domain/DiagnosticsReport.h"
#include "Core/Library/Infrastructure/SailTubeBackup.h"
#include "Core/Resolver/Infrastructure/YtDlpRunner.h"

#include <QCoreApplication>
#include <QDir>
#include <QDirIterator>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMutex>
#include <QMutexLocker>
#include <QPointer>
#include <QSet>
#include <QSettings>
#include <QSqlDatabase>
#include <QSqlQuery>
#include <QTimer>
#include <QUuid>

#include <cstdio>
#include <functional>
#include <set>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

using namespace nordtile::video;
using namespace nordtile::video::account;

static int failures = 0;
static QSet<QString> reported;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); ++failures; } } while (0)

// One line per distinct finding (a leftover reported after every step is reported once).
static void fail(const QString &what) {
    if (reported.contains(what)) return;
    reported.insert(what);
    std::printf("FAIL %s\n", qPrintable(what));
    ++failures;
}

static void spin(int ms) { QEventLoop l; QTimer::singleShot(ms, &l, SLOT(quit())); l.exec(); }
static bool waitFor(const std::function<bool()> &f, int ms = 5000) {
    QElapsedTimer t; t.start();
    while (!f() && t.elapsed() < ms) spin(5);
    return f();
}

// ---------------------------------------------------------------- secrets and logs

static std::set<std::string> secrets;
static void secret(const std::string &v) { if (v.size() >= 6) secrets.insert(v); }
static std::string synthetic(const char *name) {
    // Never a real value: a fixed marker, the cookie name and a fresh UUID.
    return std::string("synthetic-") + name + "-" + QUuid::createUuid().toString().mid(1, 36).toStdString();
}

static QMutex logMutex;
static QStringList logs;
static void captureMessages(QtMsgType, const QMessageLogContext &, const QString &message) {
    QMutexLocker lock(&logMutex);
    logs << message;
}
static QString allLogs() { QMutexLocker lock(&logMutex); return logs.join(QLatin1Char('\n')); }

// Every secret that occurs in `text` (by index, never the value).
static void scan(const QString &where, const QByteArray &text) {
    int i = 0;
    for (const std::string &s : secrets) {
        if (text.contains(QByteArray(s.data(), int(s.size()))))
            fail(QStringLiteral("%1 contains secret #%2 (%3...)").arg(where).arg(i)
                     .arg(QString::fromStdString(s.substr(0, s.find('-', 10)))));
        ++i;
    }
}

// ---------------------------------------------------------------- layout and snapshots

struct Layout {
    QString root;      // everything the test owns
    QString data;      // AppDataLocation stand-in
    QString priv;      // the account private dir (data/account)
    QString tmp;       // TMPDIR of the process
    QString home;      // fake HOME with the browser database (user-owned source, not scanned)
    QString inputs;    // files the user picked (not scanned)
    QString bin;       // the fake extractor
    QString config;    // the settings INI
    QString backup;    // backup target directory
};
static Layout L;

static QString rel(const QString &abs) { return abs.startsWith(L.root) ? abs.mid(L.root.size() + 1) : abs; }
static bool under(const QString &path, const QString &dir) { return path == dir || path.startsWith(dir + QLatin1Char('/')); }
static bool userOwned(const QString &path) {
    return under(path, L.home) || under(path, L.inputs) || under(path, L.bin) || under(path, L.config);
}
static unsigned modeOf(const QString &path) {
    struct stat st;
    if (::lstat(QFile::encodeName(path).constData(), &st) != 0) return 0;
    return st.st_mode & 07777;
}
static QString octal(unsigned m) { return QString::number(m, 8).rightJustified(4, QLatin1Char('0')); }

static QStringList openJars;   // jars whose extractor call has not ended yet
static bool expectSessionFile = false;

static void snapshot(const QString &step) {
    QStringList listing;
    if (modeOf(L.priv) && modeOf(L.priv) != 0700)
        fail(QStringLiteral("[%1] private dir %2 has mode %3, not 0700").arg(step, rel(L.priv), octal(modeOf(L.priv))));
    QDirIterator it(L.root, QDir::AllEntries | QDir::Hidden | QDir::System | QDir::NoDotAndDotDot, QDirIterator::Subdirectories);
    while (it.hasNext()) {
        const QString path = it.next();
        if (userOwned(path)) continue;
        const QFileInfo info(path);
        const unsigned mode = modeOf(path);
        if (info.isDir() && !info.isSymLink()) {
            if (under(path, L.priv) && path != L.priv && mode != 0700)
                fail(QStringLiteral("[%1] directory %2 in the private dir has mode %3, not 0700").arg(step, rel(path), octal(mode)));
            continue;
        }
        QFile f(path);
        QByteArray content;
        if (info.isFile() && !info.isSymLink() && info.size() <= 16 * 1024 * 1024 && f.open(QIODevice::ReadOnly)) content = f.readAll();
        bool holds = false;
        for (const std::string &s : secrets)
            if (content.contains(QByteArray(s.data(), int(s.size())))) { holds = true; break; }
        if (!holds) continue;
        listing << rel(path);
        if (!under(path, L.priv)) {
            fail(QStringLiteral("[%1] %2 holds a secret outside the private dir").arg(step, rel(path)));
            continue;
        }
        if (mode != 0600) fail(QStringLiteral("[%1] %2 holds a secret at mode %3, not 0600").arg(step, rel(path), octal(mode)));
        const bool sessionFile = path == L.priv + QStringLiteral("/session.json");
        if (sessionFile && !expectSessionFile) fail(QStringLiteral("[%1] the session file is still present").arg(step));
        if (!sessionFile && !openJars.contains(path))
            fail(QStringLiteral("[%1] %2 holds a secret but is no open extractor jar (left behind?)").arg(step, rel(path)));
    }
    // TMPDIR is never used for account material; the runner's per-call directories go away with the call.
    QDirIterator t(L.tmp, QDir::AllEntries | QDir::Hidden | QDir::System | QDir::NoDotAndDotDot);
    while (t.hasNext()) {
        const QString path = t.next();
        if (QFileInfo(path).isFile()) fail(QStringLiteral("[%1] file %2 in TMPDIR").arg(step, rel(path)));
    }
    std::printf("  %-34s secret-bearing files: %s\n", qPrintable(step),
                listing.isEmpty() ? "none" : qPrintable(listing.join(QStringLiteral(", "))));
}

static void noSecretAnywhere(const QString &step) {
    QDirIterator it(L.root, QDir::Files | QDir::Hidden | QDir::System | QDir::NoDotAndDotDot, QDirIterator::Subdirectories);
    while (it.hasNext()) {
        const QString path = it.next();
        if (userOwned(path)) continue;
        QFile f(path);
        if (!f.open(QIODevice::ReadOnly)) continue;
        scan(QStringLiteral("[%1] %2").arg(step, rel(path)), f.readAll());
    }
}

static int jarDirs() {
    return QDir(L.priv + QStringLiteral("/jars")).entryList(QDir::Dirs | QDir::NoDotAndDotDot | QDir::Hidden).size();
}

// ---------------------------------------------------------------- fakes

static QJsonObject runs(const QString &t) { return QJsonObject{{QStringLiteral("runs"), QJsonArray{QJsonObject{{QStringLiteral("text"), t}}}}}; }
static QJsonObject accountMenu() {
    QJsonObject header{{QStringLiteral("accountName"), runs(QStringLiteral("Synthetic User"))}};
    QJsonObject popup{{QStringLiteral("header"), QJsonObject{{QStringLiteral("activeAccountHeaderRenderer"), header}}}};
    return QJsonObject{{QStringLiteral("actions"), QJsonArray{QJsonObject{{QStringLiteral("openPopupAction"),
        QJsonObject{{QStringLiteral("popup"), QJsonObject{{QStringLiteral("multiPageMenuRenderer"), popup}}}}}}}}};
}
static QJsonObject accountItem(const QString &name, const QString &gaia, const QString &pageId) {
    QJsonArray tokens;
    tokens.append(QJsonObject{{QStringLiteral("accountStateToken"), QJsonObject{{QStringLiteral("obfuscatedGaiaId"), gaia}}}});
    if (!pageId.isEmpty()) tokens.append(QJsonObject{{QStringLiteral("pageIdToken"), QJsonObject{{QStringLiteral("pageId"), pageId}}}});
    QJsonObject item{{QStringLiteral("accountName"), QJsonObject{{QStringLiteral("simpleText"), name}}},
                     {QStringLiteral("hasChannel"), true},
                     {QStringLiteral("serviceEndpoint"), QJsonObject{{QStringLiteral("selectActiveIdentityEndpoint"),
                         QJsonObject{{QStringLiteral("supportedTokens"), tokens}}}}}};
    return QJsonObject{{QStringLiteral("accountItem"), item}};
}
static QJsonObject accountsList(bool loggedOut) {
    QJsonArray items;
    if (!loggedOut) {
        items.append(accountItem(QStringLiteral("Synthetic User"), QStringLiteral("gaia-synthetic"), QString()));
        items.append(accountItem(QStringLiteral("Synthetic Brand"), QStringLiteral("gaia-synthetic"), QStringLiteral("page-synthetic")));
    }
    QJsonObject ctx{{QStringLiteral("mainAppWebResponseContext"), QJsonObject{{QStringLiteral("loggedOut"), loggedOut}}}};
    QJsonObject section{{QStringLiteral("accountSectionListRenderer"), QJsonObject{{QStringLiteral("contents"), items}}}};
    return QJsonObject{{QStringLiteral("responseContext"), ctx}, {QStringLiteral("actions"), QJsonArray{section}}};
}

static std::string rotatedMenu, rotatedBrowse, rotatedJar;

// Answers in the documented shapes; collects every credential the service sends as a secret.
class FakeTransport final : public QObject, public AuthTransport {
public:
    int sent = 0;
    void send(const AuthRequest &request, QObject *context, Done done) override {
        ++sent;
        QByteArray authUser;
        for (const auto &h : request.headers) {
            const QByteArray n = h.first.toLower();
            if (n == "authorization") {
                for (const QByteArray &part : h.second.split(' ')) {
                    const int us = part.indexOf('_');
                    if (us > 0) secret(part.mid(us + 1).toStdString());    // the SAPISIDHASH digest
                }
            }
            if (n == "x-goog-authuser") authUser = h.second;
        }
        const QString endpoint = request.url.path().mid(QStringLiteral("/youtubei/v1/").size());
        AuthReply r;
        r.status = 200;
        if (endpoint == QLatin1String("account/account_menu")) {
            r.body = QJsonDocument(accountMenu()).toJson(QJsonDocument::Compact);
            r.setCookies.push_back(QByteArray("SIDCC=") + QByteArray::fromStdString(rotatedMenu) + "; Domain=.youtube.com; Path=/; Secure");
        } else if (endpoint == QLatin1String("account/accounts_list")) {
            r.body = QJsonDocument(accountsList(authUser != "0")).toJson(QJsonDocument::Compact);
        } else {
            r.body = "{\"contents\":{}}";
            r.setCookies.push_back(QByteArray("__Secure-3PSIDCC=") + QByteArray::fromStdString(rotatedBrowse) + "; Domain=.youtube.com; Path=/; Secure; HttpOnly");
        }
        QPointer<QObject> guard(context);
        QTimer::singleShot(1, this, [guard, done, r] { if (guard) done(r); });
    }
};

class FakeVault final : public SecretVault {
public:
    bool available() const override { return true; }
    bool store(const QByteArray &s) override { blob = s; return true; }
    bool load(QByteArray *s) override { if (blob.isEmpty()) return false; *s = blob; return true; }
    bool erase() override { blob.clear(); return true; }
    QByteArray blob;
};

// The yt-dlp stand-in: reports the jar's mode and location, rewrites one value in
// place (as the extractor does when the server rotates a cookie), fails, or hangs.
static QString writeFakeExtractor() {
    const QString path = L.bin + QStringLiteral("/fake-extractor");
    QFile f(path);
    f.open(QIODevice::WriteOnly | QIODevice::Truncate);
    f.write("#!/bin/sh\n"
            "# Synthetic yt-dlp stand-in for tests/secrets (no network, no real cookies).\n"
            "jar=''; prev=''; mode=ok\n"
            "for a in \"$@\"; do\n"
            "  [ \"$prev\" = --cookies ] && jar=$a\n"
            "  prev=$a; mode=$a\n"
            "done\n"
            "echo \"argv: $*\" >&2\n"
            "if [ -n \"$jar\" ]; then\n"
            "  if [ -f \"$jar\" ]; then\n"
            "    echo \"jar-mode=$(stat -c %a \"$jar\" 2>/dev/null || stat -f %Lp \"$jar\")\"\n"
            "    case \"$jar\" in \"$SEC_PRIVATE\"/*) echo jar-private=yes ;; *) echo jar-private=no ;; esac\n"
            "  else echo jar-missing; fi\n"
            "fi\n"
            "tab=$(printf '\\t')\n"
            "case \"$mode\" in\n"
            "  ok) if [ -n \"$jar\" ]; then content=$(cat \"$jar\"); "
            "printf '%s\\n' \"$content\" | sed \"s/${tab}SIDCC${tab}.*/${tab}SIDCC${tab}$SEC_ROTATED/\" > \"$jar\"; fi\n"
            "      echo '{\"id\":\"synthetic\"}'; exit 0 ;;\n"
            "  fail) echo \"ERROR: [youtube] synthetic: unable to read $jar\" >&2; exit 1 ;;\n"
            "  *) sleep 20; exit 0 ;;\n"
            "esac\n");
    f.close();
    QFile::setPermissions(path, QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner);
    return path;
}

static QStringList results;   // every extractor output and diagnostics text (what a caller may log)
static void keep(const resolver::RunResult &r) {
    results << QString::fromUtf8(r.output) << QString::fromUtf8(r.diagnostics);
}

// ---------------------------------------------------------------- the session

struct Synthetic {
    std::string sapisid, p3apisid, p1apisid, sid, hsid, ssid, apisid, p3psid, sidcc, p3psidcc, loginInfo, authDigest;
    std::string refusedSid, refusedHsid;
    std::string browserSapisid, browserSid, browserP3psid;
};
static Synthetic S;

static void makeSecrets() {
    S.sapisid = synthetic("SAPISID"); S.p3apisid = synthetic("3PAPISID"); S.p1apisid = synthetic("1PAPISID");
    S.sid = synthetic("SID"); S.hsid = synthetic("HSID"); S.ssid = synthetic("SSID"); S.apisid = synthetic("APISID");
    S.p3psid = synthetic("3PSID"); S.sidcc = synthetic("SIDCC"); S.p3psidcc = synthetic("3PSIDCC");
    S.loginInfo = synthetic("LOGIN_INFO");
    S.authDigest = QUuid::createUuid().toRfc4122().toHex().toStdString() + "deadbeef"; // a pasted SAPISIDHASH digest
    S.refusedSid = synthetic("REFUSED-SID"); S.refusedHsid = synthetic("REFUSED-HSID");
    S.browserSapisid = synthetic("BROWSER-SAPISID"); S.browserSid = synthetic("BROWSER-SID"); S.browserP3psid = synthetic("BROWSER-3PSID");
    rotatedMenu = synthetic("ROTATED-SIDCC-MENU");
    rotatedBrowse = synthetic("ROTATED-3PSIDCC-BROWSE");
    rotatedJar = synthetic("ROTATED-SIDCC-JAR");
    for (const std::string *v : {&S.sapisid, &S.p3apisid, &S.p1apisid, &S.sid, &S.hsid, &S.ssid, &S.apisid, &S.p3psid,
                                 &S.sidcc, &S.p3psidcc, &S.loginInfo, &S.authDigest, &S.refusedSid, &S.refusedHsid,
                                 &S.browserSapisid, &S.browserSid, &S.browserP3psid, &rotatedMenu, &rotatedBrowse, &rotatedJar})
        secret(*v);
}

static QString pastedRequest() {
    const QString c = QStringLiteral("SAPISID=%1; __Secure-3PAPISID=%2; __Secure-1PAPISID=%3; SID=%4; HSID=%5; SSID=%6; "
                                     "APISID=%7; __Secure-3PSID=%8; SIDCC=%9; ")
        .arg(QString::fromStdString(S.sapisid), QString::fromStdString(S.p3apisid), QString::fromStdString(S.p1apisid),
             QString::fromStdString(S.sid), QString::fromStdString(S.hsid), QString::fromStdString(S.ssid),
             QString::fromStdString(S.apisid), QString::fromStdString(S.p3psid), QString::fromStdString(S.sidcc))
        + QStringLiteral("__Secure-3PSIDCC=%1; LOGIN_INFO=%2").arg(QString::fromStdString(S.p3psidcc), QString::fromStdString(S.loginInfo));
    return QStringLiteral("POST /youtubei/v1/browse?prettyPrint=false HTTP/2\nHost: www.youtube.com\n"
                          "Authorization: SAPISIDHASH 1800000000_%1\nCookie: %2\nX-Goog-AuthUser: 0\n")
        .arg(QString::fromStdString(S.authDigest), c);
}

static std::string sessionValue(const AccountStore &store, const char *name) {
    const SessionCookies s = store.session();
    const Cookie *c = s.find(name);
    return c ? c->value : std::string();
}

static void collectSession(const AccountStore &store) {
    for (const Cookie &c : store.session().cookies) secret(c.value);
}

// ---------------------------------------------------------------- extractor calls

static void extractorCalls(AccountService &service, AccountStore &store, const QString &extractor) {
    resolver::RunnerOptions o;
    o.executablePath = extractor;
    o.pythonChild = false;
    o.maximumConcurrent = 2;
    resolver::YtDlpRunner runner(o);
    // As in main.cpp: a per-call jar, released exactly once.
    const auto begin = [&service]() {
        const AccountService::ExtractorCall c = service.beginExtractorCall();
        if (!c.jarPath.isEmpty()) openJars << c.jarPath;
        resolver::ExtractorCookies e; e.jarPath = c.jarPath; e.generation = c.generation;
        return e;
    };
    const auto end = [&service](const resolver::ExtractorCookies &e) {
        AccountService::ExtractorCall c; c.jarPath = e.jarPath; c.generation = e.generation;
        service.endExtractorCall(c);
        openJars.removeAll(e.jarPath);
    };
    runner.setCookieHooks(begin, end);
    QStringList base = runner.commonArguments(false);
    const auto args = [&base](const char *mode) {
        QStringList a = base;
        a << QStringLiteral("--") << QString::fromLatin1(mode);
        return a;
    };
    QString lastJar;

    // success: the jar is 0600 in the private dir while the extractor runs; its rotation folds back
    bool done = false;
    resolver::RunResult result;
    runner.run(args("ok"), 10000, 1 << 20, [&](const resolver::RunResult &r) { result = r; done = true; });
    CHECK(openJars.size() == 1);
    if (!openJars.isEmpty()) lastJar = openJars.first();
    snapshot(QStringLiteral("extractor ok: running"));
    CHECK(waitFor([&] { return done; }));
    keep(result);
    CHECK(result.exitCode == 0);
    CHECK(result.output.contains("jar-mode=600"));
    CHECK(result.output.contains("jar-private=yes"));
    CHECK(result.diagnostics.contains("--cookies [cookies]"));        // the runner scrubbed the path
    CHECK(!lastJar.isEmpty() && !result.diagnostics.contains(lastJar.toUtf8()));
    CHECK(!QFileInfo::exists(lastJar) && !QFileInfo::exists(QFileInfo(lastJar).absolutePath()));
    CHECK(sessionValue(store, "SIDCC") == rotatedJar);
    snapshot(QStringLiteral("extractor ok: ended"));

    // failure: the extractor exits 1 and names the jar on stderr
    done = false;
    runner.run(args("fail"), 10000, 1 << 20, [&](const resolver::RunResult &r) { result = r; done = true; });
    if (!openJars.isEmpty()) lastJar = openJars.first();
    CHECK(waitFor([&] { return done; }));
    keep(result);
    CHECK(result.exitCode == 1);
    CHECK(!result.diagnostics.contains(lastJar.toUtf8()) && result.diagnostics.contains("[cookies]"));
    CHECK(!QFileInfo::exists(lastJar) && openJars.isEmpty());
    snapshot(QStringLiteral("extractor failure"));

    // timeout: the runner stops the process group
    done = false;
    runner.run(args("hang"), 1000, 1 << 20, [&](const resolver::RunResult &r) { result = r; done = true; });
    if (!openJars.isEmpty()) lastJar = openJars.first();
    spin(300);
    snapshot(QStringLiteral("extractor timeout: running"));
    CHECK(waitFor([&] { return done; }, 8000));
    keep(result);
    CHECK(result.timedOut);
    CHECK(!QFileInfo::exists(lastJar) && openJars.isEmpty());
    snapshot(QStringLiteral("extractor timeout"));

    // cancel
    done = false;
    const quint64 id = runner.run(args("cancel"), 10000, 1 << 20, [&](const resolver::RunResult &r) { result = r; done = true; });
    if (!openJars.isEmpty()) lastJar = openJars.first();
    spin(300);
    runner.cancel(id);
    CHECK(waitFor([&] { return done; }));
    keep(result);
    CHECK(result.cancelled);
    CHECK(!QFileInfo::exists(lastJar) && openJars.isEmpty());
    snapshot(QStringLiteral("extractor cancel"));

    // three at once (two run, one waits): three distinct jars, all removed
    int finished = 0;
    QStringList jars;
    for (int i = 0; i < 3; ++i) {
        runner.run(args("ok"), 10000, 1 << 20, [&](const resolver::RunResult &r) { keep(r); ++finished; });
        if (!openJars.isEmpty()) jars << openJars.last();
    }
    CHECK(QSet<QString>::fromList(jars).size() == 3);
    snapshot(QStringLiteral("extractor x3: running"));
    CHECK(waitFor([&] { return finished == 3; }));
    for (const QString &j : jars) CHECK(!QFileInfo::exists(j));
    CHECK(openJars.isEmpty() && jarDirs() == 0);
    snapshot(QStringLiteral("extractor x3: ended"));
    runner.dropCookieHooks();

    // the runner goes away mid-call: the jar is still released
    {
        resolver::YtDlpRunner shortLived(o);
        shortLived.setCookieHooks(begin, end);
        shortLived.run(args("hang"), 10000, 1 << 20, [](const resolver::RunResult &) {});
        if (!openJars.isEmpty()) lastJar = openJars.first();
        spin(300);
    }
    CHECK(!QFileInfo::exists(lastJar) && openJars.isEmpty());
    snapshot(QStringLiteral("extractor runner destroyed"));

    // direct begin/end: an extractor that died leaving a damaged jar, and one that removed it
    const std::string before = sessionValue(store, "SID");
    AccountService::ExtractorCall c = service.beginExtractorCall();
    CHECK(!c.jarPath.isEmpty());
    openJars << c.jarPath;
    CHECK(modeOf(c.jarPath) == 0600 && under(c.jarPath, L.priv));
    snapshot(QStringLiteral("direct call: open"));
    { QFile j(c.jarPath); j.open(QIODevice::WriteOnly | QIODevice::Truncate); j.write("garbage\tline\n"); }
    service.endExtractorCall(c);
    openJars.removeAll(c.jarPath);
    CHECK(!QFileInfo::exists(c.jarPath) && sessionValue(store, "SID") == before);
    c = service.beginExtractorCall();
    openJars << c.jarPath;
    QFile::remove(c.jarPath);
    service.endExtractorCall(c);
    openJars.removeAll(c.jarPath);
    CHECK(!QFileInfo::exists(QFileInfo(c.jarPath).absolutePath()) && jarDirs() == 0);
    snapshot(QStringLiteral("direct call: ended"));
}

// ---------------------------------------------------------------- backup and report

static void backupAndReport(const AccountStore &store) {
    const QString ini = L.config + QStringLiteral("/harbour-sailtube.conf");
    {
        QSettings s(ini, QSettings::IniFormat);
        s.setValue(QStringLiteral("playback/maxHeight"), 1080);
        s.setValue(QStringLiteral("ui/theme"), QStringLiteral("dark"));
        // credentials that a careless setting could have picked up
        s.setValue(QStringLiteral("account/cookie"), QString::fromStdString(S.sapisid));
        s.setValue(QStringLiteral("ui/sessionToken"), QString::fromStdString(S.sid));
        s.setValue(QStringLiteral("downloads/authHeader"), QStringLiteral("SAPISIDHASH 1_") + QString::fromStdString(S.authDigest));
        s.setValue(QStringLiteral("library/lastHeaders"), QStringLiteral("Cookie: SAPISID=%1; SID=%2")
                   .arg(QString::fromStdString(sessionValue(store, "SAPISID")), QString::fromStdString(sessionValue(store, "SID"))));
        s.setValue(QStringLiteral("content/sapisid"), QString::fromStdString(S.p3apisid));
        s.sync();
    }
    const QString target = L.backup + QStringLiteral("/sailtube-backup.json");
    library::ImportBatch lib;
    lib.source = "sailtube";
    QString error;
    CHECK(library::writeSailTubeBackup(target, lib, library::backupSettingsFromIni(ini), QVariantMap(), 1800000000000LL, &error));
    QFile f(target);
    CHECK(f.open(QIODevice::ReadOnly));
    const QByteArray backup = f.readAll();
    CHECK(backup.contains("sailtube-backup") && backup.contains("maxHeight"));   // the export did run
    scan(QStringLiteral("backup"), backup);
    snapshot(QStringLiteral("backup exported"));

    // DIA-02 with the session's values in `secrets` and hostile free text in the fields
    nordtile::diagnostics::ReportInput in;
    in.versions.push_back(std::make_pair(std::string("SailTube"), std::string("test")));
    in.versions.push_back(std::make_pair(std::string("cookie"), std::string("SAPISID=") + sessionValue(store, "SAPISID")));
    for (const QString &line : allLogs().split(QLatin1Char('\n'))) in.versionSkew.push_back(line.toStdString());
    for (const QString &r : results) in.versionSkew.push_back(r.toStdString());
    in.versionSkew.push_back(std::string("Cookie: ") + cookieHeader(store.session()));
    in.versionSkew.push_back(std::string("Authorization: SAPISIDHASH 1_") + S.authDigest);
    in.breakerState = "closed after " + rotatedJar;
    in.poProviderState = "not configured";
    nordtile::diagnostics::ResolveTimeline t;
    t.path = "extractor"; t.postMs = 1200;
    in.timelines.push_back(t);
    in.hardwareDecodeProbe = "h264 hw";
    in.networkFamily = "dual " + sessionValue(store, "SIDCC");
    in.secrets.assign(secrets.begin(), secrets.end());
    const std::string report = nordtile::diagnostics::buildReport(in);
    CHECK(report.find("[redacted]") != std::string::npos);   // hostile text did reach the builder
    scan(QStringLiteral("diagnostics report"), QByteArray::fromStdString(report));
}

// ---------------------------------------------------------------- browser database (vault variant)

static void writeBrowserDb() {
    const QString path = L.home + QStringLiteral("/.local/share/org.sailfishos/browser/.mozilla/cookies.sqlite");
    QDir().mkpath(QFileInfo(path).absolutePath());
    {
        QSqlDatabase db = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), QStringLiteral("synthetic-browser"));
        db.setDatabaseName(path);
        CHECK(db.open());
        QSqlQuery q(db);
        CHECK(q.exec(QStringLiteral("CREATE TABLE moz_cookies (id INTEGER PRIMARY KEY, name TEXT, value TEXT, host TEXT, "
                                    "path TEXT, expiry INTEGER, isSecure INTEGER, isHttpOnly INTEGER)")));
        const auto add = [&db](const char *name, const std::string &value) {
            QSqlQuery ins(db);
            ins.prepare(QStringLiteral("INSERT INTO moz_cookies(name, value, host, path, expiry, isSecure, isHttpOnly) "
                                       "VALUES(?,?,'.youtube.com','/',0,1,0)"));
            ins.addBindValue(QString::fromLatin1(name));
            ins.addBindValue(QString::fromStdString(value));
            CHECK(ins.exec());
        };
        add("SAPISID", S.browserSapisid);
        add("SID", S.browserSid);
        add("__Secure-3PSID", S.browserP3psid);
        db.close();
    }
    QSqlDatabase::removeDatabase(QStringLiteral("synthetic-browser"));
    QFile::setPermissions(path, QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ReadGroup | QFileDevice::ReadOther);
}

// ---------------------------------------------------------------- file-system trace (Linux preload)

static void checkTrace() {
    const QString tracePath = QString::fromLocal8Bit(qgetenv("SEC_TRACE"));
    if (tracePath.isEmpty()) { std::printf("  file-system trace: not available on this host (snapshots only)\n"); return; }
    QFile f(tracePath);
    if (!f.open(QIODevice::ReadOnly)) { fail(QStringLiteral("trace %1 unreadable").arg(tracePath)); return; }
    const QByteArray self = QByteArray::number(qint64(QCoreApplication::applicationPid()));
    int lines = 0, own = 0, notes = 0;
    for (const QByteArray &raw : f.readAll().split('\n')) {
        if (raw.isEmpty()) continue;
        ++lines;
        const QList<QByteArray> p = raw.split(' ');
        if (p.size() < 5 || p.at(1) != self) continue;
        ++own;
        const char op = p.at(0).at(0);
        const unsigned mode = p.at(2).toUInt(nullptr, 8);
        const qint64 size = p.at(3).toLongLong();
        const QString rest = QString::fromLocal8Bit(raw.mid(p.at(0).size() + p.at(1).size() + p.at(2).size() + p.at(3).size() + 4));
        const QString path = rest.section(QLatin1Char('\t'), 0, 0);
        const bool sensitive = under(path, L.priv) || under(path, L.tmp);
        if (op == 'W' && sensitive)
            fail(QStringLiteral("[trace] bytes written to %1 while its mode was %2").arg(rel(path), octal(mode)));
        if (op == 'M' && sensitive && (mode & 077) && size > 0)
            fail(QStringLiteral("[trace] %1 (%2 bytes) was given mode %3").arg(rel(path)).arg(size).arg(octal(mode)));
        if (op == 'C' && !userOwned(path) && !under(path, L.backup)) {
            if (!under(path, L.priv)) fail(QStringLiteral("[trace] file %1 created outside the private dir").arg(rel(path)));
            else if ((mode & 0777) != 0600) {
                // Created with a wider mode and narrowed before any byte (a W line would follow otherwise).
                std::printf("  note: %s created with mode %s, narrowed before it was written\n", qPrintable(rel(path)), qPrintable(octal(mode)));
                ++notes;
            }
        }
        if (op == 'R') {
            const QString to = rest.section(QLatin1Char('\t'), 1, 1);
            if (under(path, L.priv) != under(to, L.priv))
                fail(QStringLiteral("[trace] rename %1 -> %2 crosses the private dir").arg(rel(path), rel(to)));
        }
    }
    CHECK(own > 0);   // the shim did see this process
    std::printf("  file-system trace: %d events (%d of this process, %d notes)\n", lines, own, notes);
}

// ---------------------------------------------------------------- main

int main(int argc, char **argv) {
    ::umask(022);   // the device session's umask; the code must not depend on a stricter one
    qputenv("QT_LOGGING_RULES", "*.debug=true");
    QCoreApplication app(argc, argv);
    qInstallMessageHandler(captureMessages);
    const QString root = QDir(QString::fromLocal8Bit(qgetenv("SEC_ROOT"))).canonicalPath();
    if (root.isEmpty()) { std::printf("secrets tests: FAIL (SEC_ROOT unset)\n"); return 1; }
    L.root = root;
    L.data = root + QStringLiteral("/data");
    L.priv = L.data + QStringLiteral("/account");
    L.tmp = root + QStringLiteral("/tmp");
    L.home = root + QStringLiteral("/home");
    L.inputs = root + QStringLiteral("/inputs");
    L.bin = root + QStringLiteral("/bin");
    L.config = root + QStringLiteral("/config");
    L.backup = root + QStringLiteral("/backup");
    for (const QString &d : {L.data, L.tmp, L.home, L.inputs, L.bin, L.config, L.backup}) QDir().mkpath(d);
    CHECK(QDir::tempPath() == L.tmp);   // run.sh points TMPDIR here
    makeSecrets();
    qputenv("SEC_PRIVATE", QFile::encodeName(L.priv));
    qputenv("SEC_ROTATED", QByteArray::fromStdString(rotatedJar));
    const QString extractor = writeFakeExtractor();
    qWarning("secrets test: message capture active");

    // ---- FULL, no vault: the session lives in the 0600 file fallback
    {
        AccountStoreOptions options;
        options.variant = BuildVariant::Full;
        options.privateDir = L.priv;
        AccountStore store(options);
        store.open();
        FakeTransport net;
        AccountService service(&store, &net);
        PinnedSessionParser parser;
        sailfish::AccountController::Options ui;
        ui.variant = BuildVariant::Full;
        ui.home = L.home;
        ui.scratchDir = L.priv + QStringLiteral("/scratch");
        sailfish::AccountController controller(&store, &service, &parser, ui);
        QStringList messages;
        QObject::connect(&controller, &sailfish::AccountController::changed, [&] {
            messages << controller.lastError() << controller.stateText() << controller.accountName();
        });
        snapshot(QStringLiteral("start"));

        CHECK(!controller.importText(QStringLiteral("Cookie: SID=%1; HSID=%2")
                                     .arg(QString::fromStdString(S.refusedSid), QString::fromStdString(S.refusedHsid))));
        CHECK(!store.signedIn());
        snapshot(QStringLiteral("refused paste"));

        // sign-in by file (ACC-04): the picked file is the user's, outside the app's dirs
        const QString picked = L.inputs + QStringLiteral("/request.txt");
        { QFile p(picked); p.open(QIODevice::WriteOnly); p.write(pastedRequest().toUtf8()); }
        CHECK(controller.importFile(picked));
        expectSessionFile = true;
        CHECK(store.signedIn() && store.usingFileFallback());
        CHECK(sessionValue(store, "SAPISID") == S.sapisid);
        CHECK(waitFor([&] { return service.state() == SessionState::SignedIn && !service.accounts().empty(); }));
        CHECK(sessionValue(store, "SIDCC") == rotatedMenu);   // ACC-08 rotation from the verification answer
        CHECK(modeOf(store.sessionFilePath()) == 0600);
        collectSession(store);
        snapshot(QStringLiteral("signed in (file fallback)"));
        if (controller.disclosurePending()) controller.acknowledgeDisclosure();
        snapshot(QStringLiteral("disclosure acknowledged"));

        CHECK(controller.select(1));   // the brand channel
        CHECK(waitFor([&] { return service.state() == SessionState::SignedIn; }));
        bool posted = false;
        service.post(QStringLiteral("browse"), QJsonObject{{QStringLiteral("browseId"), QStringLiteral("FEwhat_to_watch")}},
                     [&posted](bool, int, const QJsonObject &) { posted = true; });
        CHECK(waitFor([&] { return posted; }));
        CHECK(sessionValue(store, "__Secure-3PSIDCC") == rotatedBrowse);
        controller.verifyNow();
        CHECK(waitFor([&] { return service.state() == SessionState::SignedIn; }));
        collectSession(store);
        snapshot(QStringLiteral("switch + signed browse"));

        extractorCalls(service, store, extractor);
        collectSession(store);
        backupAndReport(store);

        controller.signOut();
        expectSessionFile = false;
        CHECK(!store.signedIn() && !QFileInfo::exists(store.sessionFilePath()));
        CHECK(!QFileInfo::exists(L.priv + QStringLiteral("/jars")) || jarDirs() == 0);
        CHECK(service.beginExtractorCall().jarPath.isEmpty());
        snapshot(QStringLiteral("signed out (file fallback)"));
        noSecretAnywhere(QStringLiteral("signed out (file fallback)"));
        for (const QString &m : messages) scan(QStringLiteral("controller message"), m.toUtf8());
        CHECK(net.sent > 0);
    }

    // ---- FULL with a vault: browser import; nothing secret on disk but the per-call jar
    {
        writeBrowserDb();
        FakeVault vault;
        AccountStoreOptions options;
        options.variant = BuildVariant::Full;
        options.privateDir = L.priv;
        options.vault = &vault;
        AccountStore store(options);
        store.open();
        FakeTransport net;
        AccountService service(&store, &net);
        PinnedSessionParser parser;
        sailfish::AccountController::Options ui;
        ui.variant = BuildVariant::Full;
        ui.home = L.home;
        ui.scratchDir = L.priv + QStringLiteral("/scratch");
        sailfish::AccountController controller(&store, &service, &parser, ui);
        // start from a missing private dir, as on a first sign-in
        QDir(L.priv).removeRecursively();
        CHECK(controller.importFromBrowser());
        CHECK(store.signedIn() && !store.usingFileFallback() && !vault.blob.isEmpty());
        CHECK(sessionValue(store, "SAPISID") == S.browserSapisid);
        CHECK(waitFor([&] { return service.state() == SessionState::SignedIn; }));
        collectSession(store);
        snapshot(QStringLiteral("browser import (vault)"));
        AccountService::ExtractorCall c = service.beginExtractorCall();
        CHECK(!c.jarPath.isEmpty() && modeOf(c.jarPath) == 0600);
        openJars << c.jarPath;
        snapshot(QStringLiteral("vault: extractor call open"));
        service.endExtractorCall(c);
        openJars.removeAll(c.jarPath);
        snapshot(QStringLiteral("vault: extractor call ended"));
        controller.signOut();
        CHECK(!store.signedIn() && vault.blob.isEmpty());
        snapshot(QStringLiteral("signed out (vault)"));
        noSecretAnywhere(QStringLiteral("signed out (vault)"));
        scan(QStringLiteral("controller message"), controller.lastError().toUtf8());
    }

    // ---- logs: every captured Qt message, every extractor result
    const QString captured = allLogs();
    CHECK(captured.contains(QStringLiteral("message capture active")));
    scan(QStringLiteral("captured log"), captured.toUtf8());
    CHECK(!captured.contains(QStringLiteral("--cookies")));
    CHECK(!captured.contains(L.priv + QStringLiteral("/jars")));
    for (const QString &r : results) scan(QStringLiteral("extractor result"), r.toUtf8());
    QDirIterator left(L.tmp, QDir::AllEntries | QDir::Hidden | QDir::NoDotAndDotDot);
    while (left.hasNext()) fail(QStringLiteral("TMPDIR entry left behind: %1").arg(rel(left.next())));
    checkTrace();

    std::printf("  %d secrets, %d log lines, %d extractor results scanned\n",
                int(secrets.size()), captured.split(QLatin1Char('\n')).size(), results.size());
    std::printf("secrets tests: %s (%d failures)\n", failures ? "FAIL" : "PASS", failures);
    return failures ? 1 : 0;
}
