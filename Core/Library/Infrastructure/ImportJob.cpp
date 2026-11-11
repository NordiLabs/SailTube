// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "ImportJob.h"

#include "LibraryStore.h"
#include "NewPipeImporter.h"
#include "SailTubeBackup.h"
#include "TakeoutImporter.h"

#include <QStringList>
#include <QThread>
#include <QUuid>

namespace nordtile { namespace video { namespace library {

class ImportThread : public QThread {
public:
    ImportJob::Kind kind = ImportJob::Kind::Batch;
    QString databasePath, privateDir, input, settingsIni;
    ImportBatch batch;
    // Results, read by the owning thread after finished().
    QVariantMap summary;
    QString error;

protected:
    void run() override {
        const qint64 now = libraryNowMs();
        QVariantMap extra;
        switch (kind) {
        case ImportJob::Kind::NewPipe: {
            const NewPipeImportResult r = importNewPipeBackup(input, privateDir, now);
            if (!r.ok()) { error = r.errorString; return; }
            batch = r.batch;
            break;
        }
        case ImportJob::Kind::TakeoutCsv:
        case ImportJob::Kind::Opml: {
            const TextImportResult r = kind == ImportJob::Kind::Opml ? importOpmlFile(input, now)
                                                                     : importTakeoutFile(input, now);
            if (!r.ok()) { error = r.errorString; return; }
            batch = r.batch;
            break;
        }
        case ImportJob::Kind::SailTubeBackup: {
            const BackupRestoreResult r = readSailTubeBackup(input);
            if (!r.ok()) { error = r.error; return; }
            batch = r.batch;
            extra.insert(QStringLiteral("settings"), r.settings);
            extra.insert(QStringLiteral("librarySettings"), r.librarySettings);
            break;
        }
        case ImportJob::Kind::Batch:
        case ImportJob::Kind::ExportBackup:
            break;
        }

        LibraryStore store(databasePath, QStringLiteral("sailtube-import-") + QUuid::createUuid().toString());
        QString openError;
        if (!store.open(&openError)) {
            error = openError.isEmpty() ? QStringLiteral("cannot open the library") : openError;
            return;
        }
        if (kind == ImportJob::Kind::ExportBackup) {
            const ImportBatch snapshot = store.snapshot();
            QVariantMap meta;
            for (const QString &key : backupLibraryMetaKeys()) {
                const QString value = store.metaValue(key);
                if (!value.isNull()) meta.insert(key, value);
            }
            if (!writeSailTubeBackup(input, snapshot, backupSettingsFromIni(settingsIni), meta, now, &error))
                return;
            summary.insert(QStringLiteral("source"), QStringLiteral("export"));
            summary.insert(QStringLiteral("path"), input);
            summary.insert(QStringLiteral("subscriptions"), int(snapshot.subscriptions.size()));
            summary.insert(QStringLiteral("pendingHandles"), int(snapshot.pendingHandles.size()));
            summary.insert(QStringLiteral("history"), int(snapshot.history.size()));
            summary.insert(QStringLiteral("resume"), int(snapshot.resume.size()));
            summary.insert(QStringLiteral("playlists"), int(snapshot.playlists.size()));
            summary.insert(QStringLiteral("remotePlaylists"), int(snapshot.remotePlaylists.size()));
            return;
        }
        // applyImport folds the parser's skippedRows/skippedSections into the one summary.
        summary = ImportJob::summaryToVariant(store.applyImport(batch));
        summary.insert(QStringLiteral("source"), QString::fromStdString(batch.source));
        for (QVariantMap::const_iterator it = extra.constBegin(); it != extra.constEnd(); ++it)
            summary.insert(it.key(), it.value());
    }
};

ImportJob::ImportJob(const QString &databasePath, const QString &privateDir, QObject *parent)
    : QObject(parent), databasePath_(databasePath), privateDir_(privateDir) {}

ImportJob::~ImportJob() {
    if (thread_) {
        thread_->wait();
        delete thread_;
    }
}

bool ImportJob::isRunning() const { return thread_ != nullptr; }

bool ImportJob::launch(ImportThread *thread) {
    if (thread_) {
        delete thread;
        return false;
    }
    thread->databasePath = databasePath_;
    thread->privateDir = privateDir_;
    thread_ = thread;
    // QThread::finished comes from the worker; `this` lives here, so the call is queued.
    connect(thread, &QThread::finished, this, [this]() { collect(); });
    thread->start(QThread::LowPriority);
    return true;
}

bool ImportJob::start(Kind kind, const QString &inputPath) {
    if (kind == Kind::Batch || kind == Kind::ExportBackup) return false;
    ImportThread *t = new ImportThread;
    t->kind = kind;
    t->input = inputPath;
    return launch(t);
}

bool ImportJob::startBatch(const ImportBatch &batch) {
    ImportThread *t = new ImportThread;
    t->kind = Kind::Batch;
    t->batch = batch;
    return launch(t);
}

bool ImportJob::startExport(const QString &targetPath, const QString &settingsIniPath) {
    ImportThread *t = new ImportThread;
    t->kind = Kind::ExportBackup;
    t->input = targetPath;
    t->settingsIni = settingsIniPath;
    return launch(t);
}

void ImportJob::collect() {
    ImportThread *t = thread_;
    if (!t) return;
    t->wait();
    thread_ = nullptr;
    const QVariantMap summary = t->summary;
    const QString error = t->error;
    delete t;
    emit finished(summary, error);
}

void ImportJob::applyLibrarySettings(LibraryStore &store, const QVariantMap &librarySettings) {
    const QStringList keys = backupLibraryMetaKeys();
    for (QVariantMap::const_iterator it = librarySettings.constBegin(); it != librarySettings.constEnd(); ++it)
        if (keys.contains(it.key())) store.setMetaValue(it.key(), it.value().toString());
}

QVariantMap ImportJob::summaryToVariant(const ImportSummary &s) {
    QVariantMap m;
    m.insert(QStringLiteral("subscriptionsAdded"), s.subscriptionsAdded);
    m.insert(QStringLiteral("handlesPending"), s.handlesPending);
    m.insert(QStringLiteral("historyAdded"), s.historyAdded);
    m.insert(QStringLiteral("historyUpdated"), s.historyUpdated);
    m.insert(QStringLiteral("resumeAdded"), s.resumeAdded);
    m.insert(QStringLiteral("playlistsAdded"), s.playlistsAdded);
    m.insert(QStringLiteral("playlistsSkipped"), s.playlistsSkipped);
    m.insert(QStringLiteral("playlistItemsAdded"), s.playlistItemsAdded);
    m.insert(QStringLiteral("remotePlaylistsAdded"), s.remotePlaylistsAdded);
    m.insert(QStringLiteral("skippedRows"), s.skippedRows);
    QStringList sections;
    for (const std::string &section : s.skippedSections) sections << QString::fromStdString(section);
    m.insert(QStringLiteral("skippedSections"), sections);
    m.insert(QStringLiteral("nothingNew"), s.nothingNew());
    return m;
}

}}}
