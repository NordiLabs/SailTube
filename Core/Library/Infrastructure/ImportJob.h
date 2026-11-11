// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#pragma once

// The import interface (IMP-01..04). One job runs one import on a worker
// QThread so the playback thread never waits for parsing or bulk writes
// (RES-02): the worker parses the input, opens its own LibraryStore on the
// same database file (unique connection name), calls applyImport() and reports
// finished() back on the thread that owns the job. The caller then calls
// reload() on its primary store.
//
// Every importer produces an ImportBatch; LibraryStore::applyImport() applies
// the merge rules (local wins, idempotent, same-named lists skipped, local
// resume points never overwritten). New importers plug in through
// startBatch(): the account importer (IMP-02) builds its batch online (source
// "account") and hands it over here.
//
// finished(summary, error): error is empty on success. summary holds the
// ImportSummary fields (subscriptionsAdded, handlesPending, historyAdded,
// historyUpdated, resumeAdded, playlistsAdded, playlistsSkipped,
// playlistItemsAdded, remotePlaylistsAdded, skippedRows, skippedSections,
// nothingNew) plus "source". A SailTube backup restore adds "settings" (the
// allowlisted QSettings values) and "librarySettings" (store meta values);
// applying them is the caller's decision (applyLibrarySettings() for the
// latter, then reload()). An export reports "path" and the exported counts.

#include "Core/Library/Domain/LibraryTypes.h"
#include <QObject>
#include <QString>
#include <QVariantMap>

namespace nordtile { namespace video { namespace library {

class ImportThread;
class LibraryStore;

class ImportJob final : public QObject {
    Q_OBJECT
public:
    enum class Kind { NewPipe, TakeoutCsv, Opml, SailTubeBackup, Batch, ExportBackup };

    // `privateDir`: app-private scratch directory for extracted archives (never /tmp, PLT-09).
    ImportJob(const QString &databasePath, const QString &privateDir, QObject *parent = nullptr);
    ~ImportJob() override; // waits for a running worker; imports are short and transactional

    // NewPipe/PipePipe .zip or .db, Takeout .csv or .zip, OPML, SailTube backup JSON.
    // False when a job is already running or `kind` needs another entry point.
    bool start(Kind kind, const QString &inputPath);
    // Applies an already built batch (IMP-02 account import and future sources).
    bool startBatch(const ImportBatch &batch);
    // IMP-04 export of the whole library plus allowlisted settings, off the playback thread.
    bool startExport(const QString &targetPath, const QString &settingsIniPath);
    bool isRunning() const;

    static QVariantMap summaryToVariant(const ImportSummary &summary);
    // IMP-04 restore: writes the backup's "librarySettings" with setMetaValue (allowlisted keys only).
    static void applyLibrarySettings(LibraryStore &store, const QVariantMap &librarySettings);

signals:
    void finished(const QVariantMap &summary, const QString &error);

private:
    bool launch(ImportThread *thread);
    void collect();

    QString databasePath_;
    QString privateDir_;
    ImportThread *thread_ = nullptr;
};

}}}
