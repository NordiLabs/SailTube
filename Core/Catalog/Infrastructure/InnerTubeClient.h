// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#pragma once

// First data tier (DSC-01): an anonymous InnerTube WEB POST to
// https://www.youtube.com/youtubei/v1/<endpoint>?prettyPrint=false with the shared client
// identity (DSC-02) and the device hl/gl (I18N-01). No API key, no cookies, no visitor id.
//
// One request is different (SEL-06): the caption track list of a video. The anonymous WEB
// player answer lists no tracks, so postTrackList() asks the player endpoint with the pinned
// shared client definition (Shared/SailTune/Core/SharedPlayback/Domain/InnerTubeClient.h, the
// one playback resolves with) and the device hl, and that client is answered only with a
// provider visitor id. The id is taken from the answers this client received anyway (each
// anonymous answer names a new one), else asked for once; it lives in memory only, is never
// written anywhere and is sent with no other request.
//
// The answer is parsed on a WorkLane together with the caller's own parse function, so 0.5 MB
// documents never block the UI thread. HTTP failures are classified (Outcome) and never count
// as drift; an HTTP 400 asks the identity for a background re-scrape.

#include "ClientIdentity.h"
#include "InnerTubeDefinitions.h"
#include "HttpTransport.h"
#include "WorkLane.h"
#include "Core/Catalog/Domain/Outcome.h"

#include <QJsonDocument>
#include <QJsonObject>
#include <QElapsedTimer>
#include <QSharedPointer>
#include <QWeakPointer>
#include <QObject>
#include <QPointer>
#include <functional>
#include <memory>
#include <mutex>

namespace nordtile { namespace video { namespace catalog {

struct InnerTubeReply {
    Outcome outcome = Outcome::Network;   // Ok means "HTTP 200 with a JSON object"
    int httpStatus = 0;
    QJsonObject json;
    qint64 elapsedMs = 0;
    QString errorText;
};

class InnerTubeClient final : public QObject {
    Q_OBJECT
public:
    InnerTubeClient(HttpTransport *transport, ClientIdentity *identity, WorkLane *lane,
                    const DiscoveryLocale &locale, QObject *parent = nullptr);

    DiscoveryLocale locale() const { return locale_; }
    void setLocale(const DiscoveryLocale &locale) { locale_ = locale; }
    HttpTransport *transport() const { return transport_; }
    WorkLane *lane() const { return lane_; }
    ClientIdentity *identity() const { return identity_; }

    // The body's "context" member: {client: {clientName: WEB, clientVersion, hl, gl}}.
    QJsonObject context() const;
    // The full request (exposed for tests and the network inventory).
    HttpRequest buildRequest(const QString &endpoint, const QJsonObject &body, int timeoutMs) const;

    // POSTs and runs `parse(reply)` on the lane, then `done(result)` on `context`'s thread.
    template <typename T>
    quint64 post(const QString &endpoint, const QJsonObject &body, QObject *context,
                 std::function<T(const InnerTubeReply &)> parse, std::function<void(T)> done,
                 int timeoutMs = 10000) {
        return send<T>(endpoint.toLatin1(), buildRequest(endpoint, body, timeoutMs), context, parse, done);
    }

    // SEL-06: the player answer that lists `videoId`'s caption tracks (see the file comment).
    // Sent with the visitor id known now; call ensureVisitor() first when there may be none.
    template <typename T>
    quint64 postTrackList(const QString &videoId, QObject *context,
                          std::function<T(const InnerTubeReply &)> parse, std::function<void(T)> done,
                          int timeoutMs = 10000) {
        return send<T>(QByteArray("player-tracks"), buildTrackListRequest(videoId, visitorData(), timeoutMs),
                       context, parse, done);
    }
    // The request postTrackList() sends (exposed for tests and the network inventory). The
    // visitor header is left out when `visitorData` is empty.
    HttpRequest buildTrackListRequest(const QString &videoId, const QString &visitorData, int timeoutMs) const;

