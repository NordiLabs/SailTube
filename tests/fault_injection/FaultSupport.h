// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#pragma once

// Fault-injection helpers (NFR-ROB-01): a scripted HTTP transport that can answer any status,
// a malformed body, a network error or a timeout per request; a loopback transport that sends the
// real NetworkTransport to local misbehaving servers; the fixture loader for
// tests/fault_injection/fixtures (decompressed nowhere: plain synthetic files) and a named-case
// runner. CHECK / CHECK_EQ / waitFor come from the catalog suites' TestSupport.h (read-only).

#include "tests/catalog_common/TestSupport.h"
#include "Core/Catalog/Infrastructure/HttpTransport.h"

#include <QFile>
#include <QHostAddress>
#include <QJsonDocument>
#include <QJsonObject>
#include <QPointer>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTimer>
#include <QUrlQuery>
#include <cstdio>
#include <functional>
#include <string>
#include <vector>

namespace fault {

using nordtile::video::catalog::HttpCallback;
using nordtile::video::catalog::HttpReply;
using nordtile::video::catalog::HttpRequest;
using nordtile::video::catalog::HttpTransport;

// ---- fixtures ------------------------------------------------------------------------------

inline QByteArray fixture(const char *name) {
    const QString path = QString::fromLocal8Bit(qgetenv("SAILTUBE_FAULT_FIXTURES")) + QLatin1Char('/') + QLatin1String(name);
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) {
        std::printf("FAIL missing fault fixture %s\n", name);
        ++catalog_test::failures();
        return QByteArray();
    }
    return f.readAll();
}

// The fixture with every "@KEY@" placeholder replaced.
inline QByteArray fixture(const char *name, const std::vector<std::pair<QByteArray, QByteArray>> &replace) {
    QByteArray body = fixture(name);
    for (const auto &r : replace) body.replace("@" + r.first + "@", r.second);
    return body;
}

// ---- scripted transport --------------------------------------------------------------------

struct Answer {
    int status = 200;
    QByteArray body;
    int delayMs = 0;
    bool timedOut = false;
    static Answer ok(const QByteArray &b) { Answer a; a.body = b; return a; }
    static Answer http(int status, const QByteArray &b = QByteArray()) { Answer a; a.status = status; a.body = b; return a; }
    static Answer network() { Answer a; a.status = 0; return a; }
    static Answer timeout() { Answer a; a.status = 0; a.timedOut = true; return a; }
};

// Answers every request through `handler` (unhandled: a network error). The reply carries what a
// real transport would: status 0 + errorText for transport failures, timedOut for timeouts.
class FaultTransport final : public QObject, public HttpTransport {
public:
    std::function<Answer(const HttpRequest &)> handler;
    QList<HttpRequest> requests;
    QStringList hosts;

    quint64 send(const HttpRequest &request, QObject *context, HttpCallback done) override {
        requests.append(request);
        if (!hosts.contains(request.url.host())) hosts.append(request.url.host());
        const Answer a = handler ? handler(request) : Answer::network();
        const quint64 id = ++next_;
        QPointer<QObject> guard(context);
        const bool hasContext = context != nullptr;
        const QUrl url = request.url;
        QTimer::singleShot(a.delayMs, this, [this, id, a, done, guard, hasContext, url] {
            if (cancelled_.contains(id) || (hasContext && !guard)) return;
            HttpReply r;
            r.status = a.status;
            r.body = a.body;
            r.timedOut = a.timedOut;
            r.finalUrl = url;
            if (a.status == 0) r.errorText = a.timedOut ? QStringLiteral("timeout") : QStringLiteral("injected network error");
            done(r);
        });
        return id;
    }
    void cancel(quint64 id) override { cancelled_.append(id); }
    QStringList contactedHosts() const override { return hosts; }

    int count(const std::function<bool(const HttpRequest &)> &pred) const {
        int n = 0;
        for (const HttpRequest &r : requests) if (pred(r)) ++n;
        return n;
    }

private:
    quint64 next_ = 0;
    QList<quint64> cancelled_;
};

// Request inspection.
inline QJsonObject bodyOf(const HttpRequest &r) { return QJsonDocument::fromJson(r.body).object(); }
inline bool endpoint(const HttpRequest &r, const char *name) {
    return r.url.path() == QStringLiteral("/youtubei/v1/") + QLatin1String(name);
}
inline QString field(const HttpRequest &r, const char *name) { return bodyOf(r).value(QLatin1String(name)).toString(); }
inline QString rssChannel(const HttpRequest &r) {
    return r.url.path().endsWith(QLatin1String("/feeds/videos.xml")) ? QUrlQuery(r.url).queryItemValue(QStringLiteral("channel_id")) : QString();
}

// ---- misbehaving local servers for the real NetworkTransport -------------------------------

