// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "ImportController.h"

#include "Core/Library/Infrastructure/AccountImport.h"
#include "Core/Library/Infrastructure/ImportJob.h"
#include "Core/Library/Infrastructure/LibraryStore.h"
#include "Core/Library/Infrastructure/SailTubeBackup.h"
#include <QDateTime>
#include <QDir>
#include <QSettings>

namespace nordtile { namespace video { namespace sailfish {

using library::ImportJob;

ImportController::ImportController(library::LibraryStore *store, const QString &privateDir,
                                   const QString &settingsIniPath, QObject *parent)
    : QObject(parent), store_(store), job_(new ImportJob(store->databasePath(), privateDir, this)),
      settingsPath_(settingsIniPath) {
    connect(job_, &ImportJob::finished, this, &ImportController::finished);
}

ImportController::~ImportController() {}

bool ImportController::busy() const { return job_->isRunning() || (account_ && account_->isRunning()); }

void ImportController::setAccountTransport(library::AccountBrowseTransport *transport) {
    if (account_) { account_->cancel(); account_->deleteLater(); account_ = nullptr; }
    if (transport) {
        account_ = new library::AccountImporter(transport, this);
        connect(account_, &library::AccountImporter::finished, this, &ImportController::accountFinished);
    }
    emit changed();
}

void ImportController::setAccountIdentity(int authUser, const QString &pageId) {
    authUser_ = authUser;
    pageId_ = pageId;
}

bool ImportController::accountImportAvailable() const { return account_ != nullptr; }

bool ImportController::startAccountImport() {
    if (!account_ || busy()) return false;
    library::AccountIdentity identity;
    identity.authUser = authUser_;
    identity.pageId = pageId_;
    applySettings_ = false;
    exporting_ = false;
    if (!account_->start(identity)) return false;
    summary_.clear();
    error_.clear();
    emit changed();
    return true;
}

void ImportController::accountFinished(const library::ImportBatch &batch, const QString &error) {
    if (!error.isEmpty() || !job_->startBatch(batch)) {
        //: Import error: reading the subscriptions and playlists of the signed-in account failed
        error_ = tr("Could not read the account. Check the connection and try again.");
        emit changed();
        emit done(false);
        return;
    }
    emit changed();
}

bool ImportController::startImport(const QString &kind, const QString &path, bool applySettings) {
    ImportJob::Kind k;
    if (kind == QLatin1String("newpipe")) k = ImportJob::Kind::NewPipe;
    else if (kind == QLatin1String("takeout")) k = ImportJob::Kind::TakeoutCsv;
    else if (kind == QLatin1String("opml")) k = ImportJob::Kind::Opml;
    else if (kind == QLatin1String("backup")) k = ImportJob::Kind::SailTubeBackup;
    else return false;
    applySettings_ = applySettings && k == ImportJob::Kind::SailTubeBackup;
    exporting_ = false;
    if (!job_->start(k, path)) return false;
    summary_.clear();
    error_.clear();
    emit changed();
    return true;
}

bool ImportController::exportBackup(const QString &folder) {
    const QString name = QStringLiteral("SailTube-backup-%1.json")
        .arg(QDateTime::currentDateTime().toString(QStringLiteral("yyyyMMdd-HHmm")));
    const QString target = QDir(folder).filePath(name);
    exporting_ = true;
    applySettings_ = false;
    if (!job_->startExport(target, settingsPath_)) return false;
    summary_.clear();
    error_.clear();
    emit changed();
    return true;
}

void ImportController::finished(const QVariantMap &summary, const QString &error) {
    summary_ = summary;
    error_ = error;
    if (error.isEmpty() && !exporting_) {
        if (applySettings_) {
            ImportJob::applyLibrarySettings(*store_, summary.value(QStringLiteral("librarySettings")).toMap());
            applySettings(summary.value(QStringLiteral("settings")).toMap());
        }
        store_->reload();
    }
    if (error.isEmpty() && exporting_) exportPath_ = summary.value(QStringLiteral("path")).toString();
    emit changed();
    emit done(error.isEmpty());
}

void ImportController::applySettings(const QVariantMap &settings) {
    if (settingsPath_.isEmpty() || settings.isEmpty()) return;
    QSettings ini(settingsPath_, QSettings::IniFormat);
    for (auto it = settings.constBegin(); it != settings.constEnd(); ++it)
        if (library::backupSettingAllowed(it.key()) && library::backupValueAllowed(it.value()))
            ini.setValue(it.key(), it.value());
    ini.sync();
}

QString ImportController::summaryText() const {
    if (!error_.isEmpty()) return error_;
    if (summary_.isEmpty()) return QString();
    if (exporting_) return tr("Backup saved to %1").arg(exportPath_);
    if (summary_.value(QStringLiteral("nothingNew")).toBool()) {
        //: Import summary when everything was already in the library
        return tr("Nothing new to import.");
    }
    QStringList parts;
    const auto add = [&](const char *key, const QString &text) {
        const int n = summary_.value(QString::fromLatin1(key)).toInt();
        if (n > 0) parts << text.arg(n);
    };
    //: Import summary part, %1 is a number
    add("subscriptionsAdded", tr("%1 subscriptions"));
    //: Import summary part, %1 is a number of channels to look up online
    add("handlesPending", tr("%1 channels to look up"));
    //: Import summary part, %1 is a number
    add("historyAdded", tr("%1 history entries"));
    //: Import summary part, %1 is a number
    add("resumeAdded", tr("%1 resume points"));
    //: Import summary part, %1 is a number
    add("playlistsAdded", tr("%1 playlists"));
    //: Import summary part, %1 is a number
    add("remotePlaylistsAdded", tr("%1 saved YouTube playlists"));
    //: Import summary part, %1 is a number of playlists skipped because a list of that name exists
    add("playlistsSkipped", tr("%1 playlists skipped (same name)"));
    //: Import summary, %1 is a comma-separated list of what was added
    return tr("Imported: %1").arg(parts.join(QStringLiteral(", ")));
}

}}}
