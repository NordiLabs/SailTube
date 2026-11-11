// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#pragma once

// ACC-11: the catalog's services signed in. Over a signed transport (library component's
// SignedCatalogTransport in the app, a fake in tests) it runs its own lane and InnerTubeClient,
// so signed requests never queue behind anonymous ones, and:
//  * switches HomeService between the signed client (recommended home, FEwhat_to_watch) and the
//    anonymous one (valid empty), reloading on every change;
//  * serves the account's own lists ("WL" Watch later, "LL" Liked) to PlaylistModel through its
//    PlaylistService (VLWL / VLLL);
//  * drops every personalised result on invalidate() (a sign-out or account switch).
// The transport never sees a credential from here; signed out it fails each send at once.

#include "Core/Catalog/Infrastructure/ClientIdentity.h"

#include <QObject>
#include <QScopedPointer>

namespace nordtile { namespace video {
namespace catalog { class HttpTransport; class InnerTubeClient; class HomeService; class PlaylistService; class ShortsFeedService; class WorkLane; class CatalogCounters; }
namespace sailfish {

class SignedCatalog final : public QObject {
    Q_OBJECT
public:
    SignedCatalog(catalog::HttpTransport *signedTransport, catalog::ClientIdentity *identity,
                  const catalog::DiscoveryLocale &locale, catalog::HomeService *home,
                  catalog::InnerTubeClient *anonymous, catalog::CatalogCounters *counters = nullptr,
                  QObject *parent = nullptr);
    ~SignedCatalog() override;

    // The account can sign (AccountService::canSign) or not any more.
    void setSignedIn(bool signedIn);
    bool signedIn() const { return signedIn_; }
    // The Shorts section reads the account's own Shorts sequence while signed in.
    void setShortsFeed(catalog::ShortsFeedService *shorts);
    // A sign-out or an account switch: personalised rows go, signed lists reload.
    void invalidate();

    catalog::InnerTubeClient *client() const;
    catalog::PlaylistService *playlists() const;

signals:
    void signedInChanged();

private:
    catalog::HomeService *home_;
    catalog::ShortsFeedService *shorts_ = nullptr;
    catalog::InnerTubeClient *anonymous_;
    QScopedPointer<catalog::WorkLane> lane_;
    QScopedPointer<catalog::InnerTubeClient> client_;
    QScopedPointer<catalog::PlaylistService> playlists_;
    bool signedIn_ = false;
};

}}}
