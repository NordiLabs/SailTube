// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "SignedCatalog.h"

#include "PlaylistModel.h"
#include "Core/Catalog/Infrastructure/HomeService.h"
#include "Core/Catalog/Infrastructure/InnerTubeClient.h"
#include "Core/Catalog/Infrastructure/PlaylistService.h"
#include "Core/Catalog/Infrastructure/ShortsFeedService.h"
#include "Core/Catalog/Infrastructure/WorkLane.h"

namespace nordtile { namespace video { namespace sailfish {

SignedCatalog::SignedCatalog(catalog::HttpTransport *signedTransport, catalog::ClientIdentity *identity,
                             const catalog::DiscoveryLocale &locale, catalog::HomeService *home,
                             catalog::InnerTubeClient *anonymous, catalog::CatalogCounters *counters, QObject *parent)
    : QObject(parent), home_(home), anonymous_(anonymous),
      lane_(new catalog::WorkLane(QStringLiteral("discovery-signed"), 1)),
      client_(new catalog::InnerTubeClient(signedTransport, identity, lane_.data(), locale)),
      playlists_(new catalog::PlaylistService(client_.data(), counters)) {}

SignedCatalog::~SignedCatalog() {
    PlaylistModel::setPersonalService(nullptr);
    if (shorts_) shorts_->setSignedClient(nullptr);
    if (home_ && signedIn_) home_->setClient(anonymous_, false);
    lane_->drain();
}

catalog::InnerTubeClient *SignedCatalog::client() const { return client_.data(); }
catalog::PlaylistService *SignedCatalog::playlists() const { return playlists_.data(); }

void SignedCatalog::setSignedIn(bool signedIn) {
    if (signedIn == signedIn_) return;
    signedIn_ = signedIn;
    if (home_) {
        home_->clear();
        home_->setClient(signedIn ? client_.data() : anonymous_, signedIn);
        home_->load();   // signed in: recommendations; signed out: the popular list (DSC-04a)
    }
    PlaylistModel::setPersonalService(signedIn ? playlists_.data() : nullptr);   // open lists reload
    if (shorts_) shorts_->setSignedClient(signedIn ? client_.data() : nullptr);
    emit signedInChanged();
}

void SignedCatalog::setShortsFeed(catalog::ShortsFeedService *shorts) {
    shorts_ = shorts;
    if (shorts_) shorts_->setSignedClient(signedIn_ ? client_.data() : nullptr);
}

void SignedCatalog::invalidate() {
    if (shorts_) { shorts_->setSignedClient(nullptr); if (signedIn_) shorts_->setSignedClient(client_.data()); }
    if (home_) home_->load();
    PlaylistModel::invalidatePersonal();
}

}}}
