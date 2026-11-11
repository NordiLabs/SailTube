// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#pragma once

// IMP-04: the SailTube backup, a JSON file the user chooses:
//   {"format":"sailtube-backup","version":1,"exportedMs":…,
//    "subscriptions":[…],"pendingHandles":[…],"history":[…],"resume":[…],
//    "playlists":[{"title":…,"items":[…]}],"remotePlaylists":[…],"settings":{…},
//    "librarySettings":{…}}
// The library part is LibraryStore::snapshot(). Settings come from the app's
// QSettings INI file, limited to an allowlist of prefixes and never a key or
// value that looks like a credential (NFR-SEC-03, ACC-06 cookie names). The
// file is written atomically (QSaveFile: private temp file, then rename) with
// mode 0600. "librarySettings" carries the LibraryStore meta values listed in
// backupLibraryMetaKeys() (read with metaValue(), written back with
// setMetaValue() when the caller applies a restore).
//
// Restore validates the document (unknown format or a newer version is
// refused) and returns an ImportBatch "sailtube" for LibraryStore::applyImport
// plus the allowlisted settings; the caller decides whether to apply them.

#include "Core/Library/Domain/LibraryTypes.h"
#include <QByteArray>
#include <QString>
#include <QStringList>
#include <QVariantMap>

namespace nordtile { namespace video { namespace library {

const int sailTubeBackupVersion = 1;
const qint64 maxBackupBytes = 256LL * 1024 * 1024;

// Prefixes "playback/", "library/", "content/", "downloads/", "ui/", and no
// "cookie", "token", "secret", "password", "auth", "sapisid" anywhere in the
// key (case-insensitive).
bool backupSettingAllowed(const QString &key);
// String values carrying ACC-06 cookie names are dropped as well.
bool backupValueAllowed(const QVariant &value);
// resume_behaviour, hide_watched, hide_watched_search, history_cap, history_paused.
QStringList backupLibraryMetaKeys();
// Allowlisted keys of an INI file (empty map when the path is empty or missing).
QVariantMap backupSettingsFromIni(const QString &iniPath);

// `librarySettings`: meta key -> value; keys outside backupLibraryMetaKeys() are dropped.
QByteArray sailTubeBackupJson(const ImportBatch &library, const QVariantMap &settings,
                              const QVariantMap &librarySettings, qint64 exportedMs);
// Writes the backup; `settings` is filtered again. False with `error` on failure.
bool writeSailTubeBackup(const QString &targetPath, const ImportBatch &library, const QVariantMap &settings,
                         const QVariantMap &librarySettings, qint64 exportedMs, QString *error = nullptr);

struct BackupRestoreResult {
    ImportBatch batch;             // source "sailtube"
    QVariantMap settings;          // allowlisted only
    QVariantMap librarySettings;   // meta key -> string value, backupLibraryMetaKeys() only
    qint64 exportedMs = 0;
    QString error;
    bool ok() const { return error.isEmpty(); }
};

BackupRestoreResult parseSailTubeBackup(const QByteArray &json);
BackupRestoreResult readSailTubeBackup(const QString &input); // accepts "file://" and "~/"

}}}
