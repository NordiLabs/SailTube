// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#pragma once

// ACC-11 hook: a catalog::HttpTransport whose requests are sent signed by the
// account (AccountService::sendSigned). The catalog component builds an
// InnerTubeClient over it to run its existing services signed in (recommended
// home, Watch Later "VLWL", Liked "VLLL") without knowing any credential.
// Only https://www.youtube.com targets are sent; signed out, every request
// fails at once (status 0) so callers fall back to the anonymous path.

#include "Core/Catalog/Infrastructure/HttpTransport.h"
#include <QObject>
#include <QSet>

namespace nordtile { namespace video { namespace account {

class AccountService;

class SignedCatalogTransport final : public QObject, public catalog::HttpTransport {
    Q_OBJECT
public:
    explicit SignedCatalogTransport(AccountService *service, QObject *parent = nullptr);
    quint64 send(const catalog::HttpRequest &request, QObject *context, catalog::HttpCallback done) override;
    void cancel(quint64 id) override;
    QStringList contactedHosts() const override { return hosts_; }

private:
    AccountService *service_;
    quint64 next_ = 0;
    QSet<quint64> cancelled_;
    QStringList hosts_;
};

}}}
