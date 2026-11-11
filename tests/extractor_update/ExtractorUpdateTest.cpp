// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// [FULL] NFR-MNT-01 / NFR-SEC-04 host tests (Linux, Qt5Core + Qt5Network, python3): Domain/ReleasePolicy
// and Infrastructure/ExtractorUpdater against a fake HTTPS release host that answers like GitHub
// (latest/download -> releases/download/<tag> -> asset host). Logical hosts are mapped to 127.0.0.1 by
// the updater's test-only endpointOverride; TLS is verified for real against the throwaway test
// certificate (Shared/SailTune/tests/sharedplayback/test_cert.h, included, never edited).
// The verifier is Core/Extractor/Runtime/verify_release.py run with `python3 -I` (or worker.py's verify
// task), always with XUPD_MARKER_DIR set: no test may leave a marker (the archive is never imported/run).
#include "Core/Extractor/Domain/ReleasePolicy.h"
#include "Core/Extractor/Infrastructure/ExtractorUpdater.h"
#include "test_cert.h"

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDir>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMap>
#include <QMutex>
#include <QSemaphore>
#include <QThread>
#include <QProcess>
#include <QSslCertificate>
#include <QSslKey>
#include <QSslSocket>
#include <QTcpServer>
#include <QTemporaryDir>
#include <QTimer>
#include <cstdio>
#include <functional>
#include <memory>
#include <sys/stat.h>
#include <vector>

using namespace nordtile::video::extractor;

static int failures = 0;
static int cases = 0;
#define CHECK(cond) do { if (!(cond)) { std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); ++failures; } } while (0)
#define CHECK_EQ(a, b) do { const QString va_ = (a), vb_ = (b); if (va_ != vb_) { \
    std::printf("FAIL %s:%d: %s == %s (\"%s\" vs \"%s\")\n", __FILE__, __LINE__, #a, #b, qPrintable(va_), qPrintable(vb_)); ++failures; } } while (0)
#define CASE(name) do { ++cases; std::printf("extractor_update: %s\n", name); std::fflush(stdout); } while (0)

static QString repoRoot, python, fixtures, realArchive, markerDir;

static bool waitFor(const std::function<bool()> &done, int ms = 20000) {
    QElapsedTimer t; t.start();
    while (!done() && t.elapsed() < ms) QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
    return done();
}

static QByteArray readFile(const QString &path) {
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) { std::printf("cannot read %s\n", qPrintable(path)); ++failures; return QByteArray(); }
    return f.readAll();
}

static bool writeFile(const QString &path, const QByteArray &data) {
    QFile f(path);
    return f.open(QIODevice::WriteOnly | QIODevice::Truncate) && f.write(data) == data.size();
}

static int modeOf(const QString &path) {
    struct stat st;
    return ::lstat(QFile::encodeName(path).constData(), &st) == 0 ? static_cast<int>(st.st_mode & 0777) : -1;
}

static bool noMarkers() { return QDir(markerDir).entryList(QDir::Files | QDir::NoDotAndDotDot).isEmpty(); }

// ---------------------------------------------------------------------------------------------- fake host

struct Route {
    int status = 200;
    QByteArray location;
    QByteArray body;
    enum Mode { Full, HalfThenHang, HalfThenClose } mode = Full;
};

// Runs on its own thread (Hosts below): QNetworkAccessManager's HTTP thread blocks on the main thread
// during a TLS handshake, so a server on the main thread would deadlock with it.
class FakeHost : public QTcpServer {
public:
    struct Seen { QByteArray method, path; };
    void setRoute(const QByteArray &path, const Route &route) { QMutexLocker l(&mutex_); routes_.insert(path, route); }
    Route route(const QByteArray &path) const { QMutexLocker l(&mutex_); return routes_.value(path); }
    void editRoute(const QByteArray &path, const std::function<void(Route &)> &edit) { QMutexLocker l(&mutex_); edit(routes_[path]); }
    std::vector<Seen> seen() const { QMutexLocker l(&mutex_); return seen_; }
    int handshakes() const { QMutexLocker l(&mutex_); return handshakes_; }
    int count(const QByteArray &prefix) const {
        QMutexLocker l(&mutex_);
        int n = 0;
        for (const Seen &s : seen_) if (s.path.startsWith(prefix)) ++n;
        return n;
    }

protected:
    void incomingConnection(qintptr fd) override {
        QSslSocket *socket = new QSslSocket(this);
        socket->setSocketDescriptor(fd);
        socket->setLocalCertificate(QSslCertificate(QByteArray(kTestCertPem)));
        socket->setPrivateKey(QSslKey(QByteArray(kTestKeyPem), QSsl::Rsa));
        std::shared_ptr<QByteArray> buffer(new QByteArray);
        connect(socket, &QSslSocket::encrypted, this, [this]() { QMutexLocker l(&mutex_); ++handshakes_; });
        connect(socket, &QIODevice::readyRead, this, [this, socket, buffer]() {
            buffer->append(socket->readAll());
            for (;;) {
                const int end = buffer->indexOf("\r\n\r\n");
                if (end < 0) return;
                const QList<QByteArray> lines = buffer->left(end).split('\n');
                buffer->remove(0, end + 4);
                const QList<QByteArray> first = lines.value(0).trimmed().split(' ');
                Seen s;
                s.method = first.value(0);
                s.path = first.value(1);
                const QByteArray path = s.path.split('?').value(0);
                bool known;
                Route r;
                {
                    QMutexLocker l(&mutex_);
                    seen_.push_back(s);
                    known = routes_.contains(path);
                    r = routes_.value(path);
                }
                if (!known) { answer(socket, s.method, 404, QByteArray(), "not found"); continue; }
                if (r.mode == Route::Full || s.method == "HEAD") { answer(socket, s.method, r.status, r.location, r.body); continue; }
                QByteArray out = "HTTP/1.1 200 OK\r\nContent-Length: " + QByteArray::number(r.body.size()) + "\r\n\r\n";
                out += r.body.left(r.body.size() / 2);
                socket->write(out);
                socket->flush();
                if (r.mode == Route::HalfThenClose) QTimer::singleShot(100, socket, [socket]() { socket->abort(); });
                return;   // HalfThenHang: nothing more on this connection
            }
        });
        connect(socket, &QAbstractSocket::disconnected, socket, &QObject::deleteLater);
        socket->startServerEncryption();
    }

private:
    void answer(QSslSocket *socket, const QByteArray &method, int status, const QByteArray &location, const QByteArray &body) {
        QByteArray out = "HTTP/1.1 " + QByteArray::number(status) + " X\r\n";
        if (!location.isEmpty()) out += "Location: " + location + "\r\n";
        out += "Content-Type: application/octet-stream\r\nContent-Length: " + QByteArray::number(body.size()) + "\r\n\r\n";
        if (method != "HEAD") out += body;
        socket->write(out);
    }
    mutable QMutex mutex_;
    QMap<QByteArray, Route> routes_;   // path without query
    std::vector<Seen> seen_;
    int handshakes_ = 0;
};

// Runs `work` on `context`'s thread and waits for it.
static void onThread(QObject *context, const std::function<void()> &work) {
    QSemaphore done;
    QTimer::singleShot(0, context, [&]() { work(); done.release(); });
    done.acquire();
}

