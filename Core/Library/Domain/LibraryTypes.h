// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#pragma once

// Qt-free records of the local-first library (SAILTUBE_REQUIREMENTS §3.5, §3.7).
// Times are Unix epoch milliseconds. Positions and durations are milliseconds
// unless the field name says seconds. Display strings are UTF-8.

#include <cstdint>
#include <string>
#include <vector>

namespace nordtile { namespace video { namespace library {

// LIB-01: a subscribed channel. Only UC ids are stored here; @handle URLs wait
// in PendingHandle until something online resolves them.
struct ChannelRecord {
    std::string channelId;        // UC + 22 characters
    std::string name;
    std::string avatarUrl;
    std::int64_t latestUploadMs = 0; // written back by the feed (catalog component), 0 unknown
    std::int64_t addedMs = 0;
};

struct PendingHandle {
    std::string url;              // as imported, e.g. https://www.youtube.com/@name
    std::string name;
    std::int64_t addedMs = 0;
};

enum class SubscriptionSort { Name, LatestUpload };

// LIB-02/03: playlists. Watch later is a built-in local list with a fixed id.
enum class PlaylistKind { Local = 0, WatchLater = 1, RemoteMirror = 2 };

const char *const watchLaterPlaylistId = "watch-later";

struct PlaylistRecord {
    std::string id;               // 12 lowercase hex for local lists, watchLaterPlaylistId, "r-<listId>" for mirrors
    PlaylistKind kind = PlaylistKind::Local;
    std::string title;
    std::string remoteId;         // YouTube list id (PL…, OL…, …) for mirrors, empty otherwise
    std::string channel;          // owner shown for mirrors
    std::string thumbnailUrl;     // first item's thumbnail when empty
    int itemCount = 0;
    std::int64_t createdMs = 0;
    std::int64_t updatedMs = 0;
    std::int64_t refreshedMs = 0; // mirrors: last successful fetch, 0 = never (fetch on first open)
};

struct VideoEntry {
    std::string videoId;
    std::string title;
    std::string channel;
    std::string channelId;
    std::int64_t durationSeconds = -1;
    std::string thumbnailUrl;
};

// LIB-04: one record per video.
struct HistoryRecord {
    VideoEntry video;
    std::int64_t positionMs = 0;
    std::int64_t durationMs = 0;
    double fraction = 0;          // 0..1 of the duration played (furthest position / duration)
    bool watched = false;         // sticky
    std::int64_t watchedAtMs = 0; // last time it was watched (recency)
    int playCount = 1;
};

// LIB-05: a resume point. Absence means "start from the beginning".
struct ResumeRecord {
    std::string videoId;
    std::int64_t positionMs = 0;
    std::int64_t durationMs = 0;
    std::int64_t touchedMs = 0;
};

// LIB-06: what a thumbnail overlay needs, looked up per row.
struct WatchState {
    double fraction = 0;
    bool watched = false;
    bool known = false;           // false when the video has no history record
};

// What the player reports to the library (every 30 s, on leave, stop and end).
struct PlaybackProgress {
    VideoEntry video;
    std::int64_t positionMs = 0;
    std::int64_t durationMs = 0;  // <= 0 unknown (live): history only, no resume
    bool ended = false;           // reached end of stream
    bool live = false;            // live streams store no resume point
};

// IMP-*: everything an importer (NewPipe backup, Takeout, OPML, SailTube backup,
// later the account import IMP-02) hands to the store. The store applies the
// merge rules: local records win, re-running is idempotent.
struct ImportedPlaylist {
    std::string title;
    std::vector<VideoEntry> items; // in order; duplicates are dropped
};

struct RemotePlaylistRef {
    std::string remoteId;
    std::string title;
    std::string channel;
    std::string thumbnailUrl;
    int itemCountHint = -1;
};

struct ImportBatch {
    std::string source;           // "newpipe", "takeout", "opml", "sailtube", "account"
    std::vector<ChannelRecord> subscriptions;
    std::vector<PendingHandle> pendingHandles;
    std::vector<HistoryRecord> history;  // watchedAtMs/playCount already aggregated
    std::vector<ResumeRecord> resume;    // only eligible points (see resumeEligibleForImport)
    std::vector<ImportedPlaylist> playlists;
    std::vector<RemotePlaylistRef> remotePlaylists;
    std::vector<std::string> skippedSections; // e.g. "stream_state" when a table/column was missing
    int skippedRows = 0;          // rows dropped as non-YouTube or malformed
};

struct ImportSummary {
    int subscriptionsAdded = 0;
    int handlesPending = 0;
    int historyAdded = 0;
    int historyUpdated = 0;       // existing local records that gained a watched flag or play count
    int resumeAdded = 0;
    int playlistsAdded = 0;
    int playlistsSkipped = 0;     // a local list with the same name (case-insensitive) exists
    int playlistItemsAdded = 0;
    int remotePlaylistsAdded = 0;
    int skippedRows = 0;
    std::vector<std::string> skippedSections;
    bool nothingNew() const {
        return subscriptionsAdded + handlesPending + historyAdded + historyUpdated + resumeAdded
               + playlistsAdded + playlistItemsAdded + remotePlaylistsAdded == 0;
    }
};

}}}
