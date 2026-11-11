// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Context-property fakes with the API the library component's QML calls (see
// Clients/Sailfish/Presentation/LibraryModels.h and DownloadsModel.h). They
// count the calls the tests assert on. Build: tests/library_qml_behaviour/run.sh
#pragma once
#include <QAbstractListModel>
#include <QElapsedTimer>
#include <QHash>
#include <QQmlEngine>
#include <QStringList>
#include <QVariantMap>
#include <QVector>

// watchIndex: state() follows the overlay rule of LibraryPolicy (bar above 0.5 %).
class FakeWatchIndex : public QObject {
    Q_OBJECT
    Q_PROPERTY(bool hideWatched READ off CONSTANT)
    Q_PROPERTY(bool hideWatchedInSearch READ off CONSTANT)
public:
    QHash<QString, int> stateCalls;
    QHash<QString, double> progress;
    QHash<QString, bool> watched;

    bool off() const { return false; }
    Q_INVOKABLE QVariantMap state(const QString &videoId) {
        ++stateCalls[videoId];
        const double p = progress.value(videoId, 0.0);
        QVariantMap m;
        m.insert(QStringLiteral("showProgress"), p > 0.005);
        m.insert(QStringLiteral("progress"), p);
        m.insert(QStringLiteral("watched"), watched.value(videoId, false));
        return m;
    }
    Q_INVOKABLE bool hidden(const QString &, const QString &) const { return false; }
    Q_INVOKABLE double resumePosition(const QString &) const { return -1; }
    void set(const QString &videoId, double p, bool w) {
        progress.insert(videoId, p);
        watched.insert(videoId, w);
        emit changed(videoId);
    }
signals:
    void changed(const QString &videoId);
    void settingsChanged();
};

// downloads: list model with the real role names plus stateFor()/itemChanged.
class FakeDownloads : public QAbstractListModel {
    Q_OBJECT
    Q_PROPERTY(int count READ count NOTIFY countChanged)
    Q_PROPERTY(int activeCount READ activeCount NOTIFY activeChanged)
    Q_PROPERTY(bool keepAliveWanted READ keepAliveWanted NOTIFY activeChanged)
    Q_PROPERTY(bool backgroundCapable READ backgroundCapable CONSTANT)
    // The "Download quality" setting as DownloadsModel exposes it.
    Q_PROPERTY(int defaultKind MEMBER defaultKind NOTIFY settingsChanged)
    Q_PROPERTY(int defaultQualityHeight MEMBER defaultQualityHeight NOTIFY settingsChanged)
public:
    struct Row { int id; QString videoId; QString title; int state; double fraction; };
    struct Enqueued { QString videoId; int kind; int qualityHeight; };
    int defaultKind = 1;
    int defaultQualityHeight = 1080;
    QVector<Enqueued> enqueued;
    enum Roles { IdRole = Qt::UserRole + 1, VideoIdRole, KindRole, StateRole, TitleRole, ChannelRole,
                 ThumbnailRole, FractionRole, BytesDoneRole, BytesTotalRole, ErrorRole, LayoutRole,
                 QualityRole, FilePathRole, AvailableRole };
    QVector<Row> rows;
    QHash<QString, int> stateForCalls;

    int rowCount(const QModelIndex &parent = QModelIndex()) const override { return parent.isValid() ? 0 : rows.size(); }
    QHash<int, QByteArray> roleNames() const override {
        return { { IdRole, "downloadId" }, { VideoIdRole, "videoId" }, { KindRole, "kind" }, { StateRole, "state" },
                 { TitleRole, "title" }, { ChannelRole, "channel" }, { ThumbnailRole, "thumbnail" },
                 { FractionRole, "fraction" }, { BytesDoneRole, "bytesDone" }, { BytesTotalRole, "bytesTotal" },
                 { ErrorRole, "error" }, { LayoutRole, "layout" }, { QualityRole, "qualityHeight" },
                 { FilePathRole, "filePath" }, { AvailableRole, "available" } };
    }
    QVariant data(const QModelIndex &index, int role) const override {
        if (!index.isValid() || index.row() >= rows.size()) return QVariant();
        const Row &r = rows.at(index.row());
        switch (role) {
        case IdRole: return r.id;
        case VideoIdRole: return r.videoId;
        case KindRole: return 1;
        case StateRole: return r.state;
        case TitleRole: return r.title;
        case ChannelRole: return QStringLiteral("Channel");
        case ThumbnailRole: return QString();
        case FractionRole: return r.fraction;
        case BytesDoneRole: return qint64(r.fraction * 1000000);
        case BytesTotalRole: return qint64(1000000);
        case ErrorRole: return QString();
        case LayoutRole: return QStringLiteral("single");
        case QualityRole: return 720;
        case FilePathRole: return QString();
        case AvailableRole: return r.state == 6;
        }
        return QVariant();
    }
    int count() const { return rows.size(); }
    int activeCount() const { int n = 0; for (const Row &r : rows) n += active(r.state) ? 1 : 0; return n; }
    bool keepAliveWanted() const { return activeCount() > 0; }
    bool backgroundCapable() const { return true; }
    static bool active(int s) { return s <= 2 || s == 5; }