// Accepts connections and, once a request head arrived, misbehaves as told:
//   Silent     never answers (the transport's timeout must fire)
//   Garbage    answers bytes that are not HTTP, then closes
//   Truncated  "200 OK" with Content-Length 4096 and 20 bytes of JSON, then closes
//   Reset      closes the connection without a byte
class BadServer {
public:
    enum class Mode { Silent, Garbage, Truncated, Reset };
    QTcpServer server;
    Mode mode = Mode::Silent;
    int connections = 0;
    QList<QPointer<QTcpSocket>> held;

    bool start() {
        if (!server.listen(QHostAddress::LocalHost, 0)) return false;
        QObject::connect(&server, &QTcpServer::newConnection, [this]() {
            while (QTcpSocket *s = server.nextPendingConnection()) {
                ++connections;
                QObject::connect(s, &QTcpSocket::readyRead, [this, s]() { onRead(s); });
                QObject::connect(s, &QTcpSocket::disconnected, s, &QObject::deleteLater);
            }
        });
        return true;
    }
    quint16 port() const { return server.serverPort(); }

private:
    void onRead(QTcpSocket *s) {
        QByteArray buf = s->property("buf").toByteArray() + s->readAll();
        s->setProperty("buf", buf);
        if (buf.indexOf("\r\n\r\n") < 0 || s->property("done").toBool()) return;
        s->setProperty("done", true);
        switch (mode) {
        case Mode::Silent: held.append(s); return;
        case Mode::Garbage: s->write("SSH-2.0-not-http\r\n\x01\x02\x03\r\n\r\n"); break;
        case Mode::Truncated:
            s->write("HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nContent-Length: 4096\r\n\r\n{\"contents\":{\"twoCol");
            break;
        case Mode::Reset: break;
        }
        s->flush();
        s->disconnectFromHost();
    }
};

// Sends every request to http://127.0.0.1:<port> (same path and query) through a real
// NetworkTransport, with the timeout clamped to `timeoutMs`.
class LoopbackTransport final : public QObject, public HttpTransport {
public:
    LoopbackTransport(quint16 port, int timeoutMs) : real_(QStringLiteral("fault-loopback")), port_(port), timeoutMs_(timeoutMs) {}
    quint64 send(const HttpRequest &request, QObject *context, HttpCallback done) override {
        HttpRequest r = request;
        QUrl url;
        url.setScheme(QStringLiteral("http"));
        url.setHost(QStringLiteral("127.0.0.1"));
        url.setPort(port_);
        url.setPath(request.url.path());
        url.setQuery(request.url.query());
        r.url = url;
        r.timeoutMs = timeoutMs_;
        return real_.send(r, context, done);
    }
    void cancel(quint64 id) override { real_.cancel(id); }
    QStringList contactedHosts() const override { return real_.contactedHosts(); }

private:
    nordtile::video::catalog::NetworkTransport real_;
    quint16 port_;
    int timeoutMs_;
};

// ---- named cases ---------------------------------------------------------------------------

struct CaseLog {
    int passed = 0, failed = 0, knownBugs = 0, gaps = 0;
    std::vector<std::string> failedNames;
};
inline CaseLog &log() { static CaseLog l; return l; }

// Runs a case; it passes when it adds no failed CHECK.
inline void run(const char *name, const std::function<void()> &body) {
    const int before = catalog_test::failures();
    body();
    const bool ok = catalog_test::failures() == before;
    std::printf("CASE %s: %s\n", name, ok ? "PASS" : "FAIL");
    std::fflush(stdout);
    if (ok) ++log().passed;
    else { ++log().failed; log().failedNames.push_back(name); }
}

// A behaviour the suite documents as a real defect (docs/FAULT_INJECTION.md). `stillPresent`
// is measured by the case; the suite does not fail on it, and reports XPASS once it is fixed so
// the expectation (and the document) get updated.
inline void knownBug(const char *name, const char *where, bool stillPresent) {
    if (stillPresent) {
        std::printf("CASE %s: KNOWN-BUG (%s)\n", name, where);
        ++log().knownBugs;
    } else {
        std::printf("CASE %s: XPASS (fixed? update docs/FAULT_INJECTION.md)\n", name);
        ++log().passed;
    }
    std::fflush(stdout);
}

// A protection the code does not claim (a gap, not a failure): reported, never failing.
inline void gap(const char *name, const char *what, bool present) {
    if (present) {
        std::printf("CASE %s: GAP (%s)\n", name, what);
        ++log().gaps;
    } else {
        std::printf("CASE %s: PASS (gap closed: %s)\n", name, what);
        ++log().passed;
    }
    std::fflush(stdout);
}

inline int finish(const char *suite) {
    const CaseLog &l = log();
    std::printf("%s: %d cases passed, %d failed, %d known bugs, %d gaps (%d checks)\n", suite, l.passed, l.failed,
                l.knownBugs, l.gaps, catalog_test::checks());
    for (const std::string &n : l.failedNames) std::printf("%s: failed case %s\n", suite, n.c_str());
    return l.failed || catalog_test::failures() ? 1 : 0;
}

}  // namespace fault
