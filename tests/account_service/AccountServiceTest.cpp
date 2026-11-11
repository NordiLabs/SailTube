// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// The optional account session over a fake transport (no network, synthetic
// answers in the documented shapes; no real cookie or secret anywhere):
//  * ACC-07: account/account_menu -> accepted / rejected / inconclusive; checks at
//    start, after import, after a switch, on demand and every 15 min while signed
//    in (fake clock); inconclusive keeps the state.
//  * ACC-06: every request is signed; X-Goog-AuthUser always, X-Goog-PageId only
//    for a brand channel; ?prettyPrint kept on the InnerTube URL.
//  * ACC-08: Set-Cookie on an answer rotates the stored session.
//  * ACC-09: accounts_list probing 0..7, dedupe, stop rule, probe-0 network
//    failure = unreachable; a switch invalidates caches; selection is the stored one.
//  * ACC-10: sign-out clears everything and a late answer cannot restore it.
//  * ACC-01: verification failures never block the event loop (a ticking timer).
//  * ACC-02: the controller lists methods in rank order per variant.
//  * ACC-03: browser import with a login only in the WAL.
//  * IMP-02: browse() carries the importer's identity headers.
#include "Clients/Sailfish/Presentation/AccountController.h"
#include "Core/Account/Infrastructure/AccountService.h"
#include "Core/Account/Infrastructure/AccountStore.h"
#include "Core/Account/Infrastructure/BrowserCookieImport.h"
#include "Core/Account/Infrastructure/SessionTextImport.h"
#include "Core/Account/Infrastructure/SignedCatalogTransport.h"

#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QPointer>
#include <QSqlDatabase>
#include <QSqlQuery>
#include <QTimer>
#include <cstdio>
#include <functional>

using namespace nordtile::video;
using namespace nordtile::video::account;

static int failures = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); ++failures; } } while (0)

static void spin(int ms) { QEventLoop l; QTimer::singleShot(ms, &l, SLOT(quit())); l.exec(); }
static bool waitFor(const std::function<bool()> &f, int ms = 3000) {
    QElapsedTimer t; t.start();
    while (!f() && t.elapsed() < ms) spin(5);
    return f();
}

static QByteArray json(const QJsonObject &o) { return QJsonDocument(o).toJson(QJsonDocument::Compact); }
static QJsonObject runs(const QString &t) { return QJsonObject{{QStringLiteral("runs"), QJsonArray{QJsonObject{{QStringLiteral("text"), t}}}}}; }

static QJsonObject accountMenu(const QString &name) {
    QJsonObject header{{QStringLiteral("accountName"), runs(name)}, {QStringLiteral("channelHandle"), runs(QStringLiteral("@fake"))}};
    QJsonObject popup{{QStringLiteral("header"), QJsonObject{{QStringLiteral("activeAccountHeaderRenderer"), header}}}};
    return QJsonObject{{QStringLiteral("actions"), QJsonArray{QJsonObject{{QStringLiteral("openPopupAction"),
        QJsonObject{{QStringLiteral("popup"), QJsonObject{{QStringLiteral("multiPageMenuRenderer"), popup}}}}}}}}};
}
static QJsonObject anonymousMenu() {
    return QJsonObject{{QStringLiteral("actions"), QJsonArray{QJsonObject{{QStringLiteral("openPopupAction"),
        QJsonObject{{QStringLiteral("popup"), QJsonObject{{QStringLiteral("multiPageMenuRenderer"), QJsonObject{{QStringLiteral("sections"), QJsonArray()}}}}}}}}}}};
}

static QJsonObject accountItem(const QString &name, const QString &gaia, const QString &pageId, bool useRenderer = false) {
    QJsonArray tokens;
    tokens.append(QJsonObject{{QStringLiteral("accountStateToken"), QJsonObject{{QStringLiteral("obfuscatedGaiaId"), gaia}}}});
    if (!pageId.isEmpty()) tokens.append(QJsonObject{{QStringLiteral("pageIdToken"), QJsonObject{{QStringLiteral("pageId"), pageId}}}});
    tokens.append(QJsonObject{{QStringLiteral("datasyncIdToken"), QJsonObject{{QStringLiteral("datasyncId"), gaia + QStringLiteral("||") + pageId}}}});
    QJsonObject item{{QStringLiteral("accountName"), QJsonObject{{QStringLiteral("simpleText"), name}}},
                     {QStringLiteral("hasChannel"), true},
                     {QStringLiteral("serviceEndpoint"), QJsonObject{{QStringLiteral("selectActiveIdentityEndpoint"),
                         QJsonObject{{QStringLiteral("supportedTokens"), tokens}}}}}};
    return QJsonObject{{useRenderer ? QStringLiteral("accountItemRenderer") : QStringLiteral("accountItem"), item}};
}
static QJsonObject accountsList(const QJsonArray &items, bool loggedOut = false) {
    QJsonObject ctx{{QStringLiteral("mainAppWebResponseContext"), QJsonObject{{QStringLiteral("loggedOut"), loggedOut}}}};
    QJsonObject section{{QStringLiteral("accountSectionListRenderer"), QJsonObject{{QStringLiteral("contents"), items}}}};
    return QJsonObject{{QStringLiteral("responseContext"), ctx}, {QStringLiteral("actions"), QJsonArray{section}}};
}

