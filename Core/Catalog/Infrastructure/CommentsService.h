// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#pragma once

// Comments over InnerTube (DSC-16). Runs on the *background* discovery client: comment walks
// never delay interactive discovery and never touch the playback component (RES-02). Parsing runs
// on the client's WorkLane; callbacks arrive on the caller's thread.
//
//  * First token: from watch-next, the itemSectionRenderer with sectionIdentifier
//    "comment-item-section" (the comments engagement panel's copy first, because the sort
//    menu and the count live next to it; the main results' copy otherwise).
//  * Pages: POST /next {continuation}; items from onResponseReceivedEndpoints[]
//    (reloadContinuationItemsCommand / appendContinuationItemsAction).
//  * Bodies: frameworkUpdates.entityBatchUpdate.mutations[].payload.commentEntityPayload, joined
//    to commentViewModel.commentKey by key (never by order).
//  * Total: commentsHeaderRenderer.countText when it carries a number, else the engagement
//    panel's contextualInfo from watch-next.
//  * Replies: the continuation inside each thread's replies.commentRepliesRenderer, fetched on
//    demand per thread; further reply pages from the "Show more replies" continuation.
//  * Valid-empty vs drift by container markers (DSC-01).

#include "InnerTubeClient.h"
#include "Core/Catalog/Domain/Comments.h"

#include <QJsonObject>
#include <QObject>
#include <QString>
#include <functional>

namespace nordtile { namespace video { namespace catalog {

class CommentsService final : public QObject {
    Q_OBJECT
public:
    using EntryCallback = std::function<void(const CommentsEntry &)>;
    using PageCallback = std::function<void(const CommentsPage &)>;

    // `client` must be the background discovery client. `counters` may be null.
    explicit CommentsService(InnerTubeClient *client, CatalogCounters *counters = nullptr, QObject *parent = nullptr);

    // POST /next {videoId} and extract the comments entry (used when no token was supplied).
    quint64 fetchEntry(const QString &videoId, QObject *context, EntryCallback done);
    // One top-level page (threads) for `token`.
    quint64 fetchThreads(const QString &token, QObject *context, PageCallback done);
    // One replies page for `token`.
    quint64 fetchReplies(const QString &token, QObject *context, PageCallback done);
    void cancel(quint64 id);

    std::string hl() const;

    // Pure parsers (lane-safe, used by tests).
    static CommentsEntry parseEntry(const QJsonObject &next, const std::string &hl);
    static CommentsPage parsePage(const QJsonObject &answer, const std::string &hl, bool replies);

private:
    InnerTubeClient *client_;
    CatalogCounters *counters_;
};

}}}
