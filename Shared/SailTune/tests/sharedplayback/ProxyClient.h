// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// A plain HTTP client for the loopback media proxy in tests (a player: one request, read until
// the proxy closes) and a deterministic media payload. Header-only; needs FakeProvider.h first.
#pragma once
#include <QCoreApplication>
#include <QElapsedTimer>
#include <QTcpSocket>

struct Reply { bool ok = false; int status = 0; QMap<QByteArray, QByteArray> headers; QByteArray body; bool closedByPeer = false; };

// A player: one plain TCP request to the proxy, read until it closes.
__attribute__((unused)) static Reply fetch(quint16 port, const QByteArray &path, const QByteArray &range = QByteArray(), const QByteArray &method = "GET",
                   int stopAfterBytes = -1, int timeoutMs = 8000) {
    Reply reply;
    QTcpSocket socket;
    socket.connectToHost(QHostAddress::LocalHost, port);
    if (!socket.waitForConnected(3000)) return reply;
    QByteArray request = method + " " + path + " HTTP/1.1\r\nHost: 127.0.0.1\r\nUser-Agent: test-player\r\n";
    if (!range.isEmpty()) request += "Range: " + range + "\r\n";
    request += "\r\n";
    socket.write(request);
    QByteArray all;
    bool closed = false;
    QObject::connect(&socket, &QAbstractSocket::disconnected, [&]() { closed = true; });
    QElapsedTimer timer; timer.start();
    for (;;) {
        socket.waitForReadyRead(20);
        all += socket.readAll();
        const int end = all.indexOf("\r\n\r\n");
        if (stopAfterBytes >= 0 && end >= 0 && all.size() - end - 4 >= stopAfterBytes) { socket.abort(); break; }
        if (closed || socket.state() == QAbstractSocket::UnconnectedState) break;
        if (timer.elapsed() > timeoutMs) return reply;
        QCoreApplication::processEvents();
    }
    all += socket.readAll();
    const int end = all.indexOf("\r\n\r\n");
    if (end < 0) return reply;
    const QList<QByteArray> lines = all.left(end).split('\n');
    reply.status = lines.value(0).split(' ').value(1).toInt();
    for (int i = 1; i < lines.size(); ++i) {
        const int colon = lines.at(i).indexOf(':');
        if (colon > 0) reply.headers.insert(lines.at(i).left(colon).trimmed().toLower(), lines.at(i).mid(colon + 1).trimmed());
    }
    reply.body = all.mid(end + 4);
    reply.ok = true;
    reply.closedByPeer = closed;
    return reply;
}

static QByteArray makeMedia(int size) {
    QByteArray data(size, '\0');
    unsigned seed = 7;
    for (int i = 0; i < size; ++i) { seed = seed * 1103515245u + 12345u; data[i] = char((seed >> 16) & 0xff); }
    return data;
}

