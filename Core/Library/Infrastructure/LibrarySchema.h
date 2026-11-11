// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#pragma once

// Versioned SQLite schema of library.db (PRAGMA user_version). Each entry of
// librarySchemaSteps() upgrades from version i to i+1 and is applied inside one
// transaction by LibraryStore::open(). Never edit a released step: append one.

#include <QStringList>

namespace nordtile { namespace video { namespace library {

const int librarySchemaVersion = 1;

inline QStringList librarySchemaStep(int fromVersion) {
    QStringList s;
    if (fromVersion == 0) {
        s << QStringLiteral("CREATE TABLE meta(key TEXT PRIMARY KEY, value TEXT NOT NULL)")
          // LIB-01
          << QStringLiteral("CREATE TABLE subscriptions(channel_id TEXT PRIMARY KEY, name TEXT NOT NULL,"
                            " avatar_url TEXT NOT NULL DEFAULT '', latest_upload_ms INTEGER NOT NULL DEFAULT 0,"
                            " added_ms INTEGER NOT NULL)")
          << QStringLiteral("CREATE TABLE pending_handles(url TEXT PRIMARY KEY, name TEXT NOT NULL DEFAULT '',"
                            " added_ms INTEGER NOT NULL)")
          // LIB-02/03: kind 0 local, 1 watch later, 2 remote mirror. sort_key: lower first.
          << QStringLiteral("CREATE TABLE playlists(id TEXT PRIMARY KEY, kind INTEGER NOT NULL, title TEXT NOT NULL,"
                            " remote_id TEXT, channel TEXT NOT NULL DEFAULT '', thumbnail_url TEXT NOT NULL DEFAULT '',"
                            " sort_key INTEGER NOT NULL, created_ms INTEGER NOT NULL, updated_ms INTEGER NOT NULL,"
                            " refreshed_ms INTEGER NOT NULL DEFAULT 0, item_count_hint INTEGER NOT NULL DEFAULT -1)")
          << QStringLiteral("CREATE UNIQUE INDEX playlists_remote ON playlists(remote_id) WHERE remote_id IS NOT NULL")
          << QStringLiteral("CREATE TABLE playlist_items(playlist_id TEXT NOT NULL REFERENCES playlists(id) ON DELETE CASCADE,"
                            " position INTEGER NOT NULL, video_id TEXT NOT NULL, title TEXT NOT NULL DEFAULT '',"
                            " channel TEXT NOT NULL DEFAULT '', channel_id TEXT NOT NULL DEFAULT '',"
                            " duration_s INTEGER NOT NULL DEFAULT -1, thumbnail_url TEXT NOT NULL DEFAULT '',"
                            " added_ms INTEGER NOT NULL, PRIMARY KEY(playlist_id, video_id))")
          << QStringLiteral("CREATE INDEX playlist_items_order ON playlist_items(playlist_id, position)")
          // LIB-04: seq is a strictly increasing recency counter (order equals recency within one ms).
          << QStringLiteral("CREATE TABLE history(video_id TEXT PRIMARY KEY, title TEXT NOT NULL DEFAULT '',"
                            " channel TEXT NOT NULL DEFAULT '', channel_id TEXT NOT NULL DEFAULT '',"
                            " duration_s INTEGER NOT NULL DEFAULT -1, thumbnail_url TEXT NOT NULL DEFAULT '',"
                            " position_ms INTEGER NOT NULL DEFAULT 0, duration_ms INTEGER NOT NULL DEFAULT 0,"
                            " fraction REAL NOT NULL DEFAULT 0, watched INTEGER NOT NULL DEFAULT 0,"
                            " watched_at_ms INTEGER NOT NULL, play_count INTEGER NOT NULL DEFAULT 1, seq INTEGER NOT NULL)")
          << QStringLiteral("CREATE INDEX history_recency ON history(seq)")
          // LIB-05
          << QStringLiteral("CREATE TABLE resume(video_id TEXT PRIMARY KEY, position_ms INTEGER NOT NULL,"
                            " duration_ms INTEGER NOT NULL DEFAULT 0, touched_ms INTEGER NOT NULL)")
          << QStringLiteral("CREATE INDEX resume_touched ON resume(touched_ms)")
          // DL-02: the durable download queue. kind 0 audio, 1 video. state: see DownloadTypes.h.
          // A video download keeps its two tracks (video_*, audio_*) until remuxed (DL-03).
          << QStringLiteral("CREATE TABLE downloads(id INTEGER PRIMARY KEY AUTOINCREMENT, video_id TEXT NOT NULL,"
                            " kind INTEGER NOT NULL, quality_height INTEGER NOT NULL DEFAULT 0, state INTEGER NOT NULL,"
                            " title TEXT NOT NULL DEFAULT '', channel TEXT NOT NULL DEFAULT '', channel_id TEXT NOT NULL DEFAULT '',"
                            " duration_s INTEGER NOT NULL DEFAULT -1, thumbnail_url TEXT NOT NULL DEFAULT '',"
                            " directory TEXT NOT NULL DEFAULT '', file_name TEXT NOT NULL DEFAULT '',"
                            " video_format_id TEXT NOT NULL DEFAULT '', audio_format_id TEXT NOT NULL DEFAULT '',"
                            " video_container TEXT NOT NULL DEFAULT '', audio_container TEXT NOT NULL DEFAULT '',"
                            " video_codec TEXT NOT NULL DEFAULT '', audio_codec TEXT NOT NULL DEFAULT '',"
                            " video_bytes_total INTEGER NOT NULL DEFAULT 0, video_bytes_done INTEGER NOT NULL DEFAULT 0,"
                            " audio_bytes_total INTEGER NOT NULL DEFAULT 0, audio_bytes_done INTEGER NOT NULL DEFAULT 0,"
                            " output_layout INTEGER NOT NULL DEFAULT 0, error TEXT NOT NULL DEFAULT '',"
                            " notice TEXT NOT NULL DEFAULT '', extras TEXT NOT NULL DEFAULT '{}',"
                            " created_ms INTEGER NOT NULL, updated_ms INTEGER NOT NULL, sort_key INTEGER NOT NULL,"
                            " UNIQUE(video_id, kind))")
          << QStringLiteral("INSERT INTO playlists(id, kind, title, sort_key, created_ms, updated_ms)"
                            " VALUES('watch-later', 1, 'Watch later', -1000000000, 0, 0)");
    }
    return s;
}

}}}
