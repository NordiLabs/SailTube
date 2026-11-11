// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#pragma once

// QML entry to imports and the SailTube backup (IMP-01, IMP-03, IMP-04). Runs
// one ImportJob at a time off the playback thread, then reloads the primary
// store and reports one summary. IMP-02: the account component hands in its
// signed-in browse transport and the selected identity; the page then offers
// "Import from the YouTube account".

#include <QObject>
#include <QVariantMap>

namespace nordtile { namespace video {
namespace library { class AccountBrowseTransport; class AccountImporter; class ImportJob; class LibraryStore;
                    struct ImportBatch; }
namespace sailfish {

class ImportController final : public QObject {
    Q_OBJECT
    Q_PROPERTY(bool busy READ busy NOTIFY changed)
    Q_PROPERTY(QVariantMap summary READ summary NOTIFY changed)
    Q_PROPERTY(QString errorText READ errorText NOTIFY changed)
    Q_PROPERTY(QString lastExportPath READ lastExportPath NOTIFY changed)
    Q_PROPERTY(bool accountImportAvailable READ accountImportAvailable NOTIFY changed)
public:
    ImportController(library::LibraryStore *store, const QString &privateDir, const QString &settingsIniPath,
                     QObject *parent = nullptr);
    ~ImportController() override;

    bool busy() const;
    QVariantMap summary() const { return summary_; }
    QString errorText() const { return error_; }
    QString lastExportPath() const { return exportPath_; }

    // IMP-02 (account component): null transport = signed out. The identity is the
    // selected login and brand channel (empty page id = the login's own channel).
    void setAccountTransport(library::AccountBrowseTransport *transport);
    void setAccountIdentity(int authUser, const QString &pageId);
    bool accountImportAvailable() const;
    Q_INVOKABLE bool startAccountImport();

    // kind: "newpipe", "takeout", "opml" or "backup". applySettings: backup restore only.
    Q_INVOKABLE bool startImport(const QString &kind, const QString &path, bool applySettings);
    // Writes "SailTube-backup-<date>.json" into `folder`.
    Q_INVOKABLE bool exportBackup(const QString &folder);
    // Text for the one summary shown at the end.
    Q_INVOKABLE QString summaryText() const;

signals:
    void changed();
    void done(bool ok);

private:
    void finished(const QVariantMap &summary, const QString &error);
    void applySettings(const QVariantMap &settings);
    void accountFinished(const library::ImportBatch &batch, const QString &error);

    library::LibraryStore *store_;
    library::ImportJob *job_;
    library::AccountImporter *account_ = nullptr;
    int authUser_ = 0;
    QString pageId_;
    QString settingsPath_;
    QVariantMap summary_;
    QString error_;
    QString exportPath_;
    bool applySettings_ = false;
    bool exporting_ = false;
};

}}}
