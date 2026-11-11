// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#pragma once

// IMP-02: import subscriptions and playlists from the signed-in account.
// Browses FEchannels (channelRenderer / gridChannelRenderer UC ids) and then
// FEplaylist_aggregation (gridPlaylistRenderer, playlistRenderer and
// lockupViewModel entries; UC rows are skipped). Each feed reads at most 20
// pages (the first one included) and stops at the first page that adds
// nothing new or carries no continuation. Every request carries the selected
// login (X-Goog-AuthUser) and, for a brand channel, its page id
// (X-Goog-PageId).
//
// The signed-in session (cookies, SAPISIDHASH, client context) is the account
// component's: it implements AccountBrowseTransport. The result is one ImportBatch
// (source "account") for ImportJob::startBatch, so the merge rules of every
// other import apply (additive, idempotent, one summary).

#include "Core/Library/Domain/LibraryTypes.h"
#include <QByteArray>
#include <QJsonObject>
#include <QMap>
#include <QObject>
#include <QString>
#include <functional>
#include <vector>

namespace nordtile { namespace video { namespace library {

struct AccountIdentity {
    int authUser = 0;   // index of the selected login in the browser session
    QString pageId;     // brand channel page id; empty for the login's own channel
};

class AccountBrowseTransport {
public:
    typedef std::function<void(bool ok, const QJsonObject &reply)> Done;
    virtual ~AccountBrowseTransport() {}
    // POST youtubei/v1/browse as the signed-in user. `body` holds browseId or
    // continuation; the transport adds the client context and credentials and
    // sends `headers` as given. `done` runs once, on the main thread.
    virtual void browse(const QJsonObject &body, const QMap<QByteArray, QByteArray> &headers, Done done) = 0;
};

namespace accountImport {

const int maxPages = 20;

struct Page {
    std::vector<ChannelRecord> channels;
    std::vector<RemotePlaylistRef> playlists;
    QString continuation;   // empty: last page
    int skippedRows = 0;    // UC rows among playlists, malformed ids
};

Page parseChannelsPage(const QJsonObject &reply);
Page parsePlaylistsPage(const QJsonObject &reply);
QMap<QByteArray, QByteArray> headersFor(const AccountIdentity &identity);

} // namespace accountImport

class AccountImporter final : public QObject {
    Q_OBJECT
public:
    // `transport` must outlive the importer.
    explicit AccountImporter(AccountBrowseTransport *transport, QObject *parent = nullptr);

    bool isRunning() const { return running_; }
    // false while running.
    bool start(const AccountIdentity &identity);
    // Drops the run; a late reply is ignored and nothing is reported.
    void cancel();

signals:
    void progress(int pagesRead);
    // error empty on success; on error the batch is what was read so far and
    // callers apply nothing (the import is repeatable).
    void finished(const ImportBatch &batch, const QString &error);

private:
    enum class Feed { Channels, Playlists };
    void request(const QJsonObject &body);
    void onReply(int run, bool ok, const QJsonObject &reply);
    void finish(const QString &error);

    AccountBrowseTransport *transport_;
    AccountIdentity identity_;
    ImportBatch batch_;
    Feed feed_ = Feed::Channels;
    int pagesInFeed_ = 0;
    int pagesRead_ = 0;
    int run_ = 0;
    bool running_ = false;
};

}}}