struct Hosts {
    QThread thread;
    QObject *context = nullptr;
    FakeHost *githubHost = nullptr, *assetsHost = nullptr, *evilHost = nullptr;
    quint16 githubPort = 0, assetsPort = 0, evilPort = 0;
    int assetCounter = 0;
    Hosts() {
        thread.start();
        context = new QObject;
        context->moveToThread(&thread);
        onThread(context, [this]() {
            githubHost = new FakeHost; assetsHost = new FakeHost; evilHost = new FakeHost;
            githubHost->listen(QHostAddress::LocalHost, 0); assetsHost->listen(QHostAddress::LocalHost, 0); evilHost->listen(QHostAddress::LocalHost, 0);
            githubPort = githubHost->serverPort(); assetsPort = assetsHost->serverPort(); evilPort = evilHost->serverPort();
        });
    }
    ~Hosts() {
        onThread(context, [this]() { delete githubHost; delete assetsHost; delete evilHost; });
        context->deleteLater();
        thread.quit();
        thread.wait();
    }
    FakeHost &github() { return *githubHost; }
    FakeHost &assets() { return *assetsHost; }
    FakeHost &evil() { return *evilHost; }

    // Publishes one release directory of fixtures (yt-dlp, SHA2-256SUMS, SHA2-256SUMS.sig) under `tag`.
    void publish(Channel channel, const QString &tag, const QString &dir, bool latest = true,
                 const char *assetHost = "release-assets.githubusercontent.com") {
        const QByteArray repo = QByteArray("/") + repository(channel);
        for (const char *file : {kAssetName, kSumsName, kSignatureName}) {
            const QString local = dir + QLatin1Char('/') + QString::fromLatin1(file);
            const QByteArray assetPath = "/github-production-release-asset/307260205/" + QByteArray::number(++assetCounter);
            Route tagged;
            tagged.status = 302;
            tagged.location = QByteArray("https://") + assetHost + assetPath + "?sp=r&sv=2018-11-09&sig=FAKE";
            github().setRoute(repo + "/releases/download/" + tag.toLatin1() + "/" + file, tagged);
            Route asset;
            if (QFileInfo(local).isFile()) asset.body = readFile(local);
            else asset.status = 404;
            assets().setRoute(assetPath, asset);
            if (latest) {
                Route l;
                l.status = 302;
                l.location = "https://github.com" + repo + "/releases/download/" + tag.toLatin1() + "/" + file;
                github().setRoute(repo + "/releases/latest/download/" + file, l);
            }
        }
    }
    // The asset-host path the tagged URL of `file` redirects to.
    QByteArray assetPathOf(Channel channel, const QString &tag, const char *file) {
        const Route r = github().route(QByteArray("/") + repository(channel) + "/releases/download/" + tag.toLatin1() + "/" + file);
        return r.location.mid(r.location.indexOf('/', 8)).split('?').value(0);
    }
};

static QString fakeDir(const char *name) { return fixtures + QStringLiteral("/fake/") + QString::fromLatin1(name); }
static QString testFingerprint() { return QString::fromLatin1(readFile(fixtures + QStringLiteral("/fake/test-key.fpr")).trimmed()); }

static Verifier cliVerifier() {
    ProcessVerifierOptions o;
    o.program = python;
    o.arguments << QStringLiteral("-I") << repoRoot + QStringLiteral("/Core/Extractor/Runtime/verify_release.py")
                << QStringLiteral("--key") << fixtures + QStringLiteral("/fake/test-key.asc")
                << QStringLiteral("--fingerprint") << testFingerprint();
    o.workerTask = false;
    o.environment.insert(QStringLiteral("XUPD_MARKER_DIR"), markerDir);
    o.timeoutMs = 60000;
    return processVerifier(o);
}

// worker.py's verify task (as the app binary runs it): runtimeDir holds worker.py, verify_release.py and a key.
static Verifier workerVerifier(const QString &runtimeDir) {
    ProcessVerifierOptions o;
    o.program = python;
    o.arguments << QStringLiteral("-I") << runtimeDir + QStringLiteral("/worker.py");
    o.workerTask = true;
    o.environment.insert(QStringLiteral("XUPD_MARKER_DIR"), markerDir);
    o.timeoutMs = 60000;
    return processVerifier(o);
}

struct Rig {
    Hosts hosts;
    QTemporaryDir dir;
    QString root, bundled;
    qint64 clock = 1794100000000LL;
    UpdaterOptions options(bool trustTestCa = true) {
        UpdaterOptions o;
        o.root = root;
        o.bundledArchive = bundled;
        o.verifier = cliVerifier();
        o.requestTimeoutMs = 8000;
        o.clockMs = [this]() { return clock; };
        Hosts *h = &hosts;
        o.endpointOverride = [h](const QString &host, QString *connectHost, quint16 *port) {
            *connectHost = QStringLiteral("127.0.0.1");
            if (host == QLatin1String("github.com")) *port = h->githubPort;
            else if (isAssetHost(host.toStdString())) *port = h->assetsPort;
            else *port = h->evilPort;
            return true;
        };
        if (trustTestCa) o.extraCaCertificatesForTesting << QSslCertificate(QByteArray(kTestCertPem));
        return o;
    }
    Rig() {
        root = dir.path() + QStringLiteral("/extractor");
        bundled = dir.path() + QStringLiteral("/bundled/yt-dlp.pyz");
        QDir().mkpath(dir.path() + QStringLiteral("/bundled"));
        writeFile(bundled, "bundled archive stand-in");
    }
};

static bool settle(ExtractorUpdater &u) {
    const bool ok = waitFor([&]() {
        const QString s = u.state();
        return s != QLatin1String("checking") && s != QLatin1String("downloading")
            && s != QLatin1String("verifying") && s != QLatin1String("installing");
    }, 60000);
    if (!ok) std::printf("extractor_update:   still %s after 60 s\n", qPrintable(u.state()));
    return ok;
}

static bool checkAndInstall(ExtractorUpdater &u, const QString &channel) {
    u.check(channel);
    if (!settle(u) || u.state() != QLatin1String("offered")) return false;
    u.install(u.offeredVersion(), u.offeredSize(), u.offeredSource());
    return settle(u) && u.state() == QLatin1String("installed");
}

static Manifest manifestOf(const QString &path) {
    Manifest m;
    const QJsonObject o = QJsonDocument::fromJson(readFile(path)).object();
    m.version = o.value(QStringLiteral("version")).toString().toStdString();
    m.sha256 = o.value(QStringLiteral("sha256")).toString().toStdString();
    m.source = o.value(QStringLiteral("source")).toString().toStdString();
    m.channel = o.value(QStringLiteral("channel")).toString().toStdString();
    m.size = static_cast<long long>(o.value(QStringLiteral("size")).toDouble(-1));
    m.installedAtMs = static_cast<long long>(o.value(QStringLiteral("installedAtMs")).toDouble(-1));
    return m;
}

// ---------------------------------------------------------------------------------------------- domain