struct Sent {
    AuthRequest request;
    QString endpoint;
    QByteArray header(const char *name) const {
        for (const auto &h : request.headers)
            if (h.first == name) return h.second;
        return QByteArray();
    }
};

class FakeTransport final : public QObject, public AuthTransport {
public:
    std::function<AuthReply(const Sent &)> answer;
    QList<Sent> sent;
    int delayMs = 1;
    void send(const AuthRequest &request, QObject *context, Done done) override {
        Sent s; s.request = request;
        s.endpoint = request.url.path().mid(QStringLiteral("/youtubei/v1/").size());
        sent.append(s);
        const AuthReply r = answer ? answer(s) : AuthReply();
        QPointer<QObject> guard(context);
        QTimer::singleShot(delayMs, this, [guard, done, r] { if (guard) done(r); });
    }
    int count(const QString &endpoint) const { int n = 0; for (const Sent &s : sent) n += s.endpoint == endpoint; return n; }
};

class FakeVault final : public SecretVault {
public:
    bool available() const override { return true; }
    bool store(const QByteArray &s) override { blob = s; return true; }
    bool load(QByteArray *s) override { if (blob.isEmpty()) return false; *s = blob; return true; }
    bool erase() override { blob.clear(); return true; }
    QByteArray blob;
};

static SessionCookies fakeSession() {
    SessionCookies s;
    Cookie c;
    c.domain = ".youtube.com";
    c.name = "SAPISID"; c.value = "fake-sapisid"; s.cookies.push_back(c);
    c.name = "__Secure-3PAPISID"; c.value = "fake-3papisid"; s.cookies.push_back(c);
    c.name = "SID"; c.value = "fake-sid"; s.cookies.push_back(c);
    c.name = "__Secure-3PSIDCC"; c.value = "fake-sidcc-1"; s.cookies.push_back(c);
    return s;
}

static AuthReply ok(const QJsonObject &o) { AuthReply r; r.status = 200; r.body = json(o); return r; }

static void parsers() {
    QString name;
    CHECK(AccountService::parseAccountMenu(200, accountMenu(QStringLiteral("Fake User")), &name) == Verification::Accepted);
    CHECK(name == QStringLiteral("Fake User"));
    CHECK(AccountService::parseAccountMenu(200, anonymousMenu(), &name) == Verification::Rejected);
    CHECK(AccountService::parseAccountMenu(0, QJsonObject(), &name) == Verification::Inconclusive);
    CHECK(AccountService::parseAccountMenu(503, accountMenu(QStringLiteral("x")), &name) == Verification::Inconclusive);
    CHECK(AccountService::parseAccountMenu(200, QJsonObject(), &name) == Verification::Inconclusive); // not JSON

    bool loggedOut = true;
    const std::vector<AccountEntry> e = AccountService::parseAccountsList(
        accountsList(QJsonArray{accountItem(QStringLiteral("Login"), QStringLiteral("g1"), QString()),
                                accountItem(QStringLiteral("Brand"), QStringLiteral("g1"), QStringLiteral("p1"), true)}), 2, &loggedOut);
    CHECK(!loggedOut && e.size() == 2);
    if (e.size() == 2) {
        CHECK(e[0].name == "Login" && e[0].gaiaId == "g1" && e[0].pageId.empty() && e[0].authUser == 2);
        CHECK(e[1].name == "Brand" && e[1].pageId == "p1"); // accountItemRenderer fallback
    }
    AccountService::parseAccountsList(accountsList(QJsonArray(), true), 0, &loggedOut);
    CHECK(loggedOut);

    const std::vector<SetCookieValue> sc = AccountService::parseSetCookies(
        {QByteArray("__Secure-3PSIDCC=fake-sidcc-2; Domain=.youtube.com; Path=/; Secure; HttpOnly"),
         QByteArray("SIDCC=gone; Domain=.youtube.com; Expires=Thu, 01 Jan 1970 00:00:01 GMT")}, 1700000000);
    CHECK(sc.size() == 2);
    if (sc.size() == 2) {
        CHECK(sc[0].name == "__Secure-3PSIDCC" && sc[0].value == "fake-sidcc-2" && !sc[0].expired);
        CHECK(sc[1].expired);
    }
}

