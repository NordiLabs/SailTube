// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#pragma once
// One keep-alive HTTPS/1.1 connection to one host, with the address family under the caller's
// control. Qt Core + Network only.
//
// Why not QNetworkAccessManager: it does not let the caller choose IPv4, and the provider binds a
// media address to the address family of the request that obtained it; the request for the
// address and the fetch of the media must use the same family. This class connects over IPv4
// when asked, sends exactly the headers it is given (nothing else: no cookies, no redirects, no
// compression unless asked for), reuses its connection for the next request, drops it after
// idleMs, and repeats a request once on a fresh connection when a reused one turned out dead.
#include "../Domain/HttpResponseParser.h"
#include <QByteArray>
#include <QObject>
#include <QPointer>
#include <QSslSocket>
#include <QTimer>
#include <functional>
#include <map>
#include <string>
#include <utility>
#include <vector>

namespace nordtile { namespace music { namespace sharedplayback { namespace infrastructure {

struct HttpsResult {
    bool transportOk = false;       // a complete (or headers-only, if asked) response arrived
    int status = 0;
    std::string statusClass;        // "" or a fixed class of the transport failure: connect, tls, timeout, closed, bad
    QByteArray body;
    std::string location;           // never followed, reported for diagnostics classes only
    std::string contentRange;       // the Content-Range header of the answer, when there was one
    bool connectionReused = false;
};

struct HttpsRequest {
    std::string method = "GET";
    std::string path;               // "/youtubei/v1/player?prettyPrint=false"
    std::vector<std::pair<std::string, std::string> > headers;
    QByteArray body;
    int timeoutMs = 10000;          // time without any byte from the peer
    std::size_t maxBodyBytes = 2 * 1024 * 1024;
    bool headersOnly = false;       // finish as soon as the header block has been read (a probe)
    // Streaming: the body is handed over as it arrives instead of being collected (HttpsResult::body
    // stays empty and maxBodyBytes only bounds what is waiting for the sink). Return false to stop:
    // the connection is dropped and the request ends as "aborted".
    std::function<bool(const QByteArray &)> onBody;
    // Called once with the status and headers (lower-case names) before the first body bytes.
    std::function<bool(int status, const std::map<std::string, std::string> &headers)> onHeaders;
};

class HttpsConnection final : public QObject {
    Q_OBJECT
public:
    typedef std::function<void(const HttpsResult &)> Callback;

    HttpsConnection(const QString &host, quint16 port, bool ipv4Only, QObject *parent = nullptr);
    ~HttpsConnection() override;

    bool busy() const { return active_; }
    bool connected() const;
    // One request at a time; the callback runs on the owning thread, exactly once.
    void send(const HttpsRequest &request, const Callback &callback);
    void abort();                   // drops the connection; an active request ends as "closed"
    // Flow control for a streaming body: while paused nothing is handed to the sink. What arrives
    // meanwhile stays in the socket's buffer (bounded by the length of the request), and setPaused(false)
    // delivers it.
    void setPaused(bool paused);
    bool paused() const { return paused_; }
    void setIdleTimeoutMs(int ms) { idleMs_ = ms; }
    // A reused connection that answers with no byte at all within this time is taken for dead (a carrier
    // dropped it silently, the network changed, the device slept) and the request is repeated once on a
    // fresh connection instead of waiting out the whole request timeout.
    void setReuseFirstByteTimeoutMs(int ms) { reuseFirstByteMs_ = ms; }
    // Test hook: skip certificate verification of a self-signed local server. Never set in the app.
    void setIgnoreSslErrorsForTesting(bool ignore) { ignoreSsl_ = ignore; }

private slots:
    void onEncrypted();
    void onReadyRead();
    void onDisconnected();
    void onSocketError();
    void onTimeout();
    void onIdle();
    void onReuseSilence();

private:
    void startOnFreshSocket();
    void writeRequest();
    void finish(const HttpsResult &result, bool keep);
    void dropSocket();
    QByteArray serialize(const HttpsRequest &request) const;

    QString host_;
    quint16 port_;
    bool ipv4_;
    int idleMs_ = 45000;
    int reuseFirstByteMs_ = 3000;
    bool ignoreSsl_ = false;
    QSslSocket *socket_ = nullptr;
    bool encrypted_ = false;
    bool active_ = false;
    bool reusedForThis_ = false;
    bool retriedFresh_ = false;
    bool sawBytes_ = false;
    bool paused_ = false;
    bool headersReported_ = false;
    HttpsRequest request_;
    Callback callback_;
    domain::HttpResponseParser parser_;
    QTimer timeout_;
    QTimer idle_;
    QTimer reuse_;
};

}}}}