static void domainTests() {
    CASE("domain_urls_and_hops");
    {   // small helpers, named so the coverage ratchet sees them
        Channel c = Channel::Stable;
        CHECK(parseChannel("nightly", &c) && c == Channel::Nightly);
        CHECK(!parseChannel("Nightly", &c) && !parseChannel("", &c));
        CHECK(std::string(refusalLabel(Refusal::InsecureUrl)) == "insecure-url");
        CHECK(std::string(refusalLabel(Refusal::HostNotAllowed)) == "host-not-allowed");
        const Layout l = layoutFor("/x/extractor");
        CHECK(l.root == "/x/extractor" && l.archive(l.current) == l.current + "/yt-dlp.pyz" && l.current != l.previous);
        CHECK(revertPlan(false).empty());
        const std::vector<Step> revert = revertPlan(true);
        CHECK(!revert.empty() && std::string(stepName(revert.front())).size() > 0);
        CHECK(std::string(stepName(Step::StagingToCurrent)).find("taging") != std::string::npos);
    }
    Url u;
    CHECK(parseUrl("https://GitHub.com/yt-dlp/yt-dlp/releases?x=1#f", &u) && u.host == "github.com" && u.query == "x=1");
    CHECK(parseUrl("https://user@github.com/", &u) && u.userInfo);
    CHECK(!parseUrl("https://[::1]/", &u));
    CHECK(!parseUrl("https://git hub.com/", &u));
    CHECK(!parseUrl("github.com/x", &u));
    CHECK(resolveLocation("https://github.com/a/b", "/c/d") == "https://github.com/c/d");
    CHECK(resolveLocation("https://github.com/a/b", "//evil.example/x").empty());
    CHECK(resolveLocation("https://github.com/a/b", "c/d").empty());
    {
        RedirectChain c(Channel::Stable, kAssetName);
        CHECK(c.next("http://github.com/yt-dlp/yt-dlp/releases/latest/download/yt-dlp") == Refusal::InsecureUrl);
    }
    {
        RedirectChain c(Channel::Stable, kAssetName);
        CHECK(c.next("https://github.com:8443/yt-dlp/yt-dlp/releases/latest/download/yt-dlp") == Refusal::InsecureUrl);
    }
    {
        RedirectChain c(Channel::Stable, kAssetName);
        CHECK(c.next("https://x@github.com/yt-dlp/yt-dlp/releases/latest/download/yt-dlp") == Refusal::InsecureUrl);
    }
    {
        RedirectChain c(Channel::Stable, kAssetName);
        CHECK(c.next(latestUrl(Channel::Stable, kAssetName)) == Refusal::None);
        CHECK(!c.finalOk());
        CHECK(c.next("https://github.com/yt-dlp/yt-dlp/releases/download/2026.10.26/yt-dlp") == Refusal::None);
        CHECK(c.tag() == "2026.10.26" && c.finalOk());
        CHECK(c.next("https://release-assets.githubusercontent.com/github-production-release-asset/1/2?sp=r") == Refusal::None);
        CHECK(c.next("https://objects.githubusercontent.com/x") == Refusal::UnexpectedRedirect);
    }
    {
        RedirectChain c(Channel::Stable, kAssetName);
        c.next(latestUrl(Channel::Stable, kAssetName));
        CHECK(c.next("https://github.com/yt-dlp/yt-dlp-nightly-builds/releases/download/2026.10.26/yt-dlp") == Refusal::BadReleasePath);
    }
    {
        RedirectChain c(Channel::Stable, kAssetName);
        c.next(latestUrl(Channel::Stable, kAssetName));
        CHECK(c.next("https://github.com/yt-dlp/yt-dlp/releases/download/latest/yt-dlp") == Refusal::BadReleasePath);
    }
    {
        RedirectChain c(Channel::Stable, kAssetName);
        c.next(latestUrl(Channel::Stable, kAssetName));
        CHECK(c.next("https://github.com/yt-dlp/yt-dlp/releases/download/2026.10.26/yt-dlp.exe") == Refusal::BadReleasePath);
    }
    {
        RedirectChain c(Channel::Stable, kAssetName);
        c.next(latestUrl(Channel::Stable, kAssetName));
        CHECK(c.next("https://objects.githubusercontent.com/x") == Refusal::UnexpectedRedirect);   // tag unknown
        RedirectChain d(Channel::Stable, kAssetName);
        d.next(latestUrl(Channel::Stable, kAssetName));
        CHECK(d.next("https://evil.example/yt-dlp") == Refusal::HostNotAllowed);
        RedirectChain e(Channel::Stable, kAssetName);
        e.next(latestUrl(Channel::Stable, kAssetName));
        CHECK(e.next("https://objects.githubusercontent.com.evil.example/x") == Refusal::HostNotAllowed);
    }
    {
        RedirectChain c(Channel::Nightly, kSumsName, "2026.11.04.232659");
        CHECK(c.next(latestUrl(Channel::Nightly, kSumsName)) == Refusal::BadReleasePath);   // a pinned tag never uses latest
        RedirectChain d(Channel::Nightly, kSumsName, "2026.11.04.232659");
        CHECK(d.next(taggedUrl(Channel::Nightly, "2026.11.05.000000", kSumsName)) == Refusal::BadReleasePath);
        RedirectChain e(Channel::Nightly, kSumsName, "2026.11.04.232659");
        CHECK(e.next(taggedUrl(Channel::Nightly, "2026.11.04.232659", kSumsName)) == Refusal::None);
    }
    {
        RedirectChain c(Channel::Stable, kAssetName);
        c.next(latestUrl(Channel::Stable, kAssetName));
        c.next(taggedUrl(Channel::Stable, "2026.10.26", kAssetName));
        CHECK(c.next(taggedUrl(Channel::Stable, "2026.10.26", kAssetName)) == Refusal::UnexpectedRedirect);
    }

    CASE("domain_versions_sums_manifest");
    CHECK(validVersion("2026.10.26") && validVersion("2026.11.04.232659") && !validVersion("2026.1.26")
          && !validVersion("latest") && !validVersion("2026.10.26.1234567") && !validVersion("v2026.10.26"));
    CHECK(compareVersions("2026.11.04.232659", "2026.11.04") > 0 && compareVersions("2026.10.26", "2026.11.01") < 0
          && compareVersions("2026.10.26", "2026.10.26") == 0);
    CHECK(worthOffering("2026.11.01", "2026.10.26") && !worthOffering("2026.10.26", "2026.10.26")
          && !worthOffering("2026.10.01", "2026.10.26") && worthOffering("2026.10.01", ""));
    const std::string a(64, 'a'), b(64, 'b');
    CHECK(sumsEntry(a + "  yt-dlp_linux\n" + b + "  yt-dlp\n", "yt-dlp") == b);
    CHECK(sumsEntry(a + "  yt-dlp\n" + b + " *yt-dlp\n", "yt-dlp").empty());
    CHECK(sumsEntry(a + "  yt-dlp.exe\n", "yt-dlp").empty());
    CHECK(sumsEntry(std::string(64, 'A') + "  yt-dlp\r\n", "yt-dlp") == a);
    Manifest m;
    m.version = "2026.10.26"; m.sha256 = a; m.size = 3073072; m.channel = "stable"; m.installedAtMs = 1;
    m.source = taggedUrl(Channel::Stable, "2026.10.26", kAssetName);
    CHECK(manifestValid(m));
    Manifest bad = m; bad.source = taggedUrl(Channel::Nightly, "2026.10.26", kAssetName);
    CHECK(!manifestValid(bad));
    bad = m; bad.size = kMaxArchiveBytes + 1;
    CHECK(!manifestValid(bad));
    bad = m; bad.sha256 = std::string(64, 'A');
    CHECK(!manifestValid(bad));

    CASE("domain_active_token_plans");
    InstalledCopy c;
    c.manifestReadable = true; c.manifest = m; c.archivePresent = true; c.archiveSha256 = a; c.archiveSize = 3073072;
    CHECK(chooseActive(c, "") == Active::Current);
    CHECK(chooseActive(c, "2026.10.26") == Active::Current);
    CHECK(chooseActive(c, "2026.11.01") == Active::Bundled);   // the app update brought a newer one
    InstalledCopy damaged = c; damaged.archiveSha256 = b;
    CHECK(chooseActive(damaged, "") == Active::Bundled);
    damaged = c; damaged.archiveSize = 1;
    CHECK(chooseActive(damaged, "") == Active::Bundled);
    damaged = c; damaged.manifestReadable = false;
    CHECK(chooseActive(damaged, "") == Active::Bundled);
    Offer o;
    o.version = "2026.11.01"; o.size = 5000; o.source = taggedUrl(Channel::Stable, "2026.11.01", kAssetName); o.offeredAtMs = 1000;
    CHECK(checkToken(nullptr, o.version, o.size, o.source, 1000) == TokenVerdict::NoOffer);
    CHECK(checkToken(&o, o.version, o.size, o.source, 2000) == TokenVerdict::Ok);
    CHECK(checkToken(&o, o.version, o.size + 1, o.source, 2000) == TokenVerdict::Mismatch);
    CHECK(checkToken(&o, "2026.11.02", o.size, o.source, 2000) == TokenVerdict::Mismatch);
    CHECK(checkToken(&o, o.version, o.size, o.source + "x", 2000) == TokenVerdict::Mismatch);
    CHECK(checkToken(&o, o.version, o.size, o.source, 1000 + kOfferLifetimeMs + 1) == TokenVerdict::Expired);
    const std::vector<Step> install = installPlan(true, true);
    CHECK(install.size() == 5 && install[1] == Step::PreviousToTrash && install[2] == Step::CurrentToPrevious
          && install[3] == Step::StagingToCurrent);
    CHECK(installPlan(false, false).size() == 3);
    CHECK(rollbackPlan(true, false).empty());
    CHECK(rollbackPlan(true, true).size() == 4);
    const std::vector<Step> r1 = recoveryPlan(false, true, false);
    CHECK(r1.size() == 3 && r1[0] == Step::PreviousToCurrent);
    const std::vector<Step> r2 = recoveryPlan(false, true, true);
    CHECK(r2.size() == 3 && r2[0] == Step::SwapToCurrent && r2[1] == Step::ClearStaging);
    const std::vector<Step> r3 = recoveryPlan(true, false, true);
    CHECK(r3[0] == Step::SwapToPrevious && r3[1] == Step::ClearStaging);
    CHECK(recoveryPlan(true, true, false).size() == 2);
}

