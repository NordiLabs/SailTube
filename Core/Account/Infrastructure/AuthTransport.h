// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#pragma once

// The account's own HTTPS transport (ACC-06/08). It is never shared with
// anonymous discovery: its QNetworkAccessManager keeps no cookies of its own
// (the session is sent explicitly, rotations are folded by the service), it
// follows no redirect off www.youtube.com, and it returns Set-Cookie headers.
// Only https://www.youtube.com is accepted as a target.

#include <QByteArray>
#include <QList>
#include <QObject>
#include <QPair>
#include <QUrl>
#include <functional>
#include <vector>

class QNetworkAccessManager;

namespace nordtile { namespace video { namespace account {

struct AuthRequest {
    QUrl url;
    QByteArray body;                                  // POST when non-empty
    QList<QPair<QByteArray, QByteArray>> headers;
    int timeoutMs = 10000;
};

struct AuthReply {
    int status = 0;                 // 0 = network failure, timeout or refused target
    QByteArray body;
    std::vector<QByteArray> setCookies;
    bool timedOut = false;
};

class AuthTransport {
public:
    typedef std::function<void(const AuthReply &)> Done;
    virtual ~AuthTransport() {}
    // `done` runs on the caller's thread while `context` lives.
    virtual void send(const AuthRequest &request, QObject *context, Done done) = 0;
};

class NetworkAuthTransport final : public QObject, public AuthTransport {
    Q_OBJECT
public:
    explicit NetworkAuthTransport(QObject *parent = nullptr);
    void send(const AuthRequest &request, QObject *context, Done done) override;
    static bool allowedTarget(const QUrl &url);

private:
    QNetworkAccessManager *manager_;
};

}}}
