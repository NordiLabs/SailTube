// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "SignedCatalogTransport.h"

#include "AccountService.h"
#include <QElapsedTimer>
#include <QPointer>
#include <memory>

namespace nordtile { namespace video { namespace account {

SignedCatalogTransport::SignedCatalogTransport(AccountService *service, QObject *parent)
    : QObject(parent), service_(service) {}

quint64 SignedCatalogTransport::send(const catalog::HttpRequest &request, QObject *context, catalog::HttpCallback done) {
    const quint64 id = ++next_;
    if (!hosts_.contains(request.url.host()) && NetworkAuthTransport::allowedTarget(request.url))
        hosts_.append(request.url.host());
    AuthRequest req;
    req.url = request.url;
    req.body = request.method == "POST" ? request.body : QByteArray();
    req.headers = request.headers;
    req.timeoutMs = request.timeoutMs;
    std::shared_ptr<QElapsedTimer> timer(new QElapsedTimer);
    timer->start();
    QPointer<SignedCatalogTransport> self(this);
    service_->sendSigned(req, context, [self, id, done, timer, request](const AuthReply &r) {
        if (self && self->cancelled_.remove(id)) return;
        catalog::HttpReply reply;
        reply.status = r.status;
        reply.body = r.body;
        reply.timedOut = r.timedOut;
        reply.finalUrl = request.url;
        reply.elapsedMs = timer->elapsed();
        // Never carries URLs or tokens.
        if (r.status == 0) reply.errorText = QStringLiteral("not sent or failed (signed out, offline or refused)");
        done(reply);
    });
    return id;
}

void SignedCatalogTransport::cancel(quint64 id) { cancelled_.insert(id); }

}}}