// ---------------------------------------------------------------------------------------------- updater

static void happyPaths() {
    CASE("happy_path_stable");
    {
        Rig rig;
        rig.hosts.publish(Channel::Stable, QStringLiteral("2026.11.01"), fakeDir("stable-2026.11.01"));
        ExtractorUpdater u(rig.options());
        CHECK(u.usingBundled() && u.activeArchive() == rig.bundled && u.installedVersion().isEmpty());
        QString offeredVersion, offeredSource;
        qint64 offeredSize = -1;
        QObject::connect(&u, &ExtractorUpdater::offered, [&](const QString &v, qint64 s, const QString &src) {
            offeredVersion = v; offeredSize = s; offeredSource = src;
        });
        u.check(QStringLiteral("stable"));
        CHECK(settle(u));
        CHECK_EQ(u.state(), QStringLiteral("offered"));
        const qint64 size = QFileInfo(fakeDir("stable-2026.11.01") + QStringLiteral("/yt-dlp")).size();
        CHECK_EQ(offeredVersion, QStringLiteral("2026.11.01"));
        CHECK(offeredSize == size && u.offeredSize() == size);
        CHECK_EQ(offeredSource, QStringLiteral("https://github.com/yt-dlp/yt-dlp/releases/download/2026.11.01/yt-dlp"));
        CHECK(rig.hosts.github().count("/yt-dlp/yt-dlp/releases/latest/download/yt-dlp") == 1);
        CHECK(rig.hosts.github().seen().size() == 4);   // HEAD latest, HEAD tagged, GET sums, GET sig (tagged)
        CHECK(!QFileInfo(rig.root + QStringLiteral("/staging")).exists());   // a check writes nothing
        u.install(u.offeredVersion(), u.offeredSize(), u.offeredSource());
        CHECK(settle(u));
        CHECK_EQ(u.state(), QStringLiteral("installed"));
        CHECK_EQ(u.lastError(), QString());
        CHECK_EQ(u.installedVersion(), QStringLiteral("2026.11.01"));
        CHECK(!u.usingBundled() && u.restartNeeded());
        const QString current = rig.root + QStringLiteral("/current");
        CHECK_EQ(u.activeArchive(), current + QStringLiteral("/yt-dlp.pyz"));
        CHECK(readFile(u.activeArchive()) == readFile(fakeDir("stable-2026.11.01") + QStringLiteral("/yt-dlp")));
        CHECK(modeOf(rig.root) == 0700 && modeOf(current) == 0700);
        CHECK(modeOf(current + QStringLiteral("/yt-dlp.pyz")) == 0600 && modeOf(current + QStringLiteral("/manifest.json")) == 0600);
        const Manifest m = manifestOf(current + QStringLiteral("/manifest.json"));
        CHECK(manifestValid(m) && m.version == "2026.11.01" && m.channel == "stable" && m.installedAtMs == rig.clock);
        CHECK(!QFileInfo(rig.root + QStringLiteral("/staging")).exists() && !QFileInfo(rig.root + QStringLiteral("/previous")).exists());
        CHECK(noMarkers());
        // A fresh start keeps the verified current.
        ExtractorUpdater again(rig.options());
        CHECK(!again.usingBundled() && again.activeArchive(QStringLiteral("/x")) == current + QStringLiteral("/yt-dlp.pyz"));
        again.check(QStringLiteral("stable"));
        CHECK(settle(again));
        CHECK_EQ(again.state(), QStringLiteral("upToDate"));
        CHECK(again.install(QStringLiteral("2026.11.01"), size, offeredSource) == false);
        CHECK_EQ(again.lastError(), QStringLiteral("no-offer"));
    }

    CASE("happy_path_nightly");
    {
        Rig rig;
        rig.hosts.publish(Channel::Nightly, QStringLiteral("2026.11.04.232659"), fakeDir("nightly-2026.11.04.232659"), true,
                          "objects.githubusercontent.com");
        ExtractorUpdater u(rig.options());
        CHECK(checkAndInstall(u, QStringLiteral("nightly")));
        CHECK_EQ(u.installedVersion(), QStringLiteral("2026.11.04.232659"));
        CHECK(manifestOf(rig.root + QStringLiteral("/current/manifest.json")).channel == "nightly");
        CHECK(rig.hosts.github().count("/yt-dlp/yt-dlp-nightly-builds/") >= 4 && rig.hosts.github().count("/yt-dlp/yt-dlp/") == 0);
        CHECK(noMarkers());
    }
}

