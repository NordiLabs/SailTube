// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// ACC-11: the catalog's services signed in (SignedCatalog) over a stand-in for the account component's
// SignedCatalogTransport. Signed answers are DERIVED from recorded anonymous answers (no account
// is used to record): the recommended home is the anonymous home with a channel's video grid in
// its tab (as in catalog_feed), Watch later / Liked are the recorded playlist page.
#include "tests/catalog_common/TestSupport.h"

#include "Clients/Sailfish/Presentation/PlaylistModel.h"
#include "Clients/Sailfish/Presentation/SignedCatalog.h"
#include "Core/Catalog/Infrastructure/ClientIdentity.h"
#include "Core/Catalog/Infrastructure/HomeService.h"
#include "Core/Catalog/Infrastructure/InnerTubeClient.h"
#include "Core/Catalog/Infrastructure/Json.h"
#include "Core/Catalog/Infrastructure/PlaylistService.h"
#include "Core/Catalog/Infrastructure/WorkLane.h"

#include <QJsonArray>
#include <QJsonDocument>

using namespace nordtile::video;
using namespace nordtile::video::catalog;
using catalog_test::FakeTransport;
using sailfish::PlaylistModel;
using sailfish::SignedCatalog;

namespace {
// derived: the signed-in home shape (the anonymous home with a video grid in its selected tab).
QByteArray signedHome() {
    QJsonObject doc = catalog_test::fixtureJson(QStringLiteral("home_anonymous.json"));
    const QJsonObject grid = json::findFirst(catalog_test::fixtureJson(QStringLiteral("channel_videos.json")), QStringLiteral("richGridRenderer"));
    QJsonObject content; content.insert(QStringLiteral("richGridRenderer"), grid);
    QJsonObject tabRenderer; tabRenderer.insert(QStringLiteral("selected"), true); tabRenderer.insert(QStringLiteral("content"), content);
    QJsonObject tab; tab.insert(QStringLiteral("tabRenderer"), tabRenderer);
    QJsonObject two; two.insert(QStringLiteral("tabs"), QJsonArray() << tab);
    QJsonObject contents; contents.insert(QStringLiteral("twoColumnBrowseResultsRenderer"), two);
    doc.insert(QStringLiteral("contents"), contents);
    return QJsonDocument(doc).toJson(QJsonDocument::Compact);
}
}

static void signedServices() {
    ClientIdentity identity(nullptr, QString());
    WorkLane lane(QStringLiteral("anonymous"), 1);
    DiscoveryLocale loc;
    FakeTransport anonymous, signedNet;
    anonymous.on(QStringLiteral("browse"), "\"browseId\":\"VLPLF98rxtslw6s\"", 200, catalog_test::fixture(QStringLiteral("playlist_browse.json")));
    signedNet.on(QStringLiteral("browse"), "FEwhat_to_watch", 200, signedHome());
    // derived: Watch later and Liked answer in the shape of the recorded playlist page.
    signedNet.on(QStringLiteral("browse"), "\"browseId\":\"VLWL\"", 200, catalog_test::fixture(QStringLiteral("playlist_browse.json")));
    signedNet.on(QStringLiteral("browse"), "\"browseId\":\"VLLL\"", 200, catalog_test::fixture(QStringLiteral("playlist_browse.json")));
    InnerTubeClient anonClient(&anonymous, &identity, &lane, loc);
    PlaylistService anonPlaylists(&anonClient);
    HomeService home(&anonClient, false);

    SignedCatalog account(&signedNet, &identity, loc, &home, &anonClient);
    CHECK(!account.signedIn());
    CHECK(PlaylistModel::isPersonalList(QStringLiteral("WL")) && PlaylistModel::isPersonalList(QStringLiteral("VLLL")));
    CHECK(!PlaylistModel::isPersonalList(QStringLiteral("PLF98rxtslw6s")));

    // Signed out: the personal lists say "sign in", nothing is sent anywhere; others are anonymous.
    PlaylistModel later(&anonPlaylists);
    later.open(QStringLiteral("WL"));
    CHECK(later.outcome() == QStringLiteral("unavailable"));
    CHECK_EQ(later.errorText(), QStringLiteral("Sign in to see this list."));
    PlaylistModel normal(&anonPlaylists);
    normal.open(QStringLiteral("PLF98rxtslw6s"));
    CHECK(catalog_test::waitFor([&] { return normal.count() > 0; }));
    home.load();
    CHECK(home.outcome() == Outcome::ValidEmpty);
    CHECK_EQ(signedNet.requests.size(), 0);
    CHECK_EQ(anonymous.count(QStringLiteral("browse"), "FEwhat_to_watch"), 0);

    // Signed in: home and the open personal list load through the signed transport only.
    int homeChanges = 0;
    QObject::connect(&home, &HomeService::changed, [&homeChanges] { ++homeChanges; });
    account.setSignedIn(true);
    CHECK(home.signedIn());
    CHECK(catalog_test::waitFor([&] { return !home.busy() && home.items().size() == 30; }));
    CHECK(catalog_test::waitFor([&] { return later.count() == 3; }));
    CHECK(later.title() != QString());
    CHECK_EQ(signedNet.count(QStringLiteral("browse"), "FEwhat_to_watch"), 1);
    CHECK_EQ(signedNet.count(QStringLiteral("browse"), "\"browseId\":\"VLWL\""), 1);
    CHECK_EQ(anonymous.count(QStringLiteral("browse"), "FEwhat_to_watch"), 0);
    CHECK_EQ(anonymous.count(QStringLiteral("browse"), "VLWL"), 0);
    PlaylistModel liked(&anonPlaylists);
    liked.open(QStringLiteral("LL"));
    CHECK(catalog_test::waitFor([&] { return liked.count() == 3; }));
    CHECK_EQ(signedNet.count(QStringLiteral("browse"), "\"browseId\":\"VLLL\""), 1);

    // An account switch: personalised rows go and reload.
    account.invalidate();
    CHECK(home.items().empty() || home.busy());
    CHECK(catalog_test::waitFor([&] { return !home.busy() && home.items().size() == 30; }));
    CHECK(catalog_test::waitFor([&] { return signedNet.count(QStringLiteral("browse"), "\"browseId\":\"VLWL\"") == 2; }));
    CHECK(catalog_test::waitFor([&] { return later.count() == 3; }));

    // Signed out again: anonymous home (empty, no request), personal lists unavailable.
    const int sent = signedNet.requests.size();
    account.setSignedIn(false);
    CHECK(!home.signedIn());
    CHECK(home.items().empty());
    CHECK(later.outcome() == QStringLiteral("unavailable"));
    CHECK_EQ(later.count(), 0);
    home.load();
    CHECK_EQ(signedNet.requests.size(), sent);
    CHECK(homeChanges > 0);
}

int main(int argc, char **argv) {
    QCoreApplication app(argc, argv);
    signedServices();
    return catalog_test::finish("catalog_account");
}
