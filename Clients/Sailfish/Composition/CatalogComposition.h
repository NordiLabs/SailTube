// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#pragma once

// Builds the catalog component's object graph (transports, lanes, identity, services and models) and
// exposes it to QML. main.cpp creates one instance after the core objects and before loading QML.
//
// Lanes (RES-02): discovery owns two transports and two worker lanes of its own, never the
// playback component's. "interactive" serves what the user is waiting for (search, suggestions, the
// watch page, a channel's first page, captions, SponsorBlock); "background" serves feed refresh,
// comment walks and Shorts classification.

#include <QObject>
#include <QScopedPointer>
#include <QString>

class QQmlContext;
class QQmlEngine;

namespace nordtile { namespace video {
namespace resolver { class YtDlpVideoService; }
namespace library { class LibraryStore; }
namespace downloads { class DownloadManager; }
namespace account { class AccountService; }
namespace sailfish { class WatchStateIndex; class TapIntent; }
namespace catalog {

class CatalogComposition final : public QObject {
    Q_OBJECT
public:
    CatalogComposition(resolver::YtDlpVideoService *extractor, const QString &cacheRoot, const QString &dataRoot,
                       QObject *parent = nullptr);
    ~CatalogComposition() override;

    // The library component's hooks (feed subscriptions and write-back, remote playlists, import
    // handles, hide watched); call before expose(). Either pointer may be null.
    void connectLibrary(library::LibraryStore *store, sailfish::WatchStateIndex *watch);
    // Context properties and QML types; call before the root QML is loaded.
    void expose(QQmlEngine *engine, QQmlContext *context);
    // DL-07: offline captions and SponsorBlock segments of finished downloads (library component's
    // CatalogSidecarSource over the catalog's background transport and lane). May be null.
    void connectDownloads(downloads::DownloadManager *manager);
    // ACC-11: signed-in recommended home, Watch later ("WL") and Liked ("LL") through the account
    // component's SignedCatalogTransport; signed out, everything stays anonymous. May be null.
    void connectAccount(account::AccountService *accountService);
    // E12 likely-next-tap signals (also the QML context property `tapIntent`), for playback.
    sailfish::TapIntent *tapIntent() const;
    // LNK-03 / L1: claims the link service's bus name right after the application object exists,
    // before anything slow, so a D-Bus activation call is answered at once. The composition
    // adopts the router and service later. Optional: the constructor claims it otherwise.
    static void claimLinkService();
    // Network hosts the catalog component may contact (NFR-SEC-01 inventory input).
    static QStringList allowedHosts();

private:
    struct Parts;
    QScopedPointer<Parts> d;
};

}}}