static void refusals() {
    CASE("http_url_refused");
    {
        Rig rig;
        rig.hosts.publish(Channel::Stable, QStringLiteral("2026.11.01"), fakeDir("stable-2026.11.01"));
        Route r; r.status = 302; r.location = "http://github.com/yt-dlp/yt-dlp/releases/download/2026.11.01/yt-dlp";
        rig.hosts.github().setRoute("/yt-dlp/yt-dlp/releases/latest/download/yt-dlp", r);
        ExtractorUpdater u(rig.options());
        u.check(QStringLiteral("stable"));
        CHECK(settle(u));
        CHECK_EQ(u.lastError(), QStringLiteral("insecure-url"));
        CHECK(rig.hosts.github().seen().size() == 1);   // the http hop was never requested
        // An http (and a non-443) asset hop too.
        Route t; t.status = 302; t.location = "http://release-assets.githubusercontent.com/x";
        rig.hosts.github().setRoute("/yt-dlp/yt-dlp/releases/latest/download/yt-dlp",
                                       rig.hosts.github().route("/yt-dlp/yt-dlp/releases/latest/download/SHA2-256SUMS"));
        Route latest; latest.status = 302; latest.location = "https://github.com/yt-dlp/yt-dlp/releases/download/2026.11.01/yt-dlp";
        rig.hosts.github().setRoute("/yt-dlp/yt-dlp/releases/latest/download/yt-dlp", latest);
        rig.hosts.github().setRoute("/yt-dlp/yt-dlp/releases/download/2026.11.01/yt-dlp", t);
        u.check(QStringLiteral("stable"));
        CHECK(settle(u));
        CHECK_EQ(u.lastError(), QStringLiteral("insecure-url"));
        t.location = "https://release-assets.githubusercontent.com:8443/x";
        rig.hosts.github().setRoute("/yt-dlp/yt-dlp/releases/download/2026.11.01/yt-dlp", t);
        u.check(QStringLiteral("stable"));
        CHECK(settle(u));
        CHECK_EQ(u.lastError(), QStringLiteral("insecure-url"));
        CHECK(rig.hosts.assets().seen().empty() && rig.hosts.evil().seen().empty());
        CHECK(u.usingBundled() && u.offeredVersion().isEmpty());
    }

    CASE("redirect_to_unlisted_host_refused");
    {
        Rig rig;
        rig.hosts.publish(Channel::Stable, QStringLiteral("2026.11.01"), fakeDir("stable-2026.11.01"));
        Route t; t.status = 302; t.location = "https://downloads.evil.example/yt-dlp";
        rig.hosts.github().setRoute("/yt-dlp/yt-dlp/releases/download/2026.11.01/yt-dlp", t);
        ExtractorUpdater u(rig.options());
        u.check(QStringLiteral("stable"));
        CHECK(settle(u));
        CHECK_EQ(u.lastError(), QStringLiteral("host-not-allowed"));
        CHECK(rig.hosts.evil().seen().empty() && rig.hosts.evil().handshakes() == 0);
        // A redirect from the asset host is refused too.
        Rig rig2;
        rig2.hosts.publish(Channel::Stable, QStringLiteral("2026.11.01"), fakeDir("stable-2026.11.01"));
        Route bounce; bounce.status = 302; bounce.location = "https://objects.githubusercontent.com/again";
        rig2.hosts.assets().setRoute(rig2.hosts.assetPathOf(Channel::Stable, QStringLiteral("2026.11.01"), kAssetName), bounce);
        ExtractorUpdater u2(rig2.options());
        u2.check(QStringLiteral("stable"));
        CHECK(settle(u2));
        CHECK_EQ(u2.lastError(), QStringLiteral("unexpected-redirect"));
    }

    CASE("invalid_certificate_refused");
    {
        Rig rig;
        rig.hosts.publish(Channel::Stable, QStringLiteral("2026.11.01"), fakeDir("stable-2026.11.01"));
        ExtractorUpdater u(rig.options(false));   // the throwaway certificate is not trusted
        u.check(QStringLiteral("stable"));
        CHECK(settle(u));
        CHECK_EQ(u.state(), QStringLiteral("failed"));
        CHECK_EQ(u.lastError(), QStringLiteral("certificate"));
        CHECK(rig.hosts.github().seen().empty());   // no request was sent over the unverified connection
    }

    CASE("missing_signature_refused");
    {
        Rig rig;
        rig.hosts.publish(Channel::Stable, QStringLiteral("2026.11.01"), fakeDir("stable-2026.11.01"));
        rig.hosts.assets().editRoute(rig.hosts.assetPathOf(Channel::Stable, QStringLiteral("2026.11.01"), kSignatureName), [](Route &r) { r.status = 404; });
        ExtractorUpdater u(rig.options());
        u.check(QStringLiteral("stable"));
        CHECK(settle(u));
        CHECK_EQ(u.lastError(), QStringLiteral("signature-missing"));
        rig.hosts.assets().setRoute(rig.hosts.assetPathOf(Channel::Stable, QStringLiteral("2026.11.01"), kSignatureName), Route());
        u.check(QStringLiteral("stable"));
        CHECK(settle(u));
        CHECK_EQ(u.lastError(), QStringLiteral("signature-missing"));   // an empty .sig
        CHECK(u.offeredVersion().isEmpty());
    }

    // Installs `releaseDir` (published as stable `tag`) over a working 2026.11.01 and expects `label`.
    // tamper runs before the check, or between the offer and the install (afterOffer).
    auto refusedOverWorking = [](const char *name, const QString &tag, const QString &releaseDir, const QString &label,
                                 const std::function<void(Rig &)> &tamper, bool afterOffer = false) {
        CASE(name);
        Rig rig;
        rig.hosts.publish(Channel::Stable, QStringLiteral("2026.11.01"), fakeDir("stable-2026.11.01"));
        {
            ExtractorUpdater first(rig.options());
            CHECK(checkAndInstall(first, QStringLiteral("stable")));
        }
        const QByteArray working = readFile(rig.root + QStringLiteral("/current/yt-dlp.pyz"));
        rig.hosts.publish(Channel::Stable, tag, releaseDir);
        if (tamper && !afterOffer) tamper(rig);
        ExtractorUpdater u(rig.options());
        u.check(QStringLiteral("stable"));
        CHECK(settle(u));
        if (tamper && afterOffer) tamper(rig);
        if (u.state() == QLatin1String("offered")) {
            u.install(u.offeredVersion(), u.offeredSize(), u.offeredSource());
            CHECK(settle(u));
        }
        CHECK_EQ(u.state(), QStringLiteral("failed"));
        CHECK_EQ(u.lastError(), label);
        CHECK_EQ(u.installedVersion(), QStringLiteral("2026.11.01"));
        CHECK(readFile(u.activeArchive()) == working && !u.usingBundled());
        CHECK(!QFileInfo(rig.root + QStringLiteral("/staging")).exists());
        CHECK(!QFileInfo(rig.root + QStringLiteral("/previous")).exists());
        CHECK(noMarkers());
        ExtractorUpdater restarted(rig.options());
        CHECK_EQ(restarted.installedVersion(), QStringLiteral("2026.11.01"));
        CHECK(!restarted.usingBundled());
    };

    refusedOverWorking("wrong_sha256_refused", QStringLiteral("2026.11.20"), fakeDir("stable-2026.11.20"), QStringLiteral("checksum"),
                       [](Rig &rig) {
        rig.hosts.assets().editRoute(rig.hosts.assetPathOf(Channel::Stable, QStringLiteral("2026.11.20"), kAssetName), [](Route &r) {
            r.body[r.body.size() / 2] = static_cast<char>(r.body.at(r.body.size() / 2) ^ 0x01);   // same size, other bytes
        });
    });
    refusedOverWorking("bad_signature_wrong_key_refused", QStringLiteral("2026.11.21"), fakeDir("wrongkey-2026.11.01"),
                       QStringLiteral("signature-issuer"), nullptr);
    refusedOverWorking("bad_signature_tampered_sums_refused", QStringLiteral("2026.11.20"), fakeDir("stable-2026.11.20"),
                       QStringLiteral("signature"), [](Rig &rig) {
        rig.hosts.assets().editRoute(rig.hosts.assetPathOf(Channel::Stable, QStringLiteral("2026.11.20"), kSumsName), [](Route &r) {
            r.body.replace("yt-dlp.exe", "yt-dlp.exf");   // the archive's own line still matches
        });
    });
    refusedOverWorking("bad_signature_eddsa_refused", QStringLiteral("2026.11.21"), fakeDir("ed25519-2026.11.01"),
                       QStringLiteral("signature-algorithm"), nullptr);
    refusedOverWorking("archive_dotdot_refused", QStringLiteral("2026.11.02"), fakeDir("dotdot-2026.11.02"),
                       QStringLiteral("archive-entry"), nullptr);
    refusedOverWorking("archive_absolute_refused", QStringLiteral("2026.11.02"), fakeDir("absolute-2026.11.02"),
                       QStringLiteral("archive-entry"), nullptr);
    refusedOverWorking("archive_version_mismatch_refused", QStringLiteral("2026.11.03"), fakeDir("mismatch-2026.11.03"),
                       QStringLiteral("version-mismatch"), nullptr);
    refusedOverWorking("archive_grew_after_offer_refused", QStringLiteral("2026.11.20"), fakeDir("stable-2026.11.20"),
                       QStringLiteral("too-large"), [](Rig &rig) {
        // The HEAD answered the offered size; by the install the body is longer: the download stops at the offered size.
        const QByteArray path = rig.hosts.assetPathOf(Channel::Stable, QStringLiteral("2026.11.20"), kAssetName);
        rig.hosts.assets().editRoute(path, [](Route &r) { r.body += QByteArray(4096, 'x'); });
    }, true);

    CASE("oversize_refused");
    {
        Rig rig;
        rig.hosts.publish(Channel::Stable, QStringLiteral("2026.11.01"), fakeDir("stable-2026.11.01"));
        UpdaterOptions o = rig.options();
        o.maxArchiveBytes = 3000;
        ExtractorUpdater u(o);
        u.check(QStringLiteral("stable"));
        CHECK(settle(u));
        CHECK_EQ(u.lastError(), QStringLiteral("too-large"));
        CHECK(u.offeredVersion().isEmpty());
    }

    CASE("install_without_offer_or_mismatched_token_refused");
    {
        Rig rig;
        rig.hosts.publish(Channel::Stable, QStringLiteral("2026.11.01"), fakeDir("stable-2026.11.01"));
        ExtractorUpdater u(rig.options());
        const QString source = QStringLiteral("https://github.com/yt-dlp/yt-dlp/releases/download/2026.11.01/yt-dlp");
        const qint64 size = QFileInfo(fakeDir("stable-2026.11.01") + QStringLiteral("/yt-dlp")).size();
        CHECK(!u.install(QStringLiteral("2026.11.01"), size, source));
        CHECK_EQ(u.lastError(), QStringLiteral("no-offer"));
        CHECK(rig.hosts.github().seen().empty());
        u.check(QStringLiteral("stable"));
        CHECK(settle(u) && u.state() == QLatin1String("offered"));
        CHECK(!u.install(QStringLiteral("2026.11.02"), size, source));
        CHECK_EQ(u.lastError(), QStringLiteral("token-mismatch"));
        CHECK(!u.install(QStringLiteral("2026.11.01"), size + 1, source));
        CHECK_EQ(u.lastError(), QStringLiteral("token-mismatch"));
        CHECK(!u.install(QStringLiteral("2026.11.01"), size, QStringLiteral("https://github.com/yt-dlp/yt-dlp-nightly-builds/releases/download/2026.11.01/yt-dlp")));
        CHECK_EQ(u.lastError(), QStringLiteral("token-mismatch"));
        rig.clock += kOfferLifetimeMs + 1;
        CHECK(!u.install(QStringLiteral("2026.11.01"), size, source));
        CHECK_EQ(u.lastError(), QStringLiteral("offer-expired"));
        CHECK(!u.install(QStringLiteral("2026.11.01"), size, source));
        CHECK_EQ(u.lastError(), QStringLiteral("no-offer"));
        CHECK(rig.hosts.assets().count(rig.hosts.assetPathOf(Channel::Stable, QStringLiteral("2026.11.01"), kAssetName)) == 1);   // only the check's HEAD
        CHECK(!QFileInfo(rig.root + QStringLiteral("/current")).exists());
        CHECK(!u.check(QStringLiteral("beta")));
        CHECK_EQ(u.lastError(), QStringLiteral("channel"));
    }
}

