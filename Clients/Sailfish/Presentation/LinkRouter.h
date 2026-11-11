// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#pragma once

// Routes opened links (D-Bus openUrl, LNK-03/LNK-04) to page signals. QtCore only.
//  * openUrl() takes a string, a string list or a variant list and routes the first valid link.
//  * Before setReady(true) links are queued in arrival order (cold start); setReady(true)
//    routes them all, in order.
//  * Each routed link emits its route signal first and raiseRequested() after it, so the link
//    opens even when raising the window does nothing.

#include "Core/Catalog/Domain/Links.h"

#include <QObject>
#include <QString>
#include <QStringList>
#include <QVariant>

namespace nordtile { namespace video { namespace sailfish {

class LinkRouter final : public QObject {
    Q_OBJECT
    Q_PROPERTY(bool ready READ ready WRITE setReady NOTIFY readyChanged)
    Q_PROPERTY(int pending READ pending NOTIFY readyChanged)
public:
    explicit LinkRouter(QObject *parent = nullptr);

    bool ready() const { return queue_.ready(); }
    int pending() const { return static_cast<int>(queue_.pending()); }

    // The D-Bus argument as received: a QString, a QStringList or a QVariantList (non-string
    // elements are skipped). Exposed for tests and for the adaptor.
    static const char *kindName(catalog::LinkKind kind);
    static QStringList candidates(const QVariant &argument);

public slots:
    void openUrl(const QVariant &argument);
    void setReady(bool ready);
    // A bare activation (no link): only raises the window.
    void raise();

signals:
    // startSeconds -1 = none (resume applies); index -1 = none.
    void openVideo(const QString &videoId, int startSeconds, const QString &playlistId, int index);
    // "UC...", "@handle", or a canonical https URL for /c/ and /user/ links.
    void openChannel(const QString &reference);
    void openPlaylist(const QString &listId);
    void unsupported(const QString &url);
    void raiseRequested();
    void readyChanged();

private:
    void route(const catalog::LinkTarget &target);

    catalog::LinkQueue queue_;
};

}}}