static void service(const QString &dir) {
    FakeVault vault;
    AccountStoreOptions options;
    options.privateDir = dir + QStringLiteral("/account");
    options.vault = &vault;
    AccountStore store(options);
    CHECK(!store.open()); // nothing stored yet
    FakeTransport net;
    qint64 now = 1700000000000LL;
    QString menuMode = QStringLiteral("accepted");
    net.answer = [&](const Sent &s) {
        if (s.endpoint == QLatin1String("account/account_menu")) {
            if (menuMode == QLatin1String("accepted")) {
                AuthReply r = ok(accountMenu(QStringLiteral("Fake User")));
                r.setCookies.push_back("__Secure-3PSIDCC=fake-sidcc-rotated; Domain=.youtube.com; Path=/; Secure");
                return r;
            }
            if (menuMode == QLatin1String("rejected")) return ok(anonymousMenu());
            return AuthReply(); // network
        }
        if (s.endpoint == QLatin1String("account/accounts_list")) {
            const int user = s.header("X-Goog-AuthUser").toInt();
            if (user == 0) return ok(accountsList(QJsonArray{accountItem(QStringLiteral("Login A"), QStringLiteral("ga"), QString()),
                                                             accountItem(QStringLiteral("Brand A"), QStringLiteral("ga"), QStringLiteral("pa"))}));
            if (user == 1) return ok(accountsList(QJsonArray{accountItem(QStringLiteral("Login B"), QStringLiteral("gb"), QString()),
                                                             accountItem(QStringLiteral("Login A"), QStringLiteral("ga"), QString())}));
            return ok(accountsList(QJsonArray{accountItem(QStringLiteral("Login A"), QStringLiteral("ga"), QString())})); // all duplicates
        }
        if (s.endpoint == QLatin1String("browse")) return ok(QJsonObject{{QStringLiteral("contents"), QJsonObject()}});
        return AuthReply();
    };
    AccountService svc(&store, &net);
    svc.setClockForTests([&now] { return now; });
    int invalidated = 0;
    QObject::connect(&svc, &AccountService::cachesInvalidated, [&invalidated] { ++invalidated; });

    // signed out: nothing is sent at all
    svc.start();
    CHECK(svc.state() == SessionState::SignedOut);
    svc.verifyNow();
    now += 20 * 60 * 1000;
    svc.pollSchedule();
    CHECK(net.sent.isEmpty());
    CHECK(svc.sign().empty());

    // import -> verify (accepted) and probe
    CHECK(store.importSession(fakeSession()) == ImportRefusal::None);
    svc.sessionImported();
    CHECK(svc.state() == SessionState::Checking);
    CHECK(waitFor([&] { return svc.state() == SessionState::SignedIn && svc.accounts().size() == 3; }));
    CHECK(svc.accountName() == QStringLiteral("Fake User"));
    CHECK(net.count(QStringLiteral("account/account_menu")) == 1);
    CHECK(net.count(QStringLiteral("account/accounts_list")) == 3); // 0, 1, then 2 = all duplicates -> stop
    const std::vector<AccountEntry> accounts = svc.accounts();
    if (accounts.size() == 3) {
        CHECK(accounts[0].name == "Login A" && accounts[1].name == "Brand A" && accounts[2].name == "Login B");
        CHECK(accounts[2].authUser == 1 && accounts[1].pageId == "pa");
    }
    for (const Sent &s : net.sent) {
        CHECK(s.request.url.toString().startsWith(QStringLiteral("https://www.youtube.com/youtubei/v1/")));
        CHECK(s.request.url.query().contains(QStringLiteral("prettyPrint=false")));
        CHECK(s.header("Authorization").startsWith("SAPISIDHASH "));
        CHECK(!s.header("Cookie").isEmpty());
        CHECK(s.header("Origin") == "https://www.youtube.com");
        CHECK(!s.header("X-Goog-AuthUser").isEmpty());
        CHECK(s.header("X-Goog-PageId").isEmpty()); // no brand selected; probes never send one
        CHECK(!QString::fromUtf8(s.request.body).contains(QStringLiteral("fake-"))); // secrets only in headers
    }
    // ACC-08: the rotation in the verification answer reached the store
    const SessionCookies stored = store.session();
    const Cookie *rotated = stored.find("__Secure-3PSIDCC");
    CHECK(rotated && rotated->value == "fake-sidcc-rotated");

    // ACC-07 schedule: 14 min -> nothing; 15 min -> one check
    const int before = svc.verificationsSent();
    now += 14 * 60 * 1000;
    svc.pollSchedule();
    CHECK(svc.verificationsSent() == before);
    now += 60 * 1000;
    svc.pollSchedule();
    CHECK(svc.verificationsSent() == before + 1);
    CHECK(waitFor([&] { return net.count(QStringLiteral("account/account_menu")) == 2; }));
    spin(10);

    // inconclusive (network) keeps the state
    menuMode = QStringLiteral("network");
    svc.verifyNow();
    spin(20);
    CHECK(svc.state() == SessionState::SignedIn);

    // ACC-09 switch to the brand channel: stored, caches invalidated, verified with the page id
    menuMode = QStringLiteral("accepted");
    invalidated = 0;
    CHECK(svc.selectAccount(1));
    CHECK(invalidated == 1);
    CHECK(store.selection().pageId == "pa" && store.selection().authUser == 0);
    CHECK(waitFor([&] { return svc.state() == SessionState::SignedIn; }));
    CHECK(net.sent.last().endpoint == QStringLiteral("account/account_menu"));
    CHECK(net.sent.last().header("X-Goog-PageId") == "pa");
    CHECK(net.sent.last().header("X-Goog-AuthUser") == "0");
    CHECK(!svc.selectAccount(9));

    // IMP-02 transport: the importer's identity headers win
    bool browsed = false;
    QMap<QByteArray, QByteArray> h;
    h.insert("X-Goog-AuthUser", "1");
    svc.browse(QJsonObject{{QStringLiteral("browseId"), QStringLiteral("FEchannels")}}, h, [&](bool okay, const QJsonObject &) { browsed = okay; });
    CHECK(waitFor([&] { return browsed; }));
    CHECK(net.sent.last().endpoint == QStringLiteral("browse"));
    CHECK(net.sent.last().header("X-Goog-AuthUser") == "1");
    CHECK(net.sent.last().header("X-Goog-PageId").isEmpty());
    CHECK(QString::fromUtf8(net.sent.last().request.body).contains(QStringLiteral("FEchannels")));

    // ACC-07 rejected -> expired; no periodic checks while expired
    menuMode = QStringLiteral("rejected");
    svc.verifyNow();
    CHECK(waitFor([&] { return svc.state() == SessionState::Expired; }));
    const int expiredChecks = svc.verificationsSent();
    now += 60 * 60 * 1000;
    svc.pollSchedule();
    CHECK(svc.verificationsSent() == expiredChecks);

    // ACC-01: failures while a "player" ticks: the loop keeps running
    int ticks = 0;
    QTimer player;
    QObject::connect(&player, &QTimer::timeout, [&ticks] { ++ticks; });
    player.start(5);
    menuMode = QStringLiteral("network");
    net.delayMs = 40;
    for (int i = 0; i < 3; ++i) { svc.verifyNow(); spin(50); }
    CHECK(ticks >= 20);
    player.stop();

    // ACC-10: sign-out with a verification in flight: the late answer restores nothing
    menuMode = QStringLiteral("accepted");
    net.delayMs = 50;
    svc.verifyNow();
    svc.signOut();
    CHECK(svc.state() == SessionState::SignedOut && svc.accounts().empty());
    spin(120);
    CHECK(svc.state() == SessionState::SignedOut);
    CHECK(!store.signedIn() && vault.blob.isEmpty());
    CHECK(invalidated >= 2);

    // ACC-09: a network failure on probe 0 is "unreachable", not signed out
    net.delayMs = 1;
    net.answer = [](const Sent &) { return AuthReply(); };
    CHECK(store.importSession(fakeSession()) == ImportRefusal::None);
    svc.sessionImported();
    CHECK(waitFor([&] { return svc.state() == SessionState::Unreachable; }));
    CHECK(store.signedIn());
}

