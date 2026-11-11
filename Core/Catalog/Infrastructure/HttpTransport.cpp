// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "HttpTransport.h"

#include <QElapsedTimer>
#include <QNetworkAccessManager>
#include <QNetworkCookie>
#include <QNetworkCookieJar>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QTimer>

namespace nordtile { namespace video { namespace catalog {

namespace {
// Accepts no cookies: discovery is anonymous (ACC owns signed-in calls).
class NoCookies final : public QNetworkCookieJar {
public:
    using QNetworkCookieJar::QNetworkCookieJar;
    bool setCookiesFromUrl(const QList<QNetworkCookie> &, const QUrl &) override { return false; }
    QList<QNetworkCookie> cookiesForUrl(const QUrl &) const override { return {}; }
};
}

QByteArray webUserAgent() {
    return QByteArrayLiteral("Mozilla/5.0 (X11; Linux x86_64) AppleWebKit/537.36 (KHTML, like Gecko) "
                             "Chrome/130.0.0.0 Safari/537.36");
}

struct NetworkTransport::Pending {
    quint64 id = 0;
    QNetworkReply *reply = nullptr;
    QTimer *timer = nullptr;
    QPointer<QObject> context;
    bool hasContext = false;
    HttpCallback done;
    QElapsedTimer clock;
};

NetworkTransport::NetworkTransport(const QString &name, QObject *parent)
    : QObject(parent), name_(name), manager_(new QNetworkAccessManager(this)) {
    manager_->setCookieJar(new NoCookies(manager_));
}

void NetworkTransport::warm(const QString &host) {
    if (host.isEmpty()) return;
    static QElapsedTimer clock;
    if (!clock.isValid()) clock.start();
    const qint64 now = clock.elapsed();
    const auto it = warmed_.constFind(host);
    if (it != warmed_.constEnd() && now - it.value() < kWarmIntervalMs) return;
    warmed_.insert(host, now);
    if (!hosts_.contains(host)) hosts_.append(host);
    manager_->connectToHostEncrypted(host, 443);
    qInfo("sailtube-timing prewarm %s %s", qPrintable(name_), qPrintable(host));
}

NetworkTransport::~NetworkTransport() {
    for (Pending *p : pending_) {
        if (p->reply) { p->reply->disconnect(this); p->reply->abort(); p->reply->deleteLater(); }
        delete p->timer;
        delete p;
    }
    pending_.clear();
}

quint64 NetworkTransport::send(const HttpRequest &request, QObject *context, HttpCallback done) {
    QNetworkRequest req(request.url);
    req.setAttribute(QNetworkRequest::FollowRedirectsAttribute, true);
    req.setAttribute(QNetworkRequest::CacheLoadControlAttribute, QNetworkRequest::AlwaysNetwork);
    bool hasUa = false;
    for (const auto &h : request.headers) {
        req.setRawHeader(h.first, h.second);
        if (qstricmp(h.first.constData(), "User-Agent") == 0) hasUa = true;
    }
    if (!hasUa) req.setRawHeader("User-Agent", webUserAgent());
    if (request.method == "POST" && !req.hasRawHeader("Content-Type"))
        req.setRawHeader("Content-Type", "application/json");

    Pending *p = new Pending;
    p->id = next_++;
    p->context = context;
    p->hasContext = context != nullptr;
    p->done = std::move(done);
    p->clock.start();
    p->reply = request.method == "POST" ? manager_->post(req, request.body) : manager_->get(req);
    p->timer = new QTimer;
    p->timer->setSingleShot(true);
    const quint64 id = p->id;
    connect(p->timer, &QTimer::timeout, this, [this, id] { finish(id, true); });
    p->timer->start(qMax(1000, request.timeoutMs));
    connect(p->reply, &QNetworkReply::finished, this, [this, id] { finish(id, false); });
    pending_.append(p);
    const QString host = request.url.host();
    if (!hosts_.contains(host)) hosts_.append(host);
    return id;
}

void NetworkTransport::cancel(quint64 id) {
    for (int i = 0; i < pending_.size(); ++i) {
        Pending *p = pending_[i];
        if (p->id != id) continue;
        pending_.removeAt(i);
        p->reply->disconnect(this);
        p->reply->abort();
        p->reply->deleteLater();
        delete p->timer;
        delete p;   // cancelled callers are not called back
        return;
    }
}

void NetworkTransport::finish(quint64 id, bool timedOut) {
    Pending *p = nullptr;
    for (int i = 0; i < pending_.size(); ++i) {
        if (pending_[i]->id == id) { p = pending_.takeAt(i); break; }
    }
    if (!p) return;
    HttpReply r;
    r.elapsedMs = p->clock.elapsed();
    r.timedOut = timedOut;
    p->reply->disconnect(this);
    if (timedOut) {
        p->reply->abort();
        r.errorText = QStringLiteral("timeout");
    } else {
        r.status = p->reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        r.body = p->reply->readAll();
        r.finalUrl = p->reply->url();
        if (p->reply->error() != QNetworkReply::NoError && r.status == 0)
            r.errorText = p->reply->errorString();
        // A 2xx whose body was cut off is a network failure, not a valid answer.
        const bool success = r.status >= 200 && r.status < 300;
        const bool encoded = !p->reply->rawHeader("Content-Encoding").isEmpty();
        bool lengthKnown = false;
        const qint64 announced = p->reply->rawHeader("Content-Length").toLongLong(&lengthKnown);
        if (success && (p->reply->error() != QNetworkReply::NoError ||
                        (lengthKnown && !encoded && r.body.size() < announced))) {
            r.truncated = true;
            r.status = 0;
            r.errorText = QStringLiteral("truncated body");
        }
    }
    p->reply->deleteLater();
    delete p->timer;
    HttpCallback done = std::move(p->done);
    const bool alive = !p->hasContext || p->context;
    delete p;
    if (alive && done) done(r);
}

QStringList NetworkTransport::contactedHosts() const { return hosts_; }

}}}