    // The visitor id in memory ("" when none was seen yet).
    QString visitorData() const;
    // Drops it (the provider refused it); the next ensureVisitor() asks for a new one.
    void forgetVisitor();
    // responseContext.visitorData of an answer, "" when absent or not plain header text.
    static QString visitorOf(const QJsonObject &answer);
    // Runs `then` on `context`'s thread once a visitor id is in memory, asking the provider for
    // one when no answer has named one yet. `then` also runs when that request fails (the track
    // list request is then refused and reported as such). Never blocks.
    void ensureVisitor(QObject *context, std::function<void()> then);

    void cancel(quint64 id) { transport_->cancel(id); }

    // HTTP reply -> classified InnerTube reply (JSON parsed here; call it on a lane).
    static InnerTubeReply toReply(const HttpReply &http);
    // "sailtube-timing innertube <endpoint> net=<ms> parse=<ms> ready=<ms> bytes=<n> status=<s>":
    // net = request to last byte, parse = JSON + renderer parse on the lane, ready = from the last
    // byte until the result was delivered on the owner's thread (parse plus lane queueing).
    static void logStage(const QByteArray &endpoint, qint64 netMs, qint64 parseMs, qint64 readyMs, int bytes, int status);

private:
    // Written on the lane (answers), read on the owner thread.
    struct VisitorMemory {
        std::mutex mutex;
        QString value;
    };

    template <typename T>
    quint64 send(const QByteArray &name, const HttpRequest &request, QObject *context,
                 std::function<T(const InnerTubeReply &)> parse, std::function<void(T)> done) {
        if (identity_) identity_->refreshIfStale();
        WorkLane *lane = lane_;
        QPointer<QObject> guard(context);
        const bool hasContext = context != nullptr;
        ClientIdentity *identity = identity_;
        HttpTransport *transport = transport_;
        const std::shared_ptr<VisitorMemory> visitor = visitor_;
        // NFR-ROB-01: a cut-off body is sent once more before it counts as a failure.
        QSharedPointer<int> attempts(new int(0));
        QSharedPointer<HttpCallback> handler(new HttpCallback);
        QWeakPointer<HttpCallback> weak(handler);
        *handler = [lane, guard, hasContext, identity, parse, done, name, transport, request, attempts, weak, visitor](const HttpReply &http) {
            if (http.status == 400 && identity) identity->reportHttp400();
            if (hasContext && !guard) return;
            if (http.truncated && *attempts == 0) {
                ++*attempts;
                QSharedPointer<HttpCallback> again = weak.toStrongRef();
                if (again) {
                    qInfo("sailtube-timing innertube %s truncated retry=1", name.constData());
                    transport->send(request, guard.data(), *again);
                    return;
                }
            }
            // Stage timing (NFR-PERF-09): network, then parse on the lane, then delivery.
            const qint64 net = http.elapsedMs;
            const int bytes = http.body.size();
            const int status = http.status;
            QSharedPointer<QElapsedTimer> sinceReply(new QElapsedTimer);
            sinceReply->start();
            QSharedPointer<qint64> parseMs(new qint64(-1));
            std::function<T()> work = [http, parse, parseMs, visitor] {
                QElapsedTimer t;
                t.start();
                const InnerTubeReply reply = toReply(http);
                remember(visitor, reply.json);
                T result = parse(reply);
                *parseMs = t.elapsed();
                return result;
            };
            std::function<void(T)> deliver = [done, name, net, bytes, status, sinceReply, parseMs](T result) {
                const qint64 queued = sinceReply->elapsed();
                done(result);
                logStage(name, net, *parseMs, queued, bytes, status);
            };
            if (lane) lane->run<T>(guard.data(), work, deliver);
            else deliver(work());
        };
        // The callback keeps itself alive until the last attempt has answered.
        HttpCallback first = [handler](const HttpReply &http) { (*handler)(http); };
        return transport_->send(request, context, first);
    }
    static void remember(const std::shared_ptr<VisitorMemory> &memory, const QJsonObject &answer);

    HttpTransport *transport_;
    ClientIdentity *identity_;
    WorkLane *lane_;
    DiscoveryLocale locale_;
    std::shared_ptr<VisitorMemory> visitor_;
};

}}}