class LinesParser final : public SessionTextParser {
public:
    bool parse(const QString &text, SessionCookies *out, QString *reason) override {
        // test stand-in for SailTune's parser: "name=value" lines
        for (const QString &line : text.split(QLatin1Char('\n'), QString::SkipEmptyParts)) {
            const int eq = line.indexOf(QLatin1Char('='));
            if (eq <= 0) continue;
            Cookie c; c.domain = ".youtube.com"; c.name = line.left(eq).trimmed().toStdString(); c.value = line.mid(eq + 1).trimmed().toStdString();
            out->cookies.push_back(c);
        }
        if (out->empty()) { *reason = QStringLiteral("none"); return false; }
        return true;
    }
};

static void writeBrowserDb(const QString &path, bool sessionInWalOnly, QSqlDatabase **keep) {
    QDir().mkpath(QFileInfo(path).absolutePath());
    QSqlDatabase db = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), QStringLiteral("fake-browser"));
    db.setDatabaseName(path);
    CHECK(db.open());
    QSqlQuery q(db);
    CHECK(q.exec(QStringLiteral("PRAGMA journal_mode=WAL")));
    q.exec(QStringLiteral("PRAGMA wal_autocheckpoint=0"));
    CHECK(q.exec(QStringLiteral("CREATE TABLE moz_cookies (id INTEGER PRIMARY KEY, name TEXT, value TEXT, host TEXT, path TEXT, "
                                "expiry INTEGER, isSecure INTEGER, isHttpOnly INTEGER)")));
    q.exec(QStringLiteral("PRAGMA wal_checkpoint(TRUNCATE)"));
    const auto add = [&](const char *name, const char *value, const char *host) {
        QSqlQuery ins(db);
        ins.prepare(QStringLiteral("INSERT INTO moz_cookies(name, value, host, path, expiry, isSecure, isHttpOnly) VALUES(?,?,?,'/',0,1,0)"));
        ins.addBindValue(QString::fromLatin1(name)); ins.addBindValue(QString::fromLatin1(value)); ins.addBindValue(QString::fromLatin1(host));
        CHECK(ins.exec());
    };
    add("PREF", "fake-pref", ".youtube.com");
    add("NID", "fake-nid", ".example.org"); // another site: never imported
    if (sessionInWalOnly) {
        add("SAPISID", "fake-google-sapisid", ".google.com");
        add("SAPISID", "fake-youtube-sapisid", ".youtube.com");
        add("SID", "fake-sid", ".google.com");
    }
    *keep = new QSqlDatabase(db); // keep the connection open: the rows stay in the -wal file
}

