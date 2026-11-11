// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#pragma once

// HTTP for discovery. Discovery owns its transports: they never share a connection pool or a
// queue with the playback component (RES-02). The app creates two: "interactive" (search,
// suggestions, watch page, channel first page) and "background" (feed refresh, comment walks,
// Shorts classification), so a 50-channel refresh cannot delay a search either.
//
// No cookies are stored. Requests that need one (the consent cookie of DSC-02) set it as a
// header. Tests substitute a fake transport that serves recorded fixtures.

#include <QByteArray>
#include <QList>
#include <QMap>
#include <QObject>
#include <QPair>
#include <QPointer>
#include <QString>
#include <QUrl>
#include <functional>

class QNetworkAccessManager;
class QNetworkReply;

namespace nordtile { namespace video { namespace catalog {

struct HttpRequest {
    QUrl url;
    QByteArray method = "GET";   // GET or POST
    QByteArray body;
    QList<QPair<QByteArray, QByteArray>> headers;
    int timeoutMs = 10000;
};

struct HttpReply {
    int status = 0;              // 0 when the request failed below HTTP
    QByteArray body;
    QString errorText;           // transport error text (never shown with URLs or tokens)
    bool timedOut = false;
    bool cancelled = false;
    // NFR-ROB-01: the body was cut off (the connection ended before the announced length, or Qt
    // reported an error on a 2xx reply). status is then 0: a network failure, retried once by
    // InnerTubeClient, never counted as shape drift (DIA-01).
    bool truncated = false;
    QUrl finalUrl;
    qint64 elapsedMs = 0;
};

using HttpCallback = std::function<void(const HttpReply &)>;

class HttpTransport {
public:
    virtual ~HttpTransport() {}
    // Sends asynchronously. `done` runs on the transport's thread, and only while `context`
    // (when given) is still alive. Returns a request id (> 0).
    virtual quint64 send(const HttpRequest &request, QObject *context, HttpCallback done) = 0;
    // Cancels a pending request; its callback runs with `cancelled` set, or not at all.
    virtual void cancel(quint64 id) = 0;
    // Host inventory for the privacy test (NFR-SEC-01): every host this transport contacted.
    virtual QStringList contactedHosts() const = 0;
    // Opens the TCP + TLS connection to `host` ahead of a request the user is about to make
    // (typing in search, app back in front), so the request itself skips the handshake.
    // Sends nothing at the HTTP level. Default: no-op (fakes).
    virtual void warm(const QString &host) { Q_UNUSED(host); }
};

// QNetworkAccessManager-backed transport. Lives on the thread that created it; I/O runs in
// Qt's network thread, so callbacks arrive on the owner's thread and parsing is handed to a
// WorkLane by the services.
class NetworkTransport final : public QObject, public HttpTransport {
    Q_OBJECT
public:
    explicit NetworkTransport(const QString &name, QObject *parent = nullptr);
    ~NetworkTransport() override;
    quint64 send(const HttpRequest &request, QObject *context, HttpCallback done) override;
    void cancel(quint64 id) override;
    QStringList contactedHosts() const override;
    // At most once per kWarmIntervalMs per host (an idle connection stays open about as long).
    void warm(const QString &host) override;
    static const int kWarmIntervalMs = 60000;
    QString name() const { return name_; }

private:
    struct Pending;
    void finish(quint64 id, bool timedOut);
    QString name_;
    QNetworkAccessManager *manager_;
    QList<Pending *> pending_;
    quint64 next_ = 1;
    QStringList hosts_;
    QMap<QString, qint64> warmed_;   // host -> monotonic ms of the last warm
};

// Desktop Chrome UA for www.youtube.com WEB calls, RSS and suggestions.
QByteArray webUserAgent();

}}}
