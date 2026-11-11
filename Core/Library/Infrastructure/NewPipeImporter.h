// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#pragma once

// IMP-01: NewPipe / PipePipe backup parser (they share the format). Input is
// an exported .zip (its "newpipe.db", else the first entry ending ".db") or a
// raw SQLite .db; "file://" URLs and "~/" paths are accepted. The database is
// copied or extracted to a private 0600 temp file in the caller's private
// directory (never /tmp, PLT-09), opened read-only on its own connection, and
// deleted afterwards, also on failure. Works on any thread (one QSqlDatabase
// connection per call).
//
// Only YouTube rows (service_id 0 or NULL) are used. A missing table or column
// skips only its section (named in batch.skippedSections). Counted in
// batch.skippedRows: non-YouTube or unusable subscriptions, streams and remote
// playlists, and history rows pointing at no stream. Rows that point at a
// skipped (non-YouTube) stream are dropped without being counted again.
//
// Mapping: subscriptions /channel/UC… -> ChannelRecord, @handle, /c/, /user/ ->
// PendingHandle (resolved online later). streams is the join hub (durations in
// seconds). stream_history: latest access_date kept, repeat_count summed.
// stream_state.progress_time is in milliseconds. Watched = reachesWatched() or
// present in history with no stream_state row (NewPipe drops the state at the
// end); when the whole stream_state section is missing nothing is inferred.
// Resume points from stream_state pass resumeEligibleForImport() and are
// stamped `nowMs`. playlists + playlist_stream_join in join_index order,
// duplicates dropped. remote_playlists -> RemotePlaylistRef.

#include "Core/Library/Domain/LibraryTypes.h"
#include <QString>

namespace nordtile { namespace video { namespace library {

enum class NewPipeImportError {
    None,
    NoPrivateDirectory,   // the private temp directory is missing and cannot be created
    FileNotFound,
    UnrecognizedFormat,   // neither a ZIP nor an SQLite database
    ZipError,             // damaged, ZIP64, encrypted, too large or CRC mismatch
    NoDatabaseInZip,
    TooLarge,             // raw database above the 512 MiB cap
    CopyFailed,           // the private temp copy could not be written
    DriverMissing,        // no QSQLITE plugin
    DatabaseOpenFailed,   // corrupt or not a database
    NotNewPipeDatabase    // a database without any NewPipe table
};

struct NewPipeImportResult {
    ImportBatch batch;                // source "newpipe"
    NewPipeImportError error = NewPipeImportError::None;
    QString errorString;
    bool ok() const { return error == NewPipeImportError::None; }
};

// `privateDir`: an app-private directory for the temp copy (created 0700 if
// missing). `nowMs` stamps added records and resume points (0 = current time).
NewPipeImportResult importNewPipeBackup(const QString &input, const QString &privateDir, qint64 nowMs = 0);

// Parses an already private database file (read-only). Used by the function
// above and by tests; does not delete `databasePath`.
NewPipeImportResult parseNewPipeDatabase(const QString &databasePath, qint64 nowMs);

}}}