static void controller(const QString &dir) {
    // ACC-02 rank order
    FakeVault v1;
    AccountStoreOptions o1; o1.privateDir = dir + QStringLiteral("/c1"); o1.vault = &v1;
    AccountStore s1(o1); s1.open();
    FakeTransport net; net.answer = [](const Sent &) { return ok(accountMenu(QStringLiteral("Fake User"))); };
    AccountService svc1(&s1, &net);
    sailfish::AccountController::Options full;
    full.variant = BuildVariant::Full;
    full.home = dir + QStringLiteral("/home");
    full.scratchDir = dir + QStringLiteral("/c1/scratch");
    LinesParser parser;
    sailfish::AccountController fullCtl(&s1, &svc1, &parser, full);
    CHECK(fullCtl.methods() == (QStringList() << QStringLiteral("browser") << QStringLiteral("paste")));
    sailfish::AccountController::Options store = full;
    store.variant = BuildVariant::Store;
    sailfish::AccountController storeCtl(&s1, &svc1, &parser, store);
    CHECK(storeCtl.methods() == QStringList() << QStringLiteral("paste"));
    CHECK(!storeCtl.importFromBrowser());
    store.webViewBuiltIn = true;
    sailfish::AccountController webCtl(&s1, &svc1, &parser, store);
    CHECK(webCtl.methods() == (QStringList() << QStringLiteral("paste") << QStringLiteral("webview")));

    // ACC-03: no browser database yet
    CHECK(!fullCtl.importFromBrowser() && !fullCtl.lastError().isEmpty());
    // ACC-03: the sign-in exists only in the WAL; .youtube.com wins the collision
    const QString dbPath = full.home + QStringLiteral("/.local/share/org.sailfishos/browser/.mozilla/cookies.sqlite");
    QSqlDatabase *open = nullptr;
    writeBrowserDb(dbPath, true, &open);
    CHECK(QFileInfo(dbPath + QStringLiteral("-wal")).size() > 0);
    const BrowserImportResult r = BrowserCookieImport(full.home, full.scratchDir).run();
    CHECK(r.error == BrowserImportError::None);
    const Cookie *sapisid = r.cookies.find("SAPISID");
    CHECK(sapisid && sapisid->value == "fake-youtube-sapisid");
    CHECK(!r.cookies.find("NID"));
    CHECK(QDir(full.scratchDir).entryList(QDir::NoDotAndDotDot | QDir::AllEntries).isEmpty()); // the copy is gone
    CHECK(fullCtl.importFromBrowser());
    CHECK(fullCtl.hasSession());
    open->close(); delete open;
    QSqlDatabase::removeDatabase(QStringLiteral("fake-browser"));

    // ACC-04 gate through the paste path: no secret -> refused, nothing stored
    fullCtl.signOut();
    CHECK(!fullCtl.importText(QStringLiteral("PREF=fake-pref\nSID=fake-sid")));
    CHECK(!fullCtl.hasSession() && v1.blob.isEmpty() && !fullCtl.lastError().isEmpty());
    CHECK(!fullCtl.importText(QStringLiteral("not cookies")));
    CHECK(fullCtl.importText(QStringLiteral("SAPISID=fake-sapisid\nSID=fake-sid")));
    CHECK(fullCtl.hasSession());
    // a file is read the same way, with a size cap
    const QString file = dir + QStringLiteral("/cookies.txt");
    { QFile f(file); f.open(QIODevice::WriteOnly); f.write("SAPISID=fake-sapisid-2\n"); }
    CHECK(fullCtl.importFile(QStringLiteral("file://") + file));
    { QFile f(file); f.open(QIODevice::WriteOnly); f.write(QByteArray(sailfish::AccountController::maxImportFileBytes + 1, 'x')); }
    CHECK(!fullCtl.importFile(file));
    // no parser in this build: paste is refused with a message
    sailfish::AccountController noParser(&s1, &svc1, nullptr, full);
    CHECK(!noParser.importText(QStringLiteral("SAPISID=x")) && !noParser.lastError().isEmpty());
    // state text exists for every state
    CHECK(!fullCtl.stateText().isEmpty());
}