static void aborts() {
    // A working 2026.11.01 and an offered 2026.11.20 whose download or verification is interrupted.
    struct Setup {
        Rig rig;
        QByteArray working;
        Setup() {
            rig.hosts.publish(Channel::Stable, QStringLiteral("2026.11.01"), fakeDir("stable-2026.11.01"));
            { ExtractorUpdater first(rig.options()); CHECK(checkAndInstall(first, QStringLiteral("stable"))); }
            working = readFile(rig.root + QStringLiteral("/current/yt-dlp.pyz"));
            rig.hosts.publish(Channel::Stable, QStringLiteral("2026.11.20"), fakeDir("stable-2026.11.20"));
        }
        void assetMode(Route::Mode mode) {
            rig.hosts.assets().editRoute(rig.hosts.assetPathOf(Channel::Stable, QStringLiteral("2026.11.20"), kAssetName),
                                         [mode](Route &r) { r.mode = mode; });
        }
        void checkOld(ExtractorUpdater &u) {
            CHECK_EQ(u.installedVersion(), QStringLiteral("2026.11.01"));
            CHECK(readFile(u.activeArchive()) == working);
            CHECK(!QFileInfo(rig.root + QStringLiteral("/staging")).exists());
            ExtractorUpdater restarted(rig.options());
            CHECK_EQ(restarted.installedVersion(), QStringLiteral("2026.11.01"));
            CHECK(readFile(restarted.activeArchive()) == working && !QFileInfo(rig.root + QStringLiteral("/staging")).exists());
            CHECK(noMarkers());
        }
    };

    CASE("aborted_install_cancel_mid_download");
    {
        Setup s;
        s.assetMode(Route::HalfThenHang);
        ExtractorUpdater u(s.rig.options());
        u.check(QStringLiteral("stable"));
        CHECK(settle(u) && u.state() == QLatin1String("offered"));
        u.install(u.offeredVersion(), u.offeredSize(), u.offeredSource());
        CHECK(waitFor([&]() { return u.bytesReceived() > 0; }));
        CHECK(QFileInfo(s.rig.root + QStringLiteral("/staging/yt-dlp.pyz")).exists());
        CHECK(modeOf(s.rig.root + QStringLiteral("/staging")) == 0700 && modeOf(s.rig.root + QStringLiteral("/staging/yt-dlp.pyz")) == 0600);
        u.cancel();
        CHECK_EQ(u.state(), QStringLiteral("failed"));
        CHECK_EQ(u.lastError(), QStringLiteral("cancelled"));
        s.checkOld(u);
    }

    CASE("aborted_install_connection_lost_mid_download");
    {
        Setup s;
        s.assetMode(Route::HalfThenClose);
        ExtractorUpdater u(s.rig.options());
        u.check(QStringLiteral("stable"));
        CHECK(settle(u));
        u.install(u.offeredVersion(), u.offeredSize(), u.offeredSource());
        CHECK(settle(u));
        CHECK(u.lastError() == QLatin1String("network") || u.lastError() == QLatin1String("size-mismatch"));
        s.checkOld(u);
    }

    CASE("aborted_install_crash_mid_download");
    {
        Setup s;
        s.assetMode(Route::HalfThenHang);
        {
            ExtractorUpdater u(s.rig.options());
            u.check(QStringLiteral("stable"));
            CHECK(settle(u));
            u.install(u.offeredVersion(), u.offeredSize(), u.offeredSource());
            CHECK(waitFor([&]() { return u.bytesReceived() > 0; }));
        }   // the app dies here: staging/ is left behind half written
        CHECK(QFileInfo(s.rig.root + QStringLiteral("/staging/yt-dlp.pyz")).exists());
        ExtractorUpdater restarted(s.rig.options());
        s.checkOld(restarted);
    }

    CASE("aborted_install_verifier_fails_or_hangs");
    {
        Setup s;
        UpdaterOptions o = s.rig.options();
        ProcessVerifierOptions crash;
        crash.program = python;
        crash.arguments << QStringLiteral("-I") << QStringLiteral("-c") << QStringLiteral("import os; os._exit(3)");
        crash.workerTask = false;
        o.verifier = processVerifier(crash);
        ExtractorUpdater u(o);
        CHECK(!checkAndInstall(u, QStringLiteral("stable")));
        CHECK_EQ(u.lastError(), QStringLiteral("verifier"));
        s.checkOld(u);

        // A verifier that claims success with the wrong digest is not believed.
        UpdaterOptions liar = s.rig.options();
        liar.verifier = [](const VerifyRequest &r, VerifyDone done) -> std::function<void()> {
            VerifyResult result; result.ok = true; result.version = r.expectVersion; result.sha256 = QString(64, QLatin1Char('0')); result.size = 1;
            QTimer::singleShot(0, [done, result]() { done(result); });
            return []() {};
        };
        ExtractorUpdater l(liar);
        CHECK(!checkAndInstall(l, QStringLiteral("stable")));
        CHECK_EQ(l.lastError(), QStringLiteral("checksum"));
        s.checkOld(l);

        // Cancel while the verifier is still running; and the app dying while it runs.
        UpdaterOptions slow = s.rig.options();
        int cancelled = 0;
        bool verifierCalled = false;
        slow.verifier = [&](const VerifyRequest &, VerifyDone) -> std::function<void()> {
            verifierCalled = true;
            return [&cancelled]() { ++cancelled; };
        };
        {
            ExtractorUpdater v(slow);
            v.check(QStringLiteral("stable"));
            CHECK(settle(v));
            v.install(v.offeredVersion(), v.offeredSize(), v.offeredSource());
            CHECK(waitFor([&]() { return v.state() == QLatin1String("verifying"); }));
            CHECK(verifierCalled && QFileInfo(s.rig.root + QStringLiteral("/staging/SHA2-256SUMS.sig")).exists());
            v.cancel();
            CHECK(cancelled == 1);
            CHECK_EQ(v.lastError(), QStringLiteral("cancelled"));
            s.checkOld(v);
        }
        {
            ExtractorUpdater v(slow);
            v.check(QStringLiteral("stable"));
            CHECK(settle(v));
            v.install(v.offeredVersion(), v.offeredSize(), v.offeredSource());
            CHECK(waitFor([&]() { return v.state() == QLatin1String("verifying"); }));
        }   // destroyed mid-verify (the verifier is cancelled, staging stays for recovery)
        CHECK(cancelled == 2);
        ExtractorUpdater restarted(s.rig.options());
        s.checkOld(restarted);
    }

    CASE("aborted_install_crash_mid_swap_recovers_old_current");
    {
        Setup s;
        const QString root = s.rig.root;
        // Crash after current -> previous, before staging -> current.
        CHECK(QDir().rename(root + QStringLiteral("/current"), root + QStringLiteral("/previous")));
        QDir().mkpath(root + QStringLiteral("/staging"));
        writeFile(root + QStringLiteral("/staging/yt-dlp.pyz"), "unverified");
        {
            ExtractorUpdater u(s.rig.options());
            CHECK_EQ(u.installedVersion(), QStringLiteral("2026.11.01"));
            CHECK(readFile(u.activeArchive()) == s.working);
            CHECK(!QFileInfo(root + QStringLiteral("/staging")).exists() && !QFileInfo(root + QStringLiteral("/previous")).exists());
        }
        // Crash in a rollback after current -> swap.
        CHECK(QDir().rename(root + QStringLiteral("/current"), root + QStringLiteral("/swap")));
        {
            ExtractorUpdater u(s.rig.options());
            CHECK_EQ(u.installedVersion(), QStringLiteral("2026.11.01"));
            CHECK(!QFileInfo(root + QStringLiteral("/swap")).exists());
        }
    }
}

