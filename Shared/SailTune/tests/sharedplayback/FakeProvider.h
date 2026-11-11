// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// A local TLS server that plays the provider for the shared playback tests: the visitor id
// endpoint, the player endpoint (scripted answers) and a media host for probes. Header-only so
// that other test programs can reuse it. The including file defines `failures`, `CHECK` and
// FIXTURE_DIR before including this.
#pragma once
#include "test_cert.h"
#include <QByteArray>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMap>
#include <QPointer>
#include <QSslCertificate>
#include <QSslKey>
#include <QSslSocket>
#include <QTcpServer>
#include <QTimer>
#include <deque>
#include <functional>
#include <memory>
#include <vector>

__attribute__((unused)) static QByteArray fixture(const char *name) {
    QFile file(QString::fromLatin1(FIXTURE_DIR) + QLatin1Char('/') + QString::fromLatin1(name));
    if (!file.open(QIODevice::ReadOnly)) { std::fprintf(stderr, "cannot open fixture %s\n", name); ++failures; return QByteArray(); }
    return file.readAll();
}

__attribute__((unused)) static bool waitFor(const std::function<bool()> &condition, int timeoutMs = 20000) {
    QElapsedTimer timer; timer.start();
    while (!condition()) {
        if (timer.elapsed() > timeoutMs) return false;
        QEventLoop loop; QTimer::singleShot(5, &loop, SLOT(quit())); loop.exec();
    }
    return true;
}

// ---- the fake provider -------------------------------------------------------------------------
struct Recorded { QByteArray method, path, body; QMap<QByteArray, QByteArray> headers; };

class FakeProvider : public QTcpServer {
public:
    // Scripts, consumed in order; the last entry repeats.
    std::deque<QByteArray> playerBodies;          // JSON bodies
    std::deque<int> playerStatuses;               // default 200
    std::deque<int> visitorStatuses;              // default 200
    std::deque<int> mediaStatuses;                // default 206; 0: close without answering
    bool chunkedPlayer = false;
    // The scripted player body is the fixture of video AAAAAAAAAAA: answer any requested id with it, id replaced.
    bool answerAnyVideoId = false;
    // Every player answer carries other signed addresses than the one before, as the provider's do.
    bool varyAddresses = false;
    int playerAnswers = 0;
    // Real media: when set, /videoplayback honours Range against these bytes. Scripted actions are
    // consumed one per media request (the last repeats): 0 serve, 403 refuse, 1 drop the connection
    // without answering, -N serve N bytes of the answer and then drop the connection.
    QByteArray mediaData;
    std::deque<int> mediaActions;
    bool closeAfterEveryAnswer = false;
    // Probe answers (no real media): the media request with this index (0 = the first) and every later one is never answered,
    // so that a test can act while a probe is outstanding without racing the answer. -1: all are answered.
    int silentMediaFrom = -1;
    // The Location of 3xx answers of the media host (probe statuses, and the action 302 of real media): another node of the CDN.
    QByteArray redirectLocation;
    int redirectsSent = 0;
    // Real media only: every answer is held back this long, as a slow origin would (a seek during the pre-buffer, a read that has to wait).
    int mediaDelayMs = 0;
    int mediaSeen = 0;
    // A connection that has answered once never answers again (a carrier dropped it silently): requests on it hang.
    bool silentAfterFirstAnswer = false;
    // Probe answers (no real media): the Content-Range total. 0: the length the probe asked for (a range below 10 MiB covers all of it).
    qint64 probeTotal = 0;
    // Real media answers: claim this total in Content-Range instead of the length of mediaData (0: the real one).
    qint64 mediaTotalOverride = 0;
    std::vector<Recorded> requests;
    int connectionsAccepted = 0;

