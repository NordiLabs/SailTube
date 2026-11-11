// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#pragma once

// Remote playlists for the preview (LIB-03, LNK-04): POST /youtubei/v1/browse {browseId:
// "VL<listId>"} and continuations ({continuation}), with no item cap. Rows come from
// playlistVideoRenderer (structured lengthSeconds and index) or lockupViewModel rows; each row
// carries the playlist id and its 0-based position. The header gives title, owner (name and UC
// id), video count and view text from pageHeaderViewModel, with playlistHeaderRenderer, the
// sidebar and playlistMetadataRenderer as fallbacks.
//
// Valid empty vs drift (DSC-01): the playlist's section list present without rows ->
// ValidEmpty; the container missing -> ShapeDrift; an ERROR alert (missing or private list)
// -> Unavailable. Saving a playlist belongs to the library component.

#include "InnerTubeClient.h"
#include "Core/Catalog/Domain/Item.h"
#include "Core/Catalog/Domain/Outcome.h"

#include <QMap>
#include <QObject>
#include <QString>
#include <functional>
#include <vector>

namespace nordtile { namespace video { namespace catalog {

struct PlaylistPage {
    Outcome outcome = Outcome::Network;
    int httpStatus = 0;
    QString playlistId;
    QString title;
    QString owner;
    QString ownerId;           // UC id when the owner is a channel
    QString videoCountText;
    qint64 videoCount = -1;
    QString viewText;
    QString description;
    QString thumbnailUrl;
    std::vector<Item> items;
    QString continuation;
    QMap<QString, int> unknown;
};

class PlaylistService final : public QObject {
    Q_OBJECT
public:
    PlaylistService(InnerTubeClient *client, CatalogCounters *counters = nullptr, QObject *parent = nullptr);

    // Page 1 (with the header). `listId` may be a bare id, "VL<id>" or a URL with list=.
    void open(const QString &listId, QObject *context, std::function<void(PlaylistPage)> done);
    // A continuation page; `firstIndex` is the position of its first row in the playlist.
    void more(const QString &listId, const QString &continuation, int firstIndex, QObject *context,
              std::function<void(PlaylistPage)> done);

    static QString normalizeListId(const QString &listId);
    static PlaylistPage parseBrowse(const InnerTubeReply &reply, const QString &listId, const QString &hl);
    static PlaylistPage parseContinuation(const InnerTubeReply &reply, const QString &listId, int firstIndex,
                                          const QString &hl);

private:
    void count(const PlaylistPage &page);
    InnerTubeClient *client_;
    CatalogCounters *counters_;
};

}}}
