// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#pragma once

// The merged feed's per-channel cache (DSC-06): painted at launch before any network call
// (stale-while-revalidate), rewritten after each refresh round.
//
//  * Always written from a copy of the state (FeedSchedule::snapshot()), never from the live
//    containers: FinTube lost a save to "dictionary changed size during iteration".
//  * Atomic: QSaveFile (temporary file + rename), owner-only permissions (0600).
//  * save() and load() may run on a worker lane. Writes are serialised, and a snapshot never
//    replaces a newer one: the owner numbers its snapshots, and a write whose number is lower
//    than the last one written is skipped. The lane's save and the synchronous save at
//    shutdown can therefore finish in either order and the file still holds the newer state.

#include "Core/Catalog/Domain/FeedSchedule.h"

#include <QByteArray>
#include <QJsonObject>
#include <QString>
#include <cstdint>
#include <mutex>
#include <vector>

namespace nordtile { namespace video { namespace catalog {

// The compact JSON form of a list row used by the on-disk caches (feed, last search).
QJsonObject cachedItemToJson(const Item &item);
Item cachedItemFromJson(const QJsonObject &object);

class FeedCacheStore {
public:
    static const int kVersion = 1;

    explicit FeedCacheStore(const QString &path) : path_(path) {}
    QString path() const { return path_; }

    // Writes `states` as the newest snapshot (numbered after every snapshot written so far).
    bool save(const std::vector<ChannelFeedState> &states) const;
    // Writes snapshot number `order` unless a higher number has already been written; skipping
    // returns true (the file holds something newer). Numbers start at 1.
    bool save(const std::vector<ChannelFeedState> &states, std::uint64_t order) const;
    // The number of the last snapshot written by this object, 0 when none.
    std::uint64_t lastWrittenOrder() const;
    // Missing or unreadable file: an empty list, `ok` false only when a file existed but did
    // not parse.
    std::vector<ChannelFeedState> load(bool *ok = nullptr) const;

    static QByteArray serialize(const std::vector<ChannelFeedState> &states);
    static std::vector<ChannelFeedState> deserialize(const QByteArray &data, bool *ok = nullptr);

private:
    bool write(const std::vector<ChannelFeedState> &states) const;
    QString path_;
    mutable std::mutex mutex_;
    mutable std::uint64_t written_ = 0;
};

}}}