    int count(const char *pathPrefix) const {
        int n = 0;
        for (std::size_t i = 0; i < requests.size(); ++i) if (requests[i].path.startsWith(pathPrefix)) ++n;
        return n;
    }
    const Recorded *last(const char *pathPrefix) const {
        for (std::size_t i = requests.size(); i-- > 0;) if (requests[i].path.startsWith(pathPrefix)) return &requests[i];
        return nullptr;
    }

protected:
    void incomingConnection(qintptr fd) override {
        ++connectionsAccepted;
        QSslSocket *socket = new QSslSocket(this);
        socket->setSocketDescriptor(fd);
        socket->setLocalCertificate(QSslCertificate(QByteArray(kTestCertPem)));
        socket->setPrivateKey(QSslKey(QByteArray(kTestKeyPem), QSsl::Rsa));
        const std::shared_ptr<QByteArray> buffer(new QByteArray);   // shared: abort() runs the disconnect handler inside respond()
        const std::shared_ptr<int> answered(new int(0));
        connect(socket, &QIODevice::readyRead, this, [this, socket, buffer, answered]() {
            buffer->append(socket->readAll());
            for (;;) {
                const int end = buffer->indexOf("\r\n\r\n");
                if (end < 0) return;
                Recorded request;
                const QList<QByteArray> lines = buffer->left(end).split('\n');
                const QList<QByteArray> first = lines.value(0).trimmed().split(' ');
                request.method = first.value(0); request.path = first.value(1);
                for (int i = 1; i < lines.size(); ++i) {
                    const int colon = lines.at(i).indexOf(':');
                    if (colon > 0) request.headers.insert(lines.at(i).left(colon).trimmed().toLower(), lines.at(i).mid(colon + 1).trimmed());
                }
                const int length = request.headers.value("content-length").toInt();
                if (buffer->size() < end + 4 + length) return;
                request.body = buffer->mid(end + 4, length);
                buffer->remove(0, end + 4 + length);
                requests.push_back(request);
                if (silentAfterFirstAnswer && (*answered)++ >= 1) continue;
                respond(socket, request);
                if (socket->state() != QAbstractSocket::ConnectedState) return;
            }
        });
        connect(socket, &QAbstractSocket::disconnected, this, [socket]() { socket->deleteLater(); });
        socket->startServerEncryption();
    }

private:
    // Every redirect points at a new address, as the provider's do (a redirect to the address just asked for would be a loop).
    QByteArray nextRedirect() {
        return redirectLocation + (redirectLocation.contains('?') ? "&" : "?") + "hop=" + QByteArray::number(++redirectsSent);
    }
    static QByteArray take(std::deque<QByteArray> &queue, const QByteArray &fallback) {
        if (queue.empty()) return fallback;
        QByteArray value = queue.front();
        if (queue.size() > 1) queue.pop_front();
        return value;
    }
    static int takeInt(std::deque<int> &queue, int fallback) {
        if (queue.empty()) return fallback;
        const int value = queue.front();
        if (queue.size() > 1) queue.pop_front();
        return value;
    }
    void write(QSslSocket *socket, int status, const QByteArray &headers, const QByteArray &body, bool chunked = false) {
        QByteArray out = "HTTP/1.1 " + QByteArray::number(status) + " X\r\n" + headers;
        if (chunked) {
            out += "Transfer-Encoding: chunked\r\n\r\n";
            for (int at = 0; at < body.size(); at += 1000) {
                const QByteArray part = body.mid(at, 1000);
                out += QByteArray::number(part.size(), 16) + "\r\n" + part + "\r\n";
            }
            out += "0\r\n\r\n";
        } else {
            out += "Content-Length: " + QByteArray::number(body.size()) + "\r\n\r\n" + body;
        }
        socket->write(out);
        if (closeAfterEveryAnswer) socket->disconnectFromHost();
    }
    void respond(QSslSocket *socket, const Recorded &request) {
        if (request.path.startsWith("/youtubei/v1/visitor_id")) {
            const int status = takeInt(visitorStatuses, 200);
            if (status != 200) { write(socket, status, "Content-Type: application/json\r\n", "{}"); return; }
            write(socket, 200, "Content-Type: application/json\r\n", "{\"responseContext\":{\"visitorData\":\"FAKEVISITOR%3D%3D\"}}");
        } else if (request.path.startsWith("/youtubei/v1/player")) {
            const int status = takeInt(playerStatuses, 200);
            QByteArray body = take(playerBodies, QByteArray("{}"));
            if (answerAnyVideoId) {
                const QByteArray id = QJsonDocument::fromJson(request.body).object().value(QStringLiteral("videoId")).toString().toLatin1();
                if (id.size() == 11) body.replace("AAAAAAAAAAA", id);
            }
            if (varyAddresses) body.replace("SYNTHETIC", "SYNTHETIC" + QByteArray::number(++playerAnswers));
            write(socket, status, "Content-Type: application/json\r\n", body, chunkedPlayer);
        } else if (request.path.startsWith("/videoplayback") && !mediaData.isEmpty()) {
            const int action = takeInt(mediaActions, 0);
            if (action == 403) { write(socket, 403, "", ""); return; }
            if (action == 302) { write(socket, 302, "Location: " + nextRedirect() + "\r\n", ""); return; }
            if (action == 1) { socket->abort(); return; }
            const QByteArray range = request.headers.value("range");
            qint64 from = 0, to = mediaData.size() - 1;
            if (range.startsWith("bytes=")) {
                const QList<QByteArray> parts = range.mid(6).split('-');
                from = parts.value(0).toLongLong();
                if (!parts.value(1).isEmpty()) to = qMin<qint64>(parts.value(1).toLongLong(), mediaData.size() - 1);
            }
            if (from >= mediaData.size()) { write(socket, 416, "Content-Range: bytes */" + QByteArray::number(mediaData.size()) + "\r\n", ""); return; }
            const QByteArray body = mediaData.mid(int(from), int(to - from + 1));
            QByteArray head = "HTTP/1.1 206 Partial Content\r\nContent-Type: application/octet-stream\r\nContent-Range: bytes " +
                QByteArray::number(from) + "-" + QByteArray::number(to) + "/" + QByteArray::number(mediaTotalOverride > 0 ? mediaTotalOverride : qint64(mediaData.size())) +
                "\r\nContent-Length: " + QByteArray::number(body.size()) + "\r\n\r\n";
            if (action < 0) {
                socket->write(head + body.left(-action));
                socket->flush();
                socket->abort();
                return;
            }
            if (mediaDelayMs > 0) {
                const QPointer<QSslSocket> held(socket);
                const QByteArray answer = head + body;
                QTimer::singleShot(mediaDelayMs, this, [held, answer]() { if (held) held->write(answer); });
                return;
            }
            socket->write(head + body);
            if (closeAfterEveryAnswer) socket->disconnectFromHost();
        } else if (request.path.startsWith("/videoplayback")) {
            if (silentMediaFrom >= 0 && mediaSeen++ >= silentMediaFrom) return;
            const int status = takeInt(mediaStatuses, 206);
            if (status == 0) { socket->abort(); return; }
            qint64 total = probeTotal;
            if (total == 0) {
                const QByteArray range = request.headers.value("range");
                const qint64 last = range.startsWith("bytes=0-") ? range.mid(8).toLongLong() : -1;
                total = (last >= 0 && last + 1 < 10 * 1024 * 1024) ? last + 1 : 20000000;
            }
            QByteArray headers = "Content-Type: application/octet-stream\r\nContent-Range: bytes 0-1023/" + QByteArray::number(total) + "\r\n";
            if (status >= 300 && status < 400 && !redirectLocation.isEmpty()) headers += "Location: " + nextRedirect() + "\r\n";
            write(socket, status, headers, status < 300 ? QByteArray(1024, 'x') : QByteArray());
        } else {
            write(socket, 404, "", "");
        }
    }
};