static void hooks(const QString &dir) {
    FakeVault vault;
    AccountStoreOptions options;
    options.privateDir = dir + QStringLiteral("/account");
    options.vault = &vault;
    AccountStore store(options);
    store.open();
    FakeTransport net;
    net.answer = [](const Sent &s) {
        if (s.endpoint == QLatin1String("account/account_menu")) return ok(accountMenu(QStringLiteral("Fake User")));
        if (s.endpoint == QLatin1String("account/accounts_list")) return ok(accountsList(QJsonArray()));
        AuthReply r = ok(QJsonObject{{QStringLiteral("contents"), QJsonObject()}});
        r.setCookies.push_back("SIDCC=fake-sidcc-from-home; Domain=.youtube.com; Path=/");
        return r;
    };
    AccountService svc(&store, &net);
    SignedCatalogTransport signedTransport(&svc);

    // signed out: nothing is sent, the caller gets status 0 and stays anonymous
    catalog::HttpRequest req;
    req.url = QUrl(QStringLiteral("https://www.youtube.com/youtubei/v1/browse?prettyPrint=false"));
    req.method = "POST";
    req.body = "{\"browseId\":\"FEwhat_to_watch\"}";
    req.headers.append(qMakePair(QByteArray("Content-Type"), QByteArray("application/json")));
    req.headers.append(qMakePair(QByteArray("Cookie"), QByteArray("planted=1")));
    int status = -1;
    signedTransport.send(req, nullptr, [&status](const catalog::HttpReply &r) { status = r.status; });
    CHECK(waitFor([&] { return status == 0; }));
    CHECK(net.sent.isEmpty());
    CHECK(svc.beginExtractorCall().jarPath.isEmpty());

    // signed in: the catalog's request goes out signed; planted credentials are replaced
    CHECK(store.importSession(fakeSession()) == ImportRefusal::None);
    svc.sessionImported();
    CHECK(waitFor([&] { return svc.state() == SessionState::SignedIn; }));
    CHECK(svc.canSign());
    const int before = net.sent.size();
    status = -1;
    QByteArray body;
    signedTransport.send(req, nullptr, [&](const catalog::HttpReply &r) { status = r.status; body = r.body; });
    CHECK(waitFor([&] { return status == 200; }));
    CHECK(net.sent.size() == before + 1);
    const Sent &home = net.sent.last();
    CHECK(home.endpoint == QStringLiteral("browse"));
    CHECK(home.header("Content-Type") == "application/json");
    CHECK(home.header("Authorization").startsWith("SAPISIDHASH "));
    CHECK(!home.header("Cookie").contains("planted"));
    CHECK(home.header("Cookie").contains("SAPISID=fake-sapisid"));
    CHECK(home.request.body.contains("FEwhat_to_watch"));
    CHECK(body.contains("contents"));
    { const SessionCookies s = store.session(); const Cookie *c = s.find("SIDCC"); CHECK(c && c->value == "fake-sidcc-from-home"); }
    // cancel: no callback
    bool called = false;
    const quint64 id = signedTransport.send(req, nullptr, [&called](const catalog::HttpReply &) { called = true; });
    signedTransport.cancel(id);
    spin(30);
    CHECK(!called);

    // RES-08 cookie rung: a 0600 jar per call; the extractor's rotation is folded back
    const AccountService::ExtractorCall call = svc.beginExtractorCall();
    CHECK(!call.jarPath.isEmpty());
    CHECK(call.jarPath.startsWith(options.privateDir + QLatin1Char('/')));
    CHECK((QFile::permissions(call.jarPath) & (QFileDevice::ReadGroup | QFileDevice::ReadOther | QFileDevice::WriteGroup | QFileDevice::WriteOther)) == 0);
    QFile jar(call.jarPath);
    CHECK(jar.open(QIODevice::ReadOnly));
    QByteArray text = jar.readAll();
    jar.close();
    CHECK(text.contains("fake-sapisid"));
    text.replace("fake-sidcc-from-home", "fake-sidcc-from-ytdlp");
    text.append(".youtube.com\tTRUE\t/\tTRUE\t0\tYSC\tfake-untracked\n");
    CHECK(jar.open(QIODevice::WriteOnly | QIODevice::Truncate));
    jar.write(text);
    jar.close();
    svc.endExtractorCall(call);
    CHECK(!QFileInfo(call.jarPath).exists());
    CHECK(!QFileInfo(QFileInfo(call.jarPath).absolutePath()).exists());
    { const SessionCookies s = store.session(); const Cookie *c = s.find("SIDCC"); CHECK(c && c->value == "fake-sidcc-from-ytdlp"); CHECK(!s.find("YSC")); }
    // a sign-out during the call: nothing comes back
    const AccountService::ExtractorCall late = svc.beginExtractorCall();
    CHECK(!late.jarPath.isEmpty());
    svc.signOut();
    svc.endExtractorCall(late);
    CHECK(!store.signedIn() && vault.blob.isEmpty());
    // Netscape parsing: comments skipped, #HttpOnly_ kept, expired flagged
    const std::vector<SetCookieValue> parsed = AccountService::parseNetscapeJar(
        "# Netscape HTTP Cookie File\n#HttpOnly_.youtube.com\tTRUE\t/\tTRUE\t0\tSID\tfake-sid\n"
        ".youtube.com\tTRUE\t/\tTRUE\t100\tSIDCC\tfake-old\nbroken line\n", 1000);
    CHECK(parsed.size() == 2);
    if (parsed.size() == 2) { CHECK(parsed[0].name == "SID" && !parsed[0].expired); CHECK(parsed[1].expired); }
}

