// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#pragma once

// QML view of the download manager (DL-04): the downloads page list with live
// progress, per-video lookups for row badges and the watch-page indicator, and
// notices as translated text. One signal per changed video, at most ~4 Hz
// while bytes move (the acceptance asks for at least one update per second).

#include <QAbstractListModel>
#include <QVariantMap>

namespace nordtile { namespace video {
namespace downloads { class DownloadManager; }
namespace sailfish {

class DownloadsModel final : public QAbstractListModel {
    Q_OBJECT
    Q_PROPERTY(int count READ count NOTIFY countChanged)
    Q_PROPERTY(int activeCount READ activeCount NOTIFY activeChanged)
    Q_PROPERTY(bool keepAliveWanted READ keepAliveWanted NOTIFY activeChanged)
    Q_PROPERTY(bool unmeteredOnly READ unmeteredOnly WRITE setUnmeteredOnly NOTIFY settingsChanged)
    Q_PROPERTY(QString folder READ folder NOTIFY settingsChanged)
    Q_PROPERTY(QString defaultFolder READ defaultFolder CONSTANT)
    Q_PROPERTY(int defaultQualityHeight READ defaultQualityHeight NOTIFY settingsChanged)
    // The "Download quality" setting: 0 audio, 1 video (what the dialog and "Download all" start with).
    Q_PROPERTY(int defaultKind READ defaultKind NOTIFY settingsChanged)
    // DL-07: keep captions / SponsorBlock segments with downloads for offline playback.
    Q_PROPERTY(bool saveCaptions READ saveCaptions WRITE setSaveCaptions NOTIFY settingsChanged)
    Q_PROPERTY(bool saveSegments READ saveSegments WRITE setSaveSegments NOTIFY settingsChanged)
    // STORE builds run downloads only while the app is open (DL-09).
    Q_PROPERTY(bool backgroundCapable READ backgroundCapable CONSTANT)
public:
    enum Role { IdRole = Qt::UserRole + 1, VideoIdRole, KindRole, StateRole, TitleRole, ChannelRole,
                ThumbnailRole, FractionRole, BytesDoneRole, BytesTotalRole, ErrorRole, LayoutRole,
                QualityRole, FilePathRole, AvailableRole };
    DownloadsModel(downloads::DownloadManager *manager, bool backgroundCapable, QObject *parent = nullptr);

    int rowCount(const QModelIndex &parent = QModelIndex()) const override;
    QVariant data(const QModelIndex &index, int role) const override;
    QHash<int, QByteArray> roleNames() const override;

    int count() const { return rows_.size(); }
    int activeCount() const;
    bool keepAliveWanted() const;
    bool unmeteredOnly() const;
    void setUnmeteredOnly(bool on);
    QString folder() const;
    QString defaultFolder() const;
    int defaultQualityHeight() const;
    int defaultKind() const;
    bool saveCaptions() const;
    void setSaveCaptions(bool on);
    bool saveSegments() const;
    void setSaveSegments(bool on);
    bool backgroundCapable() const { return backgroundCapable_; }

    // video uses the shared keys (videoId, title, channel, channelId, durationSeconds, thumbnail).
    // kind: 0 audio, 1 video. qualityHeight 0 = the playback ceiling.
    Q_INVOKABLE int enqueue(const QVariantMap &video, int kind, int qualityHeight);
    Q_INVOKABLE int enqueueAll(const QVariantList &videos, int kind, int qualityHeight);
    Q_INVOKABLE bool pause(int id);
    Q_INVOKABLE bool resume(int id);
    Q_INVOKABLE bool retry(int id);
    Q_INVOKABLE bool cancel(int id);
    Q_INVOKABLE bool remove(int id, bool deleteFiles);
    // {active, done, failed, paused, progress, state, kind}
    Q_INVOKABLE QVariantMap stateFor(const QString &videoId) const;
    Q_INVOKABLE QVariantMap localMedia(const QString &videoId);
    // {ok, path, error (translated)}
    Q_INVOKABLE QVariantMap chooseFolder(const QString &path);
    Q_INVOKABLE void resetFolder();
    Q_INVOKABLE QStringList candidateFolders() const;
    Q_INVOKABLE QString stateText(int state, const QString &error) const;

    static QString noticeText(const QString &key);
    static QString errorText(const QString &code);

signals:
    void countChanged();
    void activeChanged();
    void settingsChanged();
    void itemChanged(const QString &videoId);
    // A visible notice (DL-06 fallback, remux fallback), already translated.
    void notice(const QString &text);
    // Finished or failed, for the system notification.
    void finished(const QString &title, bool ok);

private:
    void reload();
    void refreshRow(int id, const QString &videoId);
    int rowOf(int id) const;

    downloads::DownloadManager *manager_;
    bool backgroundCapable_;
    QList<QVariantMap> rows_;
};

}}}