    Q_INVOKABLE QVariantMap stateFor(const QString &videoId) {
        ++stateForCalls[videoId];
        int s = -1; double f = 0;
        for (const Row &r : rows) if (r.videoId == videoId) { s = r.state; f = r.fraction; }
        QVariantMap m;
        m.insert(QStringLiteral("state"), s);
        m.insert(QStringLiteral("active"), s >= 0 && active(s));
        m.insert(QStringLiteral("paused"), s == 3 || s == 4);
        m.insert(QStringLiteral("done"), s == 6);
        m.insert(QStringLiteral("failed"), s == 7);
        m.insert(QStringLiteral("progress"), s < 0 ? 0.0 : f);
        m.insert(QStringLiteral("kind"), s < 0 ? -1 : 1);
        return m;
    }
    Q_INVOKABLE QString stateText(int state, const QString &) const {
        return state == 6 ? QStringLiteral("Done") : QStringLiteral("Downloading");
    }
    Q_INVOKABLE QVariantMap localMedia(const QString &) { return QVariantMap(); }
    Q_INVOKABLE bool pause(int) { return true; }
    Q_INVOKABLE bool resume(int) { return true; }
    Q_INVOKABLE bool retry(int) { return true; }
    Q_INVOKABLE bool cancel(int) { return true; }
    Q_INVOKABLE bool remove(int, bool) { return true; }
    Q_INVOKABLE int enqueue(const QVariantMap &video, int kind, int qualityHeight) {
        Enqueued e; e.videoId = video.value(QStringLiteral("videoId")).toString(); e.kind = kind; e.qualityHeight = qualityHeight;
        enqueued.append(e);
        return enqueued.size();
    }
    Q_INVOKABLE int enqueueAll(const QVariantList &, int, int) { return 0; }

    void add(int id, const QString &videoId, int state, double fraction) {
        beginInsertRows(QModelIndex(), rows.size(), rows.size());
        Row r; r.id = id; r.videoId = videoId; r.title = videoId; r.state = state; r.fraction = fraction;
        rows.append(r);
        endInsertRows();
        emit countChanged();
        emit itemChanged(videoId);
    }
    // What the real model does on a progress tick: refresh the row, then name the video.
    void update(const QString &videoId, int state, double fraction) {
        for (int i = 0; i < rows.size(); ++i) {
            if (rows[i].videoId != videoId) continue;
            rows[i].state = state; rows[i].fraction = fraction;
            emit dataChanged(index(i), index(i));
        }
        emit activeChanged();
        emit itemChanged(videoId);
    }
signals:
    void countChanged();
    void activeChanged();
    void settingsChanged();
    void itemChanged(const QString &videoId);
    void notice(const QString &text);
    void finished(const QString &title, bool ok);
};

// One playlist as PlaylistsModel::openList returns it; remove() only counts and drops the row.
class FakePlaylistItems : public QAbstractListModel {
    Q_OBJECT
    Q_PROPERTY(QString playlistId READ playlistId NOTIFY playlistChanged)
    Q_PROPERTY(QString title READ title NOTIFY playlistChanged)
    Q_PROPERTY(int kind READ kind NOTIFY playlistChanged)
    Q_PROPERTY(QString remoteId READ remoteId NOTIFY playlistChanged)
    Q_PROPERTY(double refreshed READ refreshed NOTIFY playlistChanged)
    Q_PROPERTY(int count READ count NOTIFY playlistChanged)
public:
    enum Roles { VideoIdRole = Qt::UserRole + 1, TitleRole, ChannelRole, ChannelIdRole, DurationSecondsRole, ThumbnailRole };
    QString id;
    QStringList videos;
    QStringList removed;        // every remove() call, in order
    QVector<qint64> removedAt;  // clock time of each call
    QElapsedTimer clock;

