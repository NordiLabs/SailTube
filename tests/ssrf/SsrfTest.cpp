// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// NFR-SEC-02 / RES-17 option (b): SailTube's SSRF test set against the pinned loopback media proxy
// (Shared/SailTune/Core/SharedPlayback/Infrastructure/MediaProxy). Named cases:
//   lookalike_hosts, plain_http, local_and_private_targets, userinfo_port_fragment, bound_to_loopback,
//   request_without_token, request_with_wrong_token, token_unguessable.
// The redirect-off-the-allow-list case runs in SailTune's own suite (tests/sharedplayback,
// MediaProxyTest "redirectLocation = https://evil.example.com"), pinned and run unchanged here.
#include "../../Shared/SailTune/Core/SharedPlayback/Infrastructure/MediaProxy.h"

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QNetworkInterface>
#include <QTcpSocket>
#include <cstdio>
#include <set>
#include <string>

using nordtile::music::sharedplayback::infrastructure::MediaProxy;
static int failures = 0;
#define CHECK(cond) do { if (!(cond)) { std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); ++failures; } } while (0)
#define CASE(name) std::printf("ssrf: %s\n", name)

static MediaProxy::Source source(const std::string &url) {
    MediaProxy::Source s;
    s.url = url;
    s.userAgent = "test";
    s.contentLength = 1000;
    s.mimeType = "video/mp4";
    return s;
}

static int statusOf(quint16 port, const QByteArray &path, QByteArray *body) {
    QTcpSocket socket;
    socket.connectToHost(QHostAddress::LocalHost, port);
    if (!socket.waitForConnected(2000)) return -1;
    socket.write("GET " + path + " HTTP/1.1\r\nHost: 127.0.0.1\r\nRange: bytes=0-99\r\n\r\n");
    QByteArray reply;
    QElapsedTimer t; t.start();
    while (t.elapsed() < 3000 && !reply.contains("\r\n\r\n")) {
        if (!socket.waitForReadyRead(200)) QCoreApplication::processEvents();
        reply += socket.readAll();
    }
    while (socket.waitForReadyRead(200)) reply += socket.readAll();
    *body = reply.mid(reply.indexOf("\r\n\r\n") + 4);
    const QList<QByteArray> first = reply.left(reply.indexOf("\r\n")).split(' ');
    return first.size() >= 2 ? first.at(1).toInt() : -1;
}

int main(int argc, char **argv) {
    QCoreApplication app(argc, argv);
    MediaProxy::Options options;
    MediaProxy proxy(options);
    const MediaProxy::Refresher none = [](const std::function<void(const MediaProxy::FreshSource &)> &deliver) {
        deliver(MediaProxy::FreshSource());
    };
    const std::string id = "dQw4w9WgXcQ";

    CASE("lookalike_hosts");
    for (const char *url : {"https://googlevideo.com.evil.com/videoplayback", "https://evilgooglevideo.com/videoplayback",
                            "https://googlevideo.com/videoplayback",     // the bare suffix is no CDN node
                            "https://rr1.googlevideo.com.attacker.net/videoplayback", "https://rr1.googlevideo.co/videoplayback",
                            "https://rr1.googlevideo.com%2eevil.com/videoplayback", "https://rr1.goog1evideo.com/videoplayback"}) {
        CHECK(!MediaProxy::allowedAddress(url));
        CHECK(proxy.open(id, source(url), none).isEmpty());
    }
    CASE("plain_http");
    CHECK(!MediaProxy::allowedAddress("http://rr1---sn-x.googlevideo.com/videoplayback"));
    CHECK(!MediaProxy::allowedAddress("ws://rr1---sn-x.googlevideo.com/videoplayback"));
    CASE("local_and_private_targets");
    for (const char *url : {"https://localhost/videoplayback", "https://127.0.0.1/videoplayback", "https://[::1]/videoplayback",
                            "https://0.0.0.0/videoplayback", "https://192.168." "1.1/videoplayback", "https://10.0." "0.1/videoplayback",
                            "https://169.254.169.254/latest/meta-data", "https://localhost.googlevideo.com.localhost/x"})
        CHECK(!MediaProxy::allowedAddress(url));
    CASE("userinfo_port_fragment");
    CHECK(!MediaProxy::allowedAddress("https://user@rr1.googlevideo.com/videoplayback"));
    CHECK(!MediaProxy::allowedAddress("https://rr1.googlevideo.com@evil.com/videoplayback"));
    CHECK(!MediaProxy::allowedAddress("https://rr1.googlevideo.com:8443/videoplayback"));
    CHECK(!MediaProxy::allowedAddress("https://rr1.googlevideo.com/videoplayback#x"));
    CHECK(MediaProxy::allowedAddress("https://rr1---sn-x.googlevideo.com/videoplayback?expire=1"));

    CASE("bound_to_loopback");
    const QUrl url = proxy.open(id, source("https://rr1---sn-x.googlevideo.com/videoplayback?expire=1"), none);
    CHECK(url.isValid() && url.host() == QLatin1String("127.0.0.1") && url.port() == proxy.port() && proxy.port() > 0);
    {
        QTcpSocket viaLan;   // not reachable on another local address
        bool other = false;
        for (const QHostAddress &a : QNetworkInterface::allAddresses()) {
            if (a.isLoopback() || a.protocol() != QAbstractSocket::IPv4Protocol) continue;
            viaLan.connectToHost(a, proxy.port());
            other = viaLan.waitForConnected(500);
            viaLan.abort();
            if (other) break;
        }
        CHECK(!other);
    }
    CASE("request_without_token");
    QByteArray body;
    const int noToken = statusOf(proxy.port(), "/", &body);
    CHECK(noToken == 403 || noToken == 404);
    CHECK(body.size() < 64);
    const int noTokenS = statusOf(proxy.port(), "/s/", &body);
    CHECK(noTokenS == 403 || noTokenS == 404);
    CASE("request_with_wrong_token");
    const QByteArray good = url.path().toLatin1();
    QByteArray wrong = good; wrong[wrong.size() - 1] = wrong.at(wrong.size() - 1) == 'a' ? 'b' : 'a';
    const int wrongToken = statusOf(proxy.port(), wrong, &body);
    CHECK(wrongToken == 403 || wrongToken == 404);
    CHECK(statusOf(proxy.port(), "/s/../s/" + good.mid(3), &body) != 206);
    std::printf("ssrf: refused requests answer %d (no token) and %d (wrong token); RES-17 asks for 403\n", noToken, wrongToken);
    CASE("token_unguessable");
    std::set<QString> tokens;
    for (int i = 0; i < 20; ++i) {
        const QUrl u = proxy.open(id, source("https://rr1---sn-x.googlevideo.com/videoplayback?expire=1&i=" + std::to_string(i)), none);
        const QString token = u.path().mid(3);
        CHECK(token.size() >= 32);   // >= 128 bits as hex
        tokens.insert(token);
    }
    CHECK(tokens.size() == 20);
    proxy.closeAll();
    if (failures) { std::printf("ssrf: %d failure(s)\n", failures); return 1; }
    std::printf("ssrf SsrfTest: all checks passed\n");
    return 0;
}
