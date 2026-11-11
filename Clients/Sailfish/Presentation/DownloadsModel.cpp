// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "DownloadsModel.h"

#include "Core/Downloads/Domain/DownloadPolicy.h"
#include "Core/Downloads/Domain/DownloadTypes.h"
#include "Core/Downloads/Infrastructure/DownloadManager.h"

namespace nordtile { namespace video { namespace sailfish {

using downloads::DownloadManager;
using downloads::State;

namespace {
QVariantMap managerHints(const QVariantMap &video) {
    QVariantMap h = video;
    if (!h.contains(QStringLiteral("thumbnailUrl"))) h.insert(QStringLiteral("thumbnailUrl"), video.value(QStringLiteral("thumbnail")));
    return h;
}
bool activeState(int s) {
    return s == int(State::Queued) || s == int(State::Resolving) || s == int(State::Downloading)
        || s == int(State::Paused) || s == int(State::WaitingForNetwork) || s == int(State::Remuxing);
}
}

DownloadsModel::DownloadsModel(DownloadManager *manager, bool backgroundCapable, QObject *parent)
    : QAbstractListModel(parent), manager_(manager), backgroundCapable_(backgroundCapable) {
    connect(manager_, &DownloadManager::recordsChanged, this, &DownloadsModel::reload);
    connect(manager_, &DownloadManager::itemChanged, this, [this](int id, const QString &videoId) {
        const int before = rowOf(id) >= 0 ? rows_.at(rowOf(id)).value(QStringLiteral("state")).toInt() : -1;
        refreshRow(id, videoId);
        const QVariantMap r = manager_->record(id);
        const int after = r.value(QStringLiteral("state")).toInt();
        if (before != after && (after == int(State::Done) || after == int(State::Failed)))
            emit finished(r.value(QStringLiteral("title")).toString(), after == int(State::Done));
    });
    connect(manager_, &DownloadManager::itemRemoved, this, [this](int, const QString &videoId) {
        reload();
        emit itemChanged(videoId);
    });
    connect(manager_, &DownloadManager::progressChanged, this, [this](int id) {
        refreshRow(id, manager_->record(id).value(QStringLiteral("videoId")).toString());
    });
    connect(manager_, &DownloadManager::notice, this, [this](const QString &key) { emit notice(noticeText(key)); });
    connect(manager_, &DownloadManager::activeCountChanged, this, &DownloadsModel::activeChanged);
    connect(manager_, &DownloadManager::keepAliveWantedChanged, this, &DownloadsModel::activeChanged);
    connect(manager_, &DownloadManager::unmeteredOnlyChanged, this, &DownloadsModel::settingsChanged);
    connect(manager_, &DownloadManager::sidecarSettingsChanged, this, &DownloadsModel::settingsChanged);
    connect(manager_, &DownloadManager::folderChanged, this, &DownloadsModel::settingsChanged);
    connect(manager_, &DownloadManager::defaultQualityHeightChanged, this, &DownloadsModel::settingsChanged);
    reload();
}

int DownloadsModel::rowCount(const QModelIndex &parent) const { return parent.isValid() ? 0 : rows_.size(); }

QVariant DownloadsModel::data(const QModelIndex &index, int role) const {
    if (!index.isValid() || index.row() >= rows_.size()) return QVariant();
    const QVariantMap &r = rows_.at(index.row());
    switch (role) {
    case IdRole: return r.value(QStringLiteral("id"));
    case VideoIdRole: return r.value(QStringLiteral("videoId"));
    case KindRole: return r.value(QStringLiteral("kind"));
    case StateRole: return r.value(QStringLiteral("state"));
    case TitleRole: return r.value(QStringLiteral("title"));
    case ChannelRole: return r.value(QStringLiteral("channel"));
    case ThumbnailRole: {
        const QString local = r.value(QStringLiteral("thumbnailPath")).toString();
        return local.isEmpty() ? r.value(QStringLiteral("thumbnailUrl")) : QVariant(QStringLiteral("file://") + local);
    }
    case FractionRole: return r.value(QStringLiteral("fraction"));
    case BytesDoneRole: return r.value(QStringLiteral("bytesDone"));
    case BytesTotalRole: return r.value(QStringLiteral("bytesTotal"));
    case ErrorRole: return r.value(QStringLiteral("error"));
    case LayoutRole: return r.value(QStringLiteral("layout"));
    case QualityRole: return r.value(QStringLiteral("qualityHeight"));
    case FilePathRole: return r.value(QStringLiteral("filePath"));
    case AvailableRole: return r.value(QStringLiteral("available"));
    default: return QVariant();
    }
}

QHash<int, QByteArray> DownloadsModel::roleNames() const {
    return { { IdRole, "downloadId" }, { VideoIdRole, "videoId" }, { KindRole, "kind" }, { StateRole, "state" },
             { TitleRole, "title" }, { ChannelRole, "channel" }, { ThumbnailRole, "thumbnail" },
             { FractionRole, "fraction" }, { BytesDoneRole, "bytesDone" }, { BytesTotalRole, "bytesTotal" },
             { ErrorRole, "error" }, { LayoutRole, "layout" }, { QualityRole, "qualityHeight" },
             { FilePathRole, "filePath" }, { AvailableRole, "available" } };
}

int DownloadsModel::activeCount() const { return manager_->activeCount(); }
bool DownloadsModel::keepAliveWanted() const { return manager_->keepAliveWanted(); }
bool DownloadsModel::unmeteredOnly() const { return manager_->unmeteredOnly(); }
void DownloadsModel::setUnmeteredOnly(bool on) { manager_->setUnmeteredOnly(on); }
QString DownloadsModel::folder() const { return manager_->folder(); }
QString DownloadsModel::defaultFolder() const { return manager_->defaultFolder(); }
int DownloadsModel::defaultQualityHeight() const { return manager_->defaultQualityHeight(); }
int DownloadsModel::defaultKind() const { return manager_->defaultKind(); }
bool DownloadsModel::saveCaptions() const { return manager_->saveCaptions(); }
void DownloadsModel::setSaveCaptions(bool on) { manager_->setSaveCaptions(on); }
bool DownloadsModel::saveSegments() const { return manager_->saveSegments(); }
void DownloadsModel::setSaveSegments(bool on) { manager_->setSaveSegments(on); }

int DownloadsModel::enqueue(const QVariantMap &video, int kind, int qualityHeight) {
    return manager_->enqueue(managerHints(video), kind, qualityHeight);
}

int DownloadsModel::enqueueAll(const QVariantList &videos, int kind, int qualityHeight) {
    QVariantList list;
    for (const QVariant &v : videos) list.append(managerHints(v.toMap()));
    return manager_->enqueueAll(list, kind, qualityHeight);
}

bool DownloadsModel::pause(int id) { return manager_->pause(id); }
bool DownloadsModel::resume(int id) { return manager_->resume(id); }
bool DownloadsModel::retry(int id) { return manager_->retry(id); }
bool DownloadsModel::cancel(int id) { return manager_->cancel(id); }
bool DownloadsModel::remove(int id, bool deleteFiles) { return manager_->remove(id, deleteFiles); }

QVariantMap DownloadsModel::stateFor(const QString &videoId) const {
    const int s = manager_->state(videoId);
    QVariantMap m;
    m.insert(QStringLiteral("state"), s);
    m.insert(QStringLiteral("active"), activeState(s));
    m.insert(QStringLiteral("paused"), s == int(State::Paused) || s == int(State::WaitingForNetwork));
    m.insert(QStringLiteral("done"), s == int(State::Done));
    m.insert(QStringLiteral("failed"), s == int(State::Failed));
    m.insert(QStringLiteral("progress"), s < 0 ? 0.0 : static_cast<double>(manager_->fraction(videoId)));
    m.insert(QStringLiteral("kind"), s < 0 ? -1 : manager_->item(videoId).value(QStringLiteral("kind")).toInt());
    return m;
}

QVariantMap DownloadsModel::localMedia(const QString &videoId) { return manager_->localMedia(videoId); }

QVariantMap DownloadsModel::chooseFolder(const QString &path) {
    QVariantMap m = manager_->chooseFolder(path);
    if (!m.value(QStringLiteral("ok")).toBool()) m.insert(QStringLiteral("error"), errorText(m.value(QStringLiteral("errorKey")).toString()));
    return m;
}

void DownloadsModel::resetFolder() { manager_->resetFolder(); }
QStringList DownloadsModel::candidateFolders() const { return manager_->candidateFolders(); }

QString DownloadsModel::stateText(int state, const QString &error) const {
    switch (static_cast<State>(state)) {
    //: Download state
    case State::Queued: return tr("Queued");
    //: Download state while the stream address is fetched
    case State::Resolving: return tr("Preparing");
    //: Download state
    case State::Downloading: return tr("Downloading");
    //: Download state
    case State::Paused: return tr("Paused");
    //: Download state when only unmetered connections may be used
    case State::WaitingForNetwork: return tr("Waiting for Wi-Fi");
    //: Download state while the video and audio tracks are joined
    case State::Remuxing: return tr("Joining tracks");
    //: Download state
    case State::Done: return tr("Downloaded");
    case State::Failed: return errorText(error);
    case State::Cancelled: break;
    }
    return QString();
}

QString DownloadsModel::noticeText(const QString &key) {
    if (key == QLatin1String(downloads::noticeKeys::folderUnavailable)) {
        //: Notice when the chosen download folder (e.g. an SD card) is missing
        return tr("The download folder is unavailable. Saving in the app's own folder instead.");
    }
    if (key == QLatin1String(downloads::noticeKeys::remuxFailed)) {
        //: Notice when the video and audio of a download could not be joined
        return tr("Video and audio could not be joined; both tracks were kept.");
    }
    return key;
}

QString DownloadsModel::errorText(const QString &code) {
    using namespace downloads::errorCodes;
    if (code == QLatin1String(noStream)) return tr("No downloadable stream");
    //: Download failure when the file size changed between attempts
    if (code == QLatin1String(sizeMismatch)) return tr("Download failed: size changed");
    if (code == QLatin1String(forbidden)) return tr("Download refused by YouTube");
    if (code == QLatin1String(network) || code == QLatin1String(http)) return tr("Network error");
    //: Download failure when the file could not be written
    if (code == QLatin1String(write)) return tr("Could not write the file");
    if (code == QLatin1String(fileMissing)) return tr("File missing");
    if (code == QLatin1String(location)) return tr("No writable download folder");
    //: Download failure when the server sent a web page instead of the video or audio
    if (code == QLatin1String(notMedia)) return tr("The server did not send a video file");
    //: Download failure when the storage is too full for the rest of the download
    if (code == QLatin1String(noSpace)) return tr("Not enough free space");
    if (code.startsWith(QLatin1String("downloads_error_folder"))) {
        //: Shown when a chosen download folder cannot be used
        return tr("This folder cannot be written to");
    }
    //: Generic download failure
    return tr("Download failed");
}

void DownloadsModel::reload() {
    beginResetModel();
    rows_.clear();
    const QVariantList all = manager_->records();
    for (const QVariant &v : all) rows_.append(v.toMap());
    endResetModel();
    emit countChanged();
}

int DownloadsModel::rowOf(int id) const {
    for (int i = 0; i < rows_.size(); ++i) if (rows_.at(i).value(QStringLiteral("id")).toInt() == id) return i;
    return -1;
}

void DownloadsModel::refreshRow(int id, const QString &videoId) {
    const int row = rowOf(id);
    if (row < 0) { reload(); emit itemChanged(videoId); return; }
    rows_[row] = manager_->record(id);
    emit dataChanged(index(row, 0), index(row, 0));
    emit itemChanged(videoId);
}

}}}