static void rollbackAndActive() {
    CASE("rollback_restores_previous");
    {
        Rig rig;
        rig.hosts.publish(Channel::Stable, QStringLiteral("2026.11.01"), fakeDir("stable-2026.11.01"));
        ExtractorUpdater u(rig.options());
        CHECK(!u.rollback());
        CHECK_EQ(u.lastError(), QStringLiteral("no-previous"));
        CHECK(checkAndInstall(u, QStringLiteral("stable")));
        CHECK(u.previousVersion().isEmpty());
        rig.hosts.publish(Channel::Stable, QStringLiteral("2026.11.20"), fakeDir("stable-2026.11.20"));
        CHECK(checkAndInstall(u, QStringLiteral("stable")));
        CHECK_EQ(u.installedVersion(), QStringLiteral("2026.11.20"));
        CHECK_EQ(u.previousVersion(), QStringLiteral("2026.11.01"));
        CHECK(u.rollback());
        CHECK_EQ(u.state(), QStringLiteral("rolledBack"));
        CHECK_EQ(u.installedVersion(), QStringLiteral("2026.11.01"));
        CHECK_EQ(u.previousVersion(), QStringLiteral("2026.11.20"));
        CHECK(readFile(u.activeArchive()) == readFile(fakeDir("stable-2026.11.01") + QStringLiteral("/yt-dlp")));
        CHECK(!QFileInfo(rig.root + QStringLiteral("/swap")).exists());
        ExtractorUpdater restarted(rig.options());
        CHECK_EQ(restarted.installedVersion(), QStringLiteral("2026.11.01"));
        CHECK(restarted.rollback());   // and forward again
        CHECK_EQ(restarted.installedVersion(), QStringLiteral("2026.11.20"));

        CASE("revert_to_bundled_keeps_previous");
        CHECK(restarted.revertToBundled());
        CHECK(restarted.usingBundled() && restarted.activeArchive() == rig.bundled);
        CHECK_EQ(restarted.previousVersion(), QStringLiteral("2026.11.01"));
        CHECK(restarted.rollback());
        CHECK_EQ(restarted.installedVersion(), QStringLiteral("2026.11.01"));
        CHECK(noMarkers());
    }

    CASE("active_archive_falls_back_to_bundled_when_current_damaged");
    {
        Rig rig;
        rig.hosts.publish(Channel::Stable, QStringLiteral("2026.11.01"), fakeDir("stable-2026.11.01"));
        { ExtractorUpdater u(rig.options()); CHECK(checkAndInstall(u, QStringLiteral("stable"))); }
        const QString archive = rig.root + QStringLiteral("/current/yt-dlp.pyz");
        QByteArray data = readFile(archive);
        data[100] = static_cast<char>(data.at(100) ^ 0x20);
        CHECK(writeFile(archive, data));
        {
            ExtractorUpdater u(rig.options());
            CHECK(u.usingBundled() && u.activeArchive(QStringLiteral("/usr/share/harbour-sailtube/runtime/yt-dlp.pyz"))
                  == QLatin1String("/usr/share/harbour-sailtube/runtime/yt-dlp.pyz"));
            CHECK(u.installedVersion().isEmpty());
        }
        data[100] = static_cast<char>(data.at(100) ^ 0x20);
        CHECK(writeFile(archive, data));
        { ExtractorUpdater u(rig.options()); CHECK(!u.usingBundled()); }
        QFile::remove(rig.root + QStringLiteral("/current/manifest.json"));
        { ExtractorUpdater u(rig.options()); CHECK(u.usingBundled() && u.activeArchive() == rig.bundled); }
        // The bundled version as main.cpp reads it from the packaged source manifest.
        const QString sm = rig.dir.path() + QStringLiteral("/source-manifest.json");
        CHECK(writeFile(sm, "{\"schema\":1,\"inputs\":{\"yt_dlp_version\":\"2026.10.26\"}}"));
        CHECK_EQ(ExtractorUpdater::bundledVersionFromManifest(sm), QStringLiteral("2026.10.26"));
        CHECK(writeFile(sm, "{\"inputs\":{\"yt_dlp_version\":\"../x\"}}"));
        CHECK(ExtractorUpdater::bundledVersionFromManifest(sm).isEmpty());
        CHECK(ExtractorUpdater::bundledVersionFromManifest(rig.dir.path() + QStringLiteral("/absent.json")).isEmpty());
        // A newer bundled extractor (an app update) wins over an older current.
        Rig rig2;
        rig2.hosts.publish(Channel::Stable, QStringLiteral("2026.11.01"), fakeDir("stable-2026.11.01"));
        { ExtractorUpdater u(rig2.options()); CHECK(checkAndInstall(u, QStringLiteral("stable"))); }
        UpdaterOptions o = rig2.options();
        o.bundledVersion = QStringLiteral("2026.11.05");
        ExtractorUpdater u(o);
        CHECK(u.usingBundled() && u.activeVersion() == QLatin1String("2026.11.05"));
        u.check(QStringLiteral("stable"));
        CHECK(settle(u));
        CHECK_EQ(u.state(), QStringLiteral("upToDate"));
    }
}

