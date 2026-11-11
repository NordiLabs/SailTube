// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#pragma once

// Shared helpers for the catalog test suites: checks, recorded fixtures (decompressed by
// compile.sh into $SAILTUBE_CATALOG_FIXTURES), a fake HTTP transport and an event-loop wait.
// Offline only: the fake transport answers from fixtures and fails anything it does not know.

#include "Core/Catalog/Infrastructure/HttpTransport.h"

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QList>
#include <QTimer>
#include <cstdio>
#include <functional>

namespace catalog_test {

inline int &failures() { static int n = 0; return n; }
inline int &checks() { static int n = 0; return n; }

#define CHECK(c) do { ++catalog_test::checks(); if (!(c)) { std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); ++catalog_test::failures(); } } while (0)
#define CHECK_EQ(a, b) do { ++catalog_test::checks(); const auto va_ = (a); const auto vb_ = (b); if (!(va_ == vb_)) { \
    std::printf("FAIL %s:%d: %s == %s (got '%s' vs '%s')\n", __FILE__, __LINE__, #a, #b, \
                catalog_test::show(va_).c_str(), catalog_test::show(vb_).c_str()); ++catalog_test::failures(); } } while (0)

inline std::string show(const std::string &s) { return s; }
inline std::string show(const QString &s) { return s.toStdString(); }
inline std::string show(const char *s) { return s ? s : "(null)"; }
inline std::string show(const QByteArray &s) { return s.toStdString(); }
inline std::string show(bool b) { return b ? "true" : "false"; }
template <typename T> std::string show(const T &v) { return std::to_string(v); }

inline int finish(const char *suite) {
    if (failures()) { std::printf("%s: %d of %d checks FAILED\n", suite, failures(), checks()); return 1; }
    std::printf("%s: all %d checks passed\n", suite, checks());
    return 0;
}

inline QString fixturePath(const QString &name) {
    return QString::fromLocal8Bit(qgetenv("SAILTUBE_CATALOG_FIXTURES")) + QLatin1Char('/') + name;
}

// `name` without the .gz suffix, e.g. "search_en_p1.json".
inline QByteArray fixture(const QString &name) {
    QFile f(fixturePath(name));
    if (!f.open(QIODevice::ReadOnly)) { std::printf("FAIL missing fixture %s\n", qPrintable(name)); ++failures(); return QByteArray(); }
    return f.readAll();
}

inline QJsonObject fixtureJson(const QString &name) { return QJsonDocument::fromJson(fixture(name)).object(); }

inline bool waitFor(const std::function<bool()> &done, int ms = 5000) {
    QElapsedTimer t; t.start();
    while (!done()) {
        if (t.elapsed() > ms) return false;
        QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
    }
    return true;
}

// Serves canned answers. Rules match in insertion order on a URL substring and a body
// substring; a rule with `times` > 0 is used that many times. Unmatched requests fail with
// status 0 ("network").
class FakeTransport final : public QObject, public nordtile::video::catalog::HttpTransport {
public:
    struct Rule {
        QString urlContains;
        QByteArray bodyContains;
        int status = 200;
        QByteArray body;
        int delayMs = 0;
        int times = -1;
    };
    QList<Rule> rules;
    QList<nordtile::video::catalog::HttpRequest> requests;
    QStringList hosts;

    Rule &on(const QString &urlContains, const QByteArray &bodyContains, int status, const QByteArray &body, int delayMs = 0, int times = -1) {
        Rule r; r.urlContains = urlContains; r.bodyContains = bodyContains; r.status = status; r.body = body;
        r.delayMs = delayMs; r.times = times;
        rules.append(r);
        return rules.last();
    }

    quint64 send(const nordtile::video::catalog::HttpRequest &request, QObject *context,
                 nordtile::video::catalog::HttpCallback done) override {
        requests.append(request);
        if (!hosts.contains(request.url.host())) hosts.append(request.url.host());
        const quint64 id = ++next_;
        nordtile::video::catalog::HttpReply reply;
        int delay = 0;
        bool matched = false;
        for (Rule &r : rules) {
            if (r.times == 0) continue;
            if (!request.url.toString().contains(r.urlContains)) continue;
            if (!r.bodyContains.isEmpty() && !request.body.contains(r.bodyContains)) continue;
            reply.status = r.status; reply.body = r.body; reply.finalUrl = request.url; delay = r.delayMs;
            if (r.times > 0) --r.times;
            matched = true;
            break;
        }
        if (!matched) reply.errorText = QStringLiteral("no fixture for ") + request.url.toString();
        QPointer<QObject> guard(context);
        const bool hasContext = context != nullptr;
        QTimer::singleShot(delay, this, [this, id, reply, done, guard, hasContext] {
            if (cancelled_.contains(id)) return;
            if (hasContext && !guard) return;
            done(reply);
        });
        return id;
    }
    void cancel(quint64 id) override { cancelled_.append(id); }
    QStringList contactedHosts() const override { return hosts; }

    int count(const QString &urlContains, const QByteArray &bodyContains = QByteArray()) const {
        int n = 0;
        for (const auto &r : requests)
            if (r.url.toString().contains(urlContains) && (bodyContains.isEmpty() || r.body.contains(bodyContains))) ++n;
        return n;
    }

private:
    quint64 next_ = 0;
    QList<quint64> cancelled_;
};

}  // namespace catalog_test