// Direct checks of the remaining public surface: target filter, jar probe order,
// request context, explicit probing and sendSigned.
static void surface(const QString &dir) {
    // AuthTransport: only https://www.youtube.com on 443, no user info
    CHECK(NetworkAuthTransport::allowedTarget(QUrl(QStringLiteral("https://www.youtube.com/youtubei/v1/browse"))));
    CHECK(NetworkAuthTransport::allowedTarget(QUrl(QStringLiteral("https://www.youtube.com:443/x"))));
    CHECK(!NetworkAuthTransport::allowedTarget(QUrl(QStringLiteral("http://www.youtube.com/x"))));
    CHECK(!NetworkAuthTransport::allowedTarget(QUrl(QStringLiteral("https://m.youtube.com/x"))));
    CHECK(!NetworkAuthTransport::allowedTarget(QUrl(QStringLiteral("https://www.youtube.com.evil.example/x"))));
    CHECK(!NetworkAuthTransport::allowedTarget(QUrl(QStringLiteral("https://www.youtube.com:8443/x"))));
    CHECK(!NetworkAuthTransport::allowedTarget(QUrl(QStringLiteral("https://user:pw@www.youtube.com/x"))));

    // BrowserCookieImport::findDatabase: the ACC-03 order, first existing wins
    const QString home = dir + QStringLiteral("/home");
    BrowserCookieImport probe(home, dir + QStringLiteral("/scratch"));
    CHECK(probe.findDatabase().isEmpty());
    const QString second = home + QStringLiteral("/.mozilla/mozembed/cookies.sqlite");
    const QString third = home + QStringLiteral("/.local/share/org.sailfishos/sailfish-browser/.mozilla/cookies.sqlite");
    const QString first = home + QStringLiteral("/.local/share/org.sailfishos/browser/.mozilla/cookies.sqlite");
    for (const QString &p : { third, second }) { QDir().mkpath(QFileInfo(p).absolutePath()); QFile f(p); f.open(QIODevice::WriteOnly); }
    CHECK(probe.findDatabase() == second);
    { QDir().mkpath(QFileInfo(first).absolutePath()); QFile f(first); f.open(QIODevice::WriteOnly); }
    CHECK(probe.findDatabase() == first);

    FakeVault vault;
    AccountStoreOptions options;
    options.privateDir = dir + QStringLiteral("/account");
    options.vault = &vault;
    AccountStore store(options);
    store.open();
    FakeTransport net;
    int lists = 0;
    net.answer = [&lists](const Sent &s) {
        if (s.endpoint == QLatin1String("account/accounts_list")) {
            ++lists;
            return ok(accountsList(s.header("X-Goog-AuthUser") == "0"
                ? QJsonArray{accountItem(QStringLiteral("Only"), QStringLiteral("g0"), QString())}
                : QJsonArray()));
        }
        return ok(accountMenu(QStringLiteral("Fake User")));
    };
    AccountService svc(&store, &net);
    // setClientVersionProvider / setRequestLocale shape the request context
    svc.setClientVersionProvider([] { return QStringLiteral("2.99990101.00.00"); });
    svc.setRequestLocale(QStringLiteral("de"), QStringLiteral("AT"));

    // sendSigned while signed out: nothing is sent, status 0
    AuthRequest plain;
    plain.url = QUrl(QStringLiteral("https://www.youtube.com/youtubei/v1/next?prettyPrint=false"));
    plain.body = "{}";
    plain.headers.append(qMakePair(QByteArray("Cookie"), QByteArray("planted=1")));
    plain.headers.append(qMakePair(QByteArray("X-Goog-PageId"), QByteArray("planted-page")));
    plain.headers.append(qMakePair(QByteArray("Accept-Language"), QByteArray("de")));
    int status = -1;
    svc.sendSigned(plain, nullptr, [&status](const AuthReply &r) { status = r.status; });
    CHECK(waitFor([&] { return status == 0; }));
    CHECK(net.sent.isEmpty());

    CHECK(store.importSession(fakeSession()) == ImportRefusal::None);
    svc.sessionImported();
    CHECK(waitFor([&] { return svc.state() == SessionState::SignedIn && svc.accounts().size() == 1; }));
    const Sent &menu = net.sent.first();
    CHECK(menu.header("X-YouTube-Client-Version") == "2.99990101.00.00");
    const QJsonObject client = QJsonDocument::fromJson(menu.request.body).object().value(QStringLiteral("context")).toObject()
                                   .value(QStringLiteral("client")).toObject();
    CHECK(client.value(QStringLiteral("clientVersion")).toString() == QStringLiteral("2.99990101.00.00"));
    CHECK(client.value(QStringLiteral("hl")).toString() == QStringLiteral("de"));
    CHECK(client.value(QStringLiteral("gl")).toString() == QStringLiteral("AT"));

    // probeAccounts on demand: one more walk (0 then the empty index 1), no overlap
    const int before = lists;
    svc.probeAccounts();
    svc.probeAccounts(); // ignored while a probe runs
    CHECK(waitFor([&] { return lists == before + 2; }));
    spin(20);
    CHECK(lists == before + 2);
    CHECK(svc.accounts().size() == 1 && svc.accounts().front().name == "Only");

    // sendSigned while signed in: credentials added fresh, planted ones dropped, others kept
    status = -1;
    svc.sendSigned(plain, nullptr, [&status](const AuthReply &r) { status = r.status; });
    CHECK(waitFor([&] { return status == 200; }));
    const Sent &next = net.sent.last();
    CHECK(next.endpoint == QStringLiteral("next"));
    CHECK(next.header("Authorization").startsWith("SAPISIDHASH "));
    CHECK(!next.header("Cookie").contains("planted") && next.header("Cookie").contains("fake-sapisid"));
    CHECK(next.header("X-Goog-PageId").isEmpty());
    CHECK(next.header("Accept-Language") == "de");
}

int main(int argc, char **argv) {
    QCoreApplication app(argc, argv);
    const QString dir = QString::fromLocal8Bit(qgetenv("TEST_DIR"));
    parsers();
    service(dir + QStringLiteral("/service"));
    controller(dir + QStringLiteral("/controller"));
    hooks(dir + QStringLiteral("/hooks"));
    surface(dir + QStringLiteral("/surface"));
    std::printf("account service tests: %s (%d failures)\n", failures ? "FAIL" : "PASS", failures);
    return failures ? 1 : 0;
}
