// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "HttpsConnection.h"
#include "../../Beta/ServerDate.h"
#include <QNetworkProxy>
#include <QSslError>

namespace nordtile { namespace music { namespace sharedplayback { namespace infrastructure {
namespace {
bool headerTextOk(const std::string &text) {
    for (std::size_t i = 0; i < text.size(); ++i) {
        const unsigned char c = static_cast<unsigned char>(text[i]);
        if (c == '\r' || c == '\n' || c == 0) return false;
    }
    return text.size() <= 8192;
}
}

HttpsConnection::HttpsConnection(const QString &host, quint16 port, bool ipv4Only, QObject *parent)
    : QObject(parent), host_(host), port_(port), ipv4_(ipv4Only), parser_() {
    timeout_.setSingleShot(true);
    idle_.setSingleShot(true);
    reuse_.setSingleShot(true);
    connect(&reuse_, &QTimer::timeout, this, &HttpsConnection::onReuseSilence);
    connect(&timeout_, &QTimer::timeout, this, &HttpsConnection::onTimeout);
    connect(&idle_, &QTimer::timeout, this, &HttpsConnection::onIdle);
}

HttpsConnection::~HttpsConnection() {
    if (socket_) { socket_->disconnect(this); socket_->abort(); }
}

bool HttpsConnection::connected() const {
    return socket_ && encrypted_ && socket_->state() == QAbstractSocket::ConnectedState;
}

QByteArray HttpsConnection::serialize(const HttpsRequest &request) const {
    QByteArray out;
    out += QByteArray::fromStdString(request.method) + ' ' + QByteArray::fromStdString(request.path) + " HTTP/1.1\r\n";
    out += "Host: " + host_.toLatin1() + "\r\n";
    for (std::size_t i = 0; i < request.headers.size(); ++i)
        out += QByteArray::fromStdString(request.headers[i].first) + ": " + QByteArray::fromStdString(request.headers[i].second) + "\r\n";
    if (!request.body.isEmpty() || request.method == "POST") out += "Content-Length: " + QByteArray::number(request.body.size()) + "\r\n";
    out += "Connection: keep-alive\r\n\r\n";
    out += request.body;
    return out;
}

void HttpsConnection::send(const HttpsRequest &request, const Callback &callback) {
    HttpsResult bad;
    bad.statusClass = "bad";
    bool valid = request.path.size() > 0 && request.path.size() <= 8192 && request.path[0] == '/' && headerTextOk(request.path) &&
                 headerTextOk(request.method);
    for (std::size_t i = 0; valid && i < request.headers.size(); ++i)
        valid = !request.headers[i].first.empty() && headerTextOk(request.headers[i].first) && headerTextOk(request.headers[i].second);
    if (!valid || active_ || !callback) {
        if (callback) { bad.statusClass = active_ ? "busy" : "bad"; QPointer<HttpsConnection> guard(this); QTimer::singleShot(0, this, [callback, bad]() { callback(bad); }); }
        return;
    }
    request_ = request;
    callback_ = callback;
    active_ = true;
    paused_ = false;
    retriedFresh_ = false;
    idle_.stop();
    parser_ = domain::HttpResponseParser(request.maxBodyBytes, 64 * 1024, request.headersOnly, bool(request.onBody));
    headersReported_ = false;
    timeout_.start(request.timeoutMs);
    if (connected()) { reusedForThis_ = true; writeRequest(); if (reuseFirstByteMs_ > 0) reuse_.start(reuseFirstByteMs_); }
    else { reusedForThis_ = false; startOnFreshSocket(); }
}

void HttpsConnection::dropSocket() {
    encrypted_ = false;
    if (!socket_) return;
    socket_->disconnect(this);
    socket_->abort();
    socket_->deleteLater();
    socket_ = nullptr;
}

void HttpsConnection::startOnFreshSocket() {
    dropSocket();
    socket_ = new QSslSocket(this);
    socket_->setProxy(QNetworkProxy::NoProxy);
    // No Qt read-buffer limit: Qt 5.6 is reported to crash intermittently when a socket with one is aborted. A
    // paused reader therefore buffers at most the rest of the chunk in flight (the proxy asks for 10 MiB at a time).
    connect(socket_, &QSslSocket::encrypted, this, &HttpsConnection::onEncrypted);
    connect(socket_, &QIODevice::readyRead, this, &HttpsConnection::onReadyRead);
    connect(socket_, &QAbstractSocket::disconnected, this, &HttpsConnection::onDisconnected);
    connect(socket_, static_cast<void (QAbstractSocket::*)(QAbstractSocket::SocketError)>(&QAbstractSocket::error),
            this, &HttpsConnection::onSocketError);
    if (ignoreSsl_)
        connect(socket_, static_cast<void (QSslSocket::*)(const QList<QSslError> &)>(&QSslSocket::sslErrors), this,
                [this](const QList<QSslError> &) { if (socket_) socket_->ignoreSslErrors(); });
    // IPv4 on request: the address family of this connection is the family the provider binds to.
    socket_->connectToHostEncrypted(host_, port_, QIODevice::ReadWrite,
                                    ipv4_ ? QAbstractSocket::IPv4Protocol : QAbstractSocket::AnyIPProtocol);
}

void HttpsConnection::onEncrypted() {
    encrypted_ = true;
    // Let the kernel notice a peer that vanished while the connection idles.
    if (socket_) socket_->setSocketOption(QAbstractSocket::KeepAliveOption, 1);
    if (active_) writeRequest();
}

void HttpsConnection::writeRequest() {
    if (!socket_) return;
    socket_->write(serialize(request_));
}

void HttpsConnection::setPaused(bool paused) {
    if (paused_ == paused) return;
    paused_ = paused;
    // The time-out is about silence from the peer: it does not run while the reader is the slow side.
    if (active_) { if (paused_) timeout_.stop(); else timeout_.start(request_.timeoutMs); }
    if (!paused_ && socket_ && active_ && socket_->bytesAvailable() > 0) QTimer::singleShot(0, this, [this]() { if (!paused_) onReadyRead(); });
}

void HttpsConnection::onReadyRead() {
    if (!socket_ || !active_) { if (socket_) socket_->readAll(); return; }
    if (paused_) return;                 // leave the bytes in the socket: TCP pushes back on the sender
    const QByteArray data = socket_->readAll();
    sawBytes_ = sawBytes_ || !data.isEmpty();
    if (sawBytes_) reuse_.stop();
    if (!data.isEmpty()) timeout_.start(request_.timeoutMs);   // inactivity, not total time: a long body keeps it alive
    const domain::HttpResponseParser::State state = parser_.feed(data.constData(), std::size_t(data.size()));
    HttpsResult result;
    result.connectionReused = reusedForThis_;
    if (state == domain::HttpResponseParser::Error) {
        result.statusClass = "bad"; finish(result, false); return;
    }
    if (parser_.headersComplete() && !headersReported_) {
        headersReported_ = true;
        { const std::string serverDate = parser_.header("date");   // a beta's clock floor; nothing in other builds
          if (!serverDate.empty()) nordtile::music::beta::noteServerDate(serverDate.data(), serverDate.size()); }
        if (request_.onHeaders && !request_.onHeaders(parser_.statusCode(), parser_.headers())) {
            // What the sink refused is still reported: a redirect's target is how the caller follows it.
            result.statusClass = "aborted"; result.status = parser_.statusCode();
            result.location = parser_.header("location"); result.contentRange = parser_.header("content-range");
            finish(result, false); return;
        }
    }
    if (request_.onBody && !request_.headersOnly) {
        const QByteArray part = QByteArray::fromStdString(parser_.takeBody());
        if (!part.isEmpty() && !request_.onBody(part)) {
            result.statusClass = "aborted"; result.status = parser_.statusCode(); finish(result, false); return;
        }
        if (!active_) return;            // the sink finished or aborted this request itself
    }
    if (state == domain::HttpResponseParser::Done || (state == domain::HttpResponseParser::HeadersDone && request_.headersOnly)) {
        result.transportOk = true;
        result.status = parser_.statusCode();
        result.body = QByteArray::fromStdString(parser_.body());
        result.location = parser_.header("location");
        result.contentRange = parser_.header("content-range");
        // A headers-only answer leaves unread body on the wire: the connection cannot be reused.
        finish(result, state == domain::HttpResponseParser::Done && parser_.keepAlive());
    }
}

void HttpsConnection::onDisconnected() {
    const bool wasActive = active_;
    encrypted_ = false;
    if (!wasActive) return;
    if (parser_.headersComplete() && parser_.closed() == domain::HttpResponseParser::Done) {
        HttpsResult result; result.transportOk = true; result.status = parser_.statusCode();
        result.body = QByteArray::fromStdString(parser_.body()); result.connectionReused = reusedForThis_;
        finish(result, false); return;
    }
    // A pooled connection that died while idle: the first sign is a close before any byte of the answer.
    if (reusedForThis_ && !retriedFresh_ && !sawBytes_) {
        retriedFresh_ = true; reusedForThis_ = false; startOnFreshSocket(); return;
    }
    HttpsResult result; result.statusClass = "closed"; result.connectionReused = reusedForThis_;
    finish(result, false);
}

void HttpsConnection::onSocketError() {
    if (!active_) { dropSocket(); return; }
    if (reusedForThis_ && !retriedFresh_ && !sawBytes_) {
        retriedFresh_ = true; reusedForThis_ = false; startOnFreshSocket(); return;
    }
    HttpsResult result;
    result.statusClass = encrypted_ ? "closed" : (socket_ && socket_->error() == QAbstractSocket::SslHandshakeFailedError ? "tls" : "connect");
    result.connectionReused = reusedForThis_;
    finish(result, false);
}

void HttpsConnection::onReuseSilence() {
    if (!active_ || !reusedForThis_ || sawBytes_ || retriedFresh_) return;
    retriedFresh_ = true; reusedForThis_ = false;
    timeout_.start(request_.timeoutMs);
    startOnFreshSocket();
}

void HttpsConnection::onTimeout() {
    if (!active_) return;
    HttpsResult result; result.statusClass = "timeout"; result.connectionReused = reusedForThis_;
    finish(result, false);
}

void HttpsConnection::onIdle() {
    if (!active_) dropSocket();
}

void HttpsConnection::abort() {
    if (active_) {
        HttpsResult result; result.statusClass = "closed";
        finish(result, false);
    } else {
        dropSocket();
    }
}

void HttpsConnection::finish(const HttpsResult &result, bool keep) {
    if (!active_) return;
    active_ = false;
    paused_ = false;
    sawBytes_ = false;
    timeout_.stop();
    reuse_.stop();
    Callback callback = callback_;
    callback_ = Callback();
    if (keep && socket_) idle_.start(idleMs_);
    else dropSocket();
    if (callback) callback(result);
}

}}}}
