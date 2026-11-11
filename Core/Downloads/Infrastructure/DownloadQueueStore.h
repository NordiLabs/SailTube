// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#pragma once

// DL-02 persistent queue: the `downloads` table of library.db (LibrarySchema.h)
// through this store's own QSqlDatabase connection. The library store creates
// and migrates the schema; this store requires it (user_version >= 1) and
// never migrates. Each call is one statement or one transaction, so a crash
// leaves the previous or the next row, never a torn one (NFR-ROB-02).
// Progress is written by the engine at most every 2 s or 4 MiB per item.
//
// Settings kept with the library (meta table): the chosen folder and the
// "unmetered only" switch.

#include "Core/Downloads/Domain/DownloadTypes.h"
#include <QString>
#include <QVariantList>
#include <vector>

namespace nordtile { namespace video { namespace downloads {

class DownloadQueueStore {
public:
    // `connectionName` must be unique per instance.
    DownloadQueueStore(const QString &databasePath, const QString &connectionName);
    ~DownloadQueueStore();
    DownloadQueueStore(const DownloadQueueStore &) = delete;
    DownloadQueueStore &operator=(const DownloadQueueStore &) = delete;

    bool open(QString *error = nullptr);
    bool isOpen() const { return open_; }
    QString databasePath() const { return path_; }

    std::vector<DownloadRecord> loadAll() const;          // by sort_key, id
    // Inserts and sets r->id; false when (video_id, kind) exists already.
    bool insert(DownloadRecord *r);
    bool update(const DownloadRecord &r);                // every column, by id; sets nothing else
    bool updateProgress(qint64 id, qint64 videoBytesDone, qint64 audioBytesDone, qint64 nowMs);
    bool remove(qint64 id);
    qint64 nextSortKey() const;

    QString metaValue(const QString &key, const QString &fallback = QString()) const;
    bool setMetaValue(const QString &key, const QString &value);

    // extras JSON <-> record fields (unknown keys preserved).
    static void readExtras(DownloadRecord *r);
    static std::string writeExtras(const DownloadRecord &r);

private:
    bool exec(const QString &sql, const QVariantList &bind, QString *error = nullptr) const;
    QString path_;
    QString connection_;
    bool open_ = false;
};

}}}
