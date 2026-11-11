// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "AuthTransport.h"

#include <QNetworkAccessManager>
#include <QNetworkCookie>
#include <QNetworkCookieJar>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QPointer>
#include <QTimer>

namespace nordtile { namespace video { namespace account {

namespace {

// Stores nothing: the session is sent explicitly with each request.
class NoCookieJar final : public QNetworkCookieJar {
public:
    using QNetworkCookieJar::QNetworkCookieJar;
    bool setCookiesFromUrl(const QList<QNetworkCookie> &, const QUrl &) override { return false; }
    QList<QNetworkCookie> cookiesForUrl(const QUrl &) const override { return QList<QNetworkCookie>(); }
};

} // namespace

NetworkAuthTransport::NetworkAuthTransport(QObject *parent)
    : QObject(parent), manager_(new QNetworkAccessManager(this)) {
    manager_->setCookieJar(new NoCookieJar(manager_));
}

bool NetworkAuthTransport::allowedTarget(const QUrl &url) {
    return url.scheme() == QLatin1String("https") && url.host() == QLatin1String("www.youtube.com")
        && (url.port() == -1 || url.port() == 443) && url.userInfo().isEmpty();
}

void NetworkAuthTransport::send(const AuthRequest &request, QObject *context, Done done) {
    QPointer<QObject> guard(context);
    const bool hasContext = context != nullptr;
    if (!allowedTarget(request.url)) {
        QTimer::singleShot(0, this, [done, guard, hasContext] {
            if (hasContext && !guard) return;
            done(AuthReply());
        });
        return;
    }
    QNetworkRequest req(request.url);
    // Redirects are not followed (Qt 5.6 default): a 3xx comes back as is.
    for (const auto &h : request.headers) req.setRawHeader(h.first, h.second);
    QNetworkReply *reply = request.body.isEmpty() ? manager_->get(req) : manager_->post(req, request.body);
    QTimer *timer = new QTimer(reply);
    timer->setSingleShot(true);
    bool *timedOut = new bool(false);
    connect(timer, &QTimer::timeout, reply, [reply, timedOut] { *timedOut = true; reply->abort(); });
    timer->start(request.timeoutMs);
    connect(reply, &QNetworkReply::finished, this, [reply, done, guard, hasContext, timedOut] {
        AuthReply r;
        r.timedOut = *timedOut;
        delete timedOut;
        if (reply->error() == QNetworkReply::NoError || reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).isValid()) {
            r.status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
            r.body = reply->readAll();
            for (const QNetworkReply::RawHeaderPair &h : reply->rawHeaderPairs())
                if (h.first.toLower() == "set-cookie")
                    for (const QByteArray &line : h.second.split('\n'))
                        if (!line.trimmed().isEmpty()) r.setCookies.push_back(line.trimmed());
        }
        if (r.timedOut) r.status = 0;
        reply->deleteLater();
        if (hasContext && !guard) return;
        done(r);
    });
}

}}}