    FakePlaylistItems() { clock.start(); }
    QString playlistId() const { return id; }
    QString title() const { return QStringLiteral("Test list"); }
    int kind() const { return 0; }
    QString remoteId() const { return QString(); }
    double refreshed() const { return 0; }
    int count() const { return videos.size(); }
    int rowCount(const QModelIndex &parent = QModelIndex()) const override { return parent.isValid() ? 0 : videos.size(); }
    QHash<int, QByteArray> roleNames() const override {
        return { { VideoIdRole, "videoId" }, { TitleRole, "title" }, { ChannelRole, "channel" },
                 { ChannelIdRole, "channelId" }, { DurationSecondsRole, "durationSeconds" }, { ThumbnailRole, "thumbnail" } };
    }
    QVariant data(const QModelIndex &index, int role) const override {
        if (!index.isValid() || index.row() >= videos.size()) return QVariant();
        const QString &v = videos.at(index.row());
        switch (role) {
        case VideoIdRole: return v;
        case TitleRole: return QStringLiteral("Title ") + v;
        case ChannelRole: return QStringLiteral("Channel");
        case ChannelIdRole: return QStringLiteral("UC1");
        case DurationSecondsRole: return 61;
        case ThumbnailRole: return QString();
        }
        return QVariant();
    }
    Q_INVOKABLE bool remove(const QString &videoId) {
        removed.append(videoId);
        removedAt.append(clock.elapsed());
        const int i = videos.indexOf(videoId);
        if (i < 0) return false;
        beginRemoveRows(QModelIndex(), i, i);
        videos.removeAt(i);
        endRemoveRows();
        emit playlistChanged();
        return true;
    }
    Q_INVOKABLE bool move(int, int) { return false; }
    Q_INVOKABLE QVariantMap get(int row) const {
        QVariantMap m;
        if (row >= 0 && row < videos.size()) m.insert(QStringLiteral("videoId"), videos.at(row));
        return m;
    }
    Q_INVOKABLE QVariantList items(bool) const { return QVariantList(); }
    Q_INVOKABLE bool replaceRemoteItems(const QVariantList &) { return false; }
signals:
    void playlistChanged();
};

// playlistsModel: Watch later first, then local lists; openList() hands out one FakePlaylistItems.
class FakePlaylists : public QAbstractListModel {
    Q_OBJECT
    Q_PROPERTY(int count READ count NOTIFY countChanged)
    Q_PROPERTY(int revision READ count NOTIFY countChanged)
public:
    enum Roles { IdRole = Qt::UserRole + 1, KindRole, TitleRole, ItemCountRole, ThumbnailRole, RemoteIdRole,
                 ChannelRole, WatchLaterRole, MirrorRole, RefreshedRole };
    QStringList ids;
    QStringList removed;
    QVector<qint64> removedAt;
    QElapsedTimer clock;
    FakePlaylistItems *list = nullptr;

    FakePlaylists() { clock.start(); }
    int count() const { return ids.size(); }
    int rowCount(const QModelIndex &parent = QModelIndex()) const override { return parent.isValid() ? 0 : ids.size(); }
    QHash<int, QByteArray> roleNames() const override {
        return { { IdRole, "playlistId" }, { KindRole, "kind" }, { TitleRole, "title" }, { ItemCountRole, "itemCount" },
                 { ThumbnailRole, "thumbnail" }, { RemoteIdRole, "remoteId" }, { ChannelRole, "channel" },
                 { WatchLaterRole, "watchLater" }, { MirrorRole, "mirror" }, { RefreshedRole, "refreshed" } };
    }
    QVariant data(const QModelIndex &index, int role) const override {
        if (!index.isValid() || index.row() >= ids.size()) return QVariant();
        const QString &id = ids.at(index.row());
        const bool wl = id == QLatin1String("wl");
        switch (role) {
        case IdRole: return id;
        case KindRole: return wl ? 1 : 0;
        case TitleRole: return QStringLiteral("List ") + id;
        case ItemCountRole: return 3;
        case ThumbnailRole: return QString();
        case RemoteIdRole: return QString();
        case ChannelRole: return QString();
        case WatchLaterRole: return wl;
        case MirrorRole: return false;
        case RefreshedRole: return 0;
        }
        return QVariant();
    }
    Q_INVOKABLE QString create(const QString &) { return QString(); }
    Q_INVOKABLE bool rename(const QString &, const QString &) { return false; }
    Q_INVOKABLE bool move(const QString &, int) { return false; }
    Q_INVOKABLE bool remove(const QString &id) {
        removed.append(id);
        removedAt.append(clock.elapsed());
        const int i = ids.indexOf(id);
        if (i < 0) return false;
        beginRemoveRows(QModelIndex(), i, i);
        ids.removeAt(i);
        endRemoveRows();
        emit countChanged();
        return true;
    }
    Q_INVOKABLE bool inWatchLater(const QString &) const { return false; }
    // The real model hands the page a new object; this one lives on the test's stack.
    Q_INVOKABLE QObject *openList(const QString &) {
        QQmlEngine::setObjectOwnership(list, QQmlEngine::CppOwnership);
        return list;
    }
signals:
    void countChanged();
};