static void neverRunsAndReal() {
    CASE("verifier_never_imports_or_executes_archive");
    {
        // Positive control: the markers are created when the archive is really imported.
        QTemporaryDir probe;
        QProcess p;
        QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
        env.insert(QStringLiteral("XUPD_MARKER_DIR"), probe.path());
        p.setProcessEnvironment(env);
        p.start(python, QStringList() << QStringLiteral("-I") << QStringLiteral("-c")
                << QStringLiteral("import sys; sys.path.insert(0, sys.argv[1]); import yt_dlp.version")
                << fakeDir("stable-2026.11.01") + QStringLiteral("/yt-dlp"));
        CHECK(p.waitForFinished(30000) && p.exitCode() == 0);
        CHECK(QFileInfo(probe.path() + QStringLiteral("/imported-init")).exists());
        // Every verifier run in this program (CLI and worker task) used XUPD_MARKER_DIR=markerDir.
        CHECK(noMarkers());
    }

    CASE("worker_task_uses_only_the_pinned_installed_key");
    {
        QTemporaryDir runtime;
        QFile::copy(repoRoot + QStringLiteral("/Core/Worker/Runtime/worker.py"), runtime.path() + QStringLiteral("/worker.py"));
        QFile::copy(repoRoot + QStringLiteral("/Core/Extractor/Runtime/verify_release.py"), runtime.path() + QStringLiteral("/verify_release.py"));
        // A test key in place of the installed release key: refused by the pin, whatever signed the release.
        QFile::copy(fixtures + QStringLiteral("/fake/test-key.asc"), runtime.path() + QStringLiteral("/yt-dlp-release-key.asc"));
        Rig rig;
        rig.hosts.publish(Channel::Stable, QStringLiteral("2026.11.01"), fakeDir("stable-2026.11.01"));
        UpdaterOptions o = rig.options();
        o.verifier = workerVerifier(runtime.path());
        ExtractorUpdater u(o);
        CHECK(!checkAndInstall(u, QStringLiteral("stable")));
        CHECK_EQ(u.lastError(), QStringLiteral("key-pin"));
        CHECK(u.usingBundled() && !QFileInfo(rig.root + QStringLiteral("/current")).exists());
        CHECK(noMarkers());
    }

    CASE("real_2026_10_26_release_verifies_with_pinned_key");
    {
        const QByteArray sums = readFile(fixtures + QStringLiteral("/real/SHA2-256SUMS"));
        CHECK(sumsEntry(sums.toStdString(), kAssetName) == "e51e69e89489f199530f13d32a4342da3ddb2cf34842f364603a066db05afbf3");
        QTemporaryDir runtime;
        QFile::copy(repoRoot + QStringLiteral("/Core/Worker/Runtime/worker.py"), runtime.path() + QStringLiteral("/worker.py"));
        QFile::copy(repoRoot + QStringLiteral("/Core/Extractor/Runtime/verify_release.py"), runtime.path() + QStringLiteral("/verify_release.py"));
        QFile::copy(repoRoot + QStringLiteral("/Core/Extractor/Runtime/yt-dlp-release-key.asc"), runtime.path() + QStringLiteral("/yt-dlp-release-key.asc"));
        if (realArchive.isEmpty()) {
            std::printf("extractor_update:   (the real archive is not available: end-to-end part skipped)\n");
        } else {
            QTemporaryDir release;
            QFile::copy(realArchive, release.path() + QStringLiteral("/yt-dlp"));
            QFile::copy(fixtures + QStringLiteral("/real/SHA2-256SUMS"), release.path() + QStringLiteral("/SHA2-256SUMS"));
            QFile::copy(fixtures + QStringLiteral("/real/SHA2-256SUMS.sig"), release.path() + QStringLiteral("/SHA2-256SUMS.sig"));
            Rig rig;
            rig.hosts.publish(Channel::Stable, QStringLiteral("2026.10.26"), release.path());
            UpdaterOptions o = rig.options();
            o.verifier = workerVerifier(runtime.path());
            ExtractorUpdater u(o);
            CHECK(checkAndInstall(u, QStringLiteral("stable")));
            CHECK_EQ(u.installedVersion(), QStringLiteral("2026.10.26"));
            CHECK(manifestOf(rig.root + QStringLiteral("/current/manifest.json")).sha256
                  == "e51e69e89489f199530f13d32a4342da3ddb2cf34842f364603a066db05afbf3");
            CHECK(u.offeredSize() == -1 && QFileInfo(u.activeArchive()).size() == 3073072);
            CHECK(noMarkers());
        }
    }
}

int main(int argc, char **argv) {
    QCoreApplication app(argc, argv);
    repoRoot = QString::fromLocal8Bit(qgetenv("XUPD_ROOT"));
    python = QString::fromLocal8Bit(qgetenv("XUPD_PYTHON"));
    realArchive = QString::fromLocal8Bit(qgetenv("XUPD_REAL_ARCHIVE"));
    fixtures = repoRoot + QStringLiteral("/tests/extractor_update/fixtures");
    QTemporaryDir markers;
    markerDir = markers.path();
    if (repoRoot.isEmpty() || python.isEmpty()) { std::printf("extractor_update: XUPD_ROOT and XUPD_PYTHON are required\n"); return 2; }
    domainTests();
    happyPaths();
    refusals();
    aborts();
    rollbackAndActive();
    neverRunsAndReal();
    std::printf("extractor_update: %d cases, %d failures\n", cases, failures);
    return failures ? 1 : 0;
}
