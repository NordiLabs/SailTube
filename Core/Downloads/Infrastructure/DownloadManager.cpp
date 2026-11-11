// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "DownloadManager.h"

#include <QDir>
#include <QFileInfo>

namespace nordtile { namespace video { namespace downloads {

namespace {

const char folderKey[] = "downloads.folder";
const char unmeteredKey[] = "downloads.unmeteredOnly";
const char captionsKey[] = "downloads.saveCaptions";
const char segmentsKey[] = "downloads.saveSegments";

QString sidecarRoot(const DownloadManagerConfig &c) {
    if (!c.sidecarDirectory.isEmpty()) return c.sidecarDirectory;
    return QFileInfo(c.databasePath).absolutePath() + QStringLiteral("/sidecars");
}

QString qs(const std::string &s) { return QString::fromStdString(s); }

const char *layoutName(OutputLayout l) {
    switch (l) {
    case OutputLayout::SingleFile: return "single";
    case OutputLayout::MuxedFile: return "muxed";
    case OutputLayout::TrackPair: return "pair";
    }
    return "single";
}

bool isFile(const QString &path) { return !path.isEmpty() && QFileInfo(path).isFile(); }

library::VideoEntry entryFrom(const QVariantMap &m) {
    library::VideoEntry e;
    e.videoId = m.value(QStringLiteral("videoId")).toString().toStdString();
    e.title = m.value(QStringLiteral("title")).toString().toStdString();
    e.channel = m.value(QStringLiteral("channel")).toString().toStdString();
    e.channelId = m.value(QStringLiteral("channelId")).toString().toStdString();
    const QVariant duration = m.contains(QStringLiteral("durationSeconds")) ? m.value(QStringLiteral("durationSeconds"))
                                                                             : m.value(QStringLiteral("duration"));
    e.durationSeconds = duration.isValid() ? duration.toLongLong() : -1;
    const QVariant thumb = m.contains(QStringLiteral("thumbnailUrl")) ? m.value(QStringLiteral("thumbnailUrl"))
                                                                       : m.value(QStringLiteral("thumbnail"));
    e.thumbnailUrl = thumb.toString().toStdString();
    return e;
}

} // namespace

DownloadManager::DownloadManager(const DownloadManagerConfig &config, DownloadResolver *resolver, Remuxer *remuxer,
                                 QObject *parent)
    : QObject(parent), config_(config), location_(config.roots), store_(config.databasePath, config.connectionName),
      sidecars_(sidecarRoot(config)) {
    EngineOptions options = config.engine;
    options.locate = [this](const QString &current) {
        if (!current.isEmpty() && DownloadLocation::writable(current)) {
            LocationDecision keep;
            keep.directory = current.toStdString();
            return keep;
        }
        return location_.resolve(current.isEmpty() ? folder_ : current);
    };
    engine_.reset(new DownloadEngine(&store_, resolver, remuxer, options));
    connect(engine_.get(), &DownloadEngine::recordAdded, this, &DownloadManager::onAdded);
    connect(engine_.get(), &DownloadEngine::recordChanged, this, &DownloadManager::onChanged);
    connect(engine_.get(), &DownloadEngine::recordRemoved, this, &DownloadManager::onRemoved);
    connect(engine_.get(), &DownloadEngine::progressed, this, &DownloadManager::onProgress);
    connect(engine_.get(), &DownloadEngine::noticeRaised, this, [this](qint64, const QString &key) { emit notice(key); });
    progressTimer_.setSingleShot(true);
    progressTimer_.setInterval(config.progressIntervalMs);
    connect(&progressTimer_, &QTimer::timeout, this, &DownloadManager::flushProgress);
    connect(&sidecars_, &SidecarStore::changed, this, [this](const QString &videoId) {
        const DownloadRecord *r = preferred(videoId);
        if (r) emit itemChanged(static_cast<int>(r->id), videoId);
    });
}

DownloadManager::~DownloadManager() {
    engine_.reset(); // stops transfers; their state is recovered on the next open()
}

bool DownloadManager::open(QString *error) {
    if (!store_.open(error)) return false;
    folder_ = store_.metaValue(QLatin1String(folderKey));
    engine_->setUnmeteredOnly(store_.metaValue(QLatin1String(unmeteredKey)) == QLatin1String("1"));
    sidecars_.setWanted(store_.metaValue(QLatin1String(captionsKey), QStringLiteral("1")) == QLatin1String("1"),
                        store_.metaValue(QLatin1String(segmentsKey), QStringLiteral("1")) == QLatin1String("1"));
    engine_->start();
    byVideo_.clear();
    for (const DownloadRecord &r : engine_->records()) byVideo_[qs(r.videoId)].append(r.id);
    recount();
    emit recordsChanged();
    emit folderChanged();
    emit unmeteredOnlyChanged();
    emit sidecarSettingsChanged();
    ensureSidecarsForDone(); // extras that failed or were off last run
    return true;
}

bool DownloadManager::isOpen() const { return store_.isOpen(); }

int DownloadManager::enqueueEntry(const library::VideoEntry &v, Kind kind, int qualityHeight) {
    EnqueueHints h;
    h.videoId = qs(v.videoId);
    h.title = qs(v.title);
    h.channel = qs(v.channel);
    h.channelId = qs(v.channelId);
    h.durationSeconds = v.durationSeconds;
    h.thumbnailUrl = qs(v.thumbnailUrl);
    return static_cast<int>(engine_->enqueue(h, kind, effectiveQualityHeight(qualityHeight, defaultQualityHeight_)));
}

int DownloadManager::enqueue(const QVariantMap &hints, int kind, int qualityHeight) {
    // kind < 0: the default kind of the "Download quality" setting.
    return enqueueEntry(entryFrom(hints), (kind < 0 ? defaultKind_ : kind) == 1 ? Kind::Video : Kind::Audio, qualityHeight);
}

int DownloadManager::enqueueAllEntries(const std::vector<library::VideoEntry> &videos, Kind kind, int qualityHeight) {
    std::vector<std::string> ids;
    for (const library::VideoEntry &v : videos) ids.push_back(v.videoId);
    std::vector<ExistingDownload> existing;
    for (const DownloadRecord &r : engine_->records()) {
        ExistingDownload e;
        e.videoId = r.videoId; e.kind = r.kind; e.state = r.state;
        existing.push_back(e);
    }
    const std::vector<std::string> wanted = idsToEnqueue(ids, existing, kind);
    QSet<QString> pending;
    for (const std::string &id : wanted) pending.insert(qs(id));
    int queued = 0;
    for (const library::VideoEntry &v : videos) {
        if (!pending.remove(qs(v.videoId))) continue;
        if (enqueueEntry(v, kind, qualityHeight) > 0) ++queued;
    }
    return queued;
}

int DownloadManager::enqueueAll(const QVariantList &entries, int kind, int qualityHeight) {
    std::vector<library::VideoEntry> videos;
    for (const QVariant &v : entries) {
        if (v.type() == QVariant::String) {
            library::VideoEntry e;
            e.videoId = v.toString().toStdString();
            videos.push_back(e);
        } else {
            videos.push_back(entryFrom(v.toMap()));
        }
    }
    return enqueueAllEntries(videos, (kind < 0 ? defaultKind_ : kind) == 1 ? Kind::Video : Kind::Audio, qualityHeight);
}

bool DownloadManager::pause(int id) { return engine_->pause(id); }
bool DownloadManager::resume(int id) { return engine_->resume(id); }
bool DownloadManager::retry(int id) { return engine_->retry(id); }
bool DownloadManager::cancel(int id) { return engine_->cancel(id); }
bool DownloadManager::remove(int id, bool deleteFiles) { return engine_->remove(id, deleteFiles); }

const DownloadRecord *DownloadManager::preferred(const QString &videoId) const {
    const QList<qint64> ids = byVideo_.value(videoId);
    const DownloadRecord *best = nullptr;
    for (qint64 id : ids) {
        const DownloadRecord *r = engine_->record(id);
        if (!r) continue;
        if (!best) { best = r; continue; }
        const bool ru = isUnfinished(r->state), bu = isUnfinished(best->state);
        if (ru != bu) { if (ru) best = r; continue; }
        if (r->kind == Kind::Video && best->kind != Kind::Video) best = r;
    }
    return best;
}

int DownloadManager::state(const QString &videoId) const {
    const DownloadRecord *r = preferred(videoId);
    return r ? static_cast<int>(r->state) : -1;
}

qreal DownloadManager::fraction(const QString &videoId) const {
    const DownloadRecord *r = preferred(videoId);
    return r ? progressOf(*r).fraction() : 0;
}

QVariantMap DownloadManager::item(const QString &videoId) const {
    const DownloadRecord *r = preferred(videoId);
    return r ? toMap(*r) : QVariantMap();
}

QVariantMap DownloadManager::record(int id) const {
    const DownloadRecord *r = engine_->record(id);
    return r ? toMap(*r) : QVariantMap();
}

QVariantList DownloadManager::records() const {
    QVariantList out;
    for (const DownloadRecord &r : engine_->records()) out << toMap(r);
    return out;
}

int DownloadManager::count() const { return static_cast<int>(engine_->records().size()); }

QVariantMap DownloadManager::toMap(const DownloadRecord &r) const {
    const Progress p = progressOf(r);
    QVariantMap m;
    m.insert(QStringLiteral("id"), static_cast<int>(r.id));
    m.insert(QStringLiteral("videoId"), qs(r.videoId));
    m.insert(QStringLiteral("kind"), static_cast<int>(r.kind));
    m.insert(QStringLiteral("state"), static_cast<int>(r.state));
    m.insert(QStringLiteral("stateName"), QString::fromLatin1(stateName(r.state)));
    m.insert(QStringLiteral("title"), qs(r.title));
    m.insert(QStringLiteral("channel"), qs(r.channel));
    m.insert(QStringLiteral("channelId"), qs(r.channelId));
    m.insert(QStringLiteral("durationSeconds"), static_cast<qlonglong>(r.durationSeconds));
    m.insert(QStringLiteral("thumbnailUrl"), qs(r.thumbnailUrl));
    m.insert(QStringLiteral("thumbnailPath"), DownloadEngine::thumbnailPath(r));
    m.insert(QStringLiteral("qualityHeight"), r.qualityHeight);
    m.insert(QStringLiteral("layout"), QString::fromLatin1(layoutName(r.layout)));
    m.insert(QStringLiteral("bytesDone"), static_cast<qlonglong>(p.done));
    m.insert(QStringLiteral("bytesTotal"), static_cast<qlonglong>(p.total));
    m.insert(QStringLiteral("fraction"), p.fraction());
    m.insert(QStringLiteral("error"), qs(r.error));
    m.insert(QStringLiteral("notice"), qs(r.notice));
    m.insert(QStringLiteral("directory"), qs(r.directory));
    m.insert(QStringLiteral("filePath"), DownloadEngine::outputPath(r));
    return m;
}

QVariantMap DownloadManager::localMedia(const QString &videoId) {
    QVariantMap m;
    m.insert(QStringLiteral("available"), false);
    m.insert(QStringLiteral("videoId"), videoId); // history and resume of local playback (PLY-26)
    const DownloadRecord *found = nullptr;
    for (qint64 id : byVideo_.value(videoId)) {
        const DownloadRecord *r = engine_->record(id);
        if (!r || r->state != State::Done) continue;
        if (!found || (r->kind == Kind::Video && found->kind != Kind::Video)) found = r;
    }
    if (!found) return m;
    const DownloadRecord r = *found;
    QString file = DownloadEngine::outputPath(r), video, audio;
    if (r.layout == OutputLayout::TrackPair) {
        video = DownloadEngine::trackPath(r, Track::Video);
        audio = DownloadEngine::trackPath(r, Track::Audio);
        file = video;
    } else if (r.kind == Kind::Video) {
        video = file;
    } else {
        audio = file;
    }
    const bool present = r.layout == OutputLayout::TrackPair ? isFile(video) && isFile(audio) : isFile(file);
    m.insert(QStringLiteral("id"), static_cast<int>(r.id));
    m.insert(QStringLiteral("kind"), r.kind == Kind::Video ? QStringLiteral("video") : QStringLiteral("audio"));
    m.insert(QStringLiteral("layout"), QString::fromLatin1(layoutName(r.layout)));
    if (!present) {
        engine_->markFileMissing(r.id);
        m.insert(QStringLiteral("error"), QString::fromLatin1(errorCodes::fileMissing));
        return m;
    }
    const QString thumb = DownloadEngine::thumbnailPath(r);
    m.insert(QStringLiteral("available"), true);
    m.insert(QStringLiteral("filePath"), file);
    m.insert(QStringLiteral("videoPath"), video);
    m.insert(QStringLiteral("audioPath"), audio);
    m.insert(QStringLiteral("thumbnailPath"), isFile(thumb) ? thumb : QString());
    m.insert(QStringLiteral("title"), qs(r.title));
    m.insert(QStringLiteral("channel"), qs(r.channel));
    m.insert(QStringLiteral("channelId"), qs(r.channelId));
    m.insert(QStringLiteral("durationSeconds"), static_cast<qlonglong>(r.durationSeconds));
    m.insert(QStringLiteral("videoCodec"), qs(r.videoCodec));
    m.insert(QStringLiteral("audioCodec"), qs(r.audioCodec));
    m.insert(QStringLiteral("error"), QString());
    const QVariantMap extras = sidecars_.info(videoId);
    m.insert(QStringLiteral("captions"), sidecars_.captionsWanted() ? extras.value(QStringLiteral("captions")) : QVariantList());
    m.insert(QStringLiteral("segments"), sidecars_.segmentsWanted() ? extras.value(QStringLiteral("segments")) : QVariantList());
    m.insert(QStringLiteral("sidecarsPending"), extras.value(QStringLiteral("pending")));
    return m;
}

QVariantMap DownloadManager::chooseFolder(const QString &path) {
    const FolderCheck c = location_.checkChosen(path);
    QVariantMap m;
    m.insert(QStringLiteral("ok"), c.ok);
    m.insert(QStringLiteral("path"), c.path);
    m.insert(QStringLiteral("errorKey"), c.errorKey);
    if (c.ok && c.path != folder_) {
        folder_ = c.path;
        store_.setMetaValue(QLatin1String(folderKey), folder_);
        emit folderChanged();
    }
    return m;
}

void DownloadManager::resetFolder() {
    if (folder_.isEmpty()) return;
    folder_.clear();
    store_.setMetaValue(QLatin1String(folderKey), QString());
    emit folderChanged();
}

QStringList DownloadManager::candidateFolders() const { return location_.candidateFolders(); }

QString DownloadManager::defaultFolder() const { return location_.defaultDirectory(); }

bool DownloadManager::unmeteredOnly() const { return engine_->unmeteredOnly(); }

void DownloadManager::setUnmeteredOnly(bool on) {
    if (on == engine_->unmeteredOnly()) return;
    engine_->setUnmeteredOnly(on);
    store_.setMetaValue(QLatin1String(unmeteredKey), on ? QStringLiteral("1") : QStringLiteral("0"));
    emit unmeteredOnlyChanged();
}

void DownloadManager::setDefaultQualityHeight(int height) {
    height = qMax(0, height);
    if (height == defaultQualityHeight_) return;
    defaultQualityHeight_ = height;
    emit defaultQualityHeightChanged();
}

void DownloadManager::setDownloadDefaults(int qualitySetting, int playbackCeiling) {
    const DownloadDefault d = downloadDefaultFor(qualitySetting, playbackCeiling);
    const int kind = static_cast<int>(d.kind);
    if (kind == defaultKind_ && d.qualityHeight == defaultQualityHeight_) return;
    defaultKind_ = kind;
    defaultQualityHeight_ = d.qualityHeight;
    emit defaultQualityHeightChanged();
}

void DownloadManager::onAdded(qint64 id) {
    const DownloadRecord *r = engine_->record(id);
    if (!r) return;
    const QString vid = qs(r->videoId);
    if (!byVideo_[vid].contains(id)) byVideo_[vid].append(id);
    recount();
    emit recordsChanged();
    emit itemChanged(static_cast<int>(id), vid);
}

void DownloadManager::onChanged(qint64 id) {
    const DownloadRecord *r = engine_->record(id);
    if (!r) return;
    if (r->state == State::Done) sidecars_.ensure(qs(r->videoId));
    recount();
    emit itemChanged(static_cast<int>(id), qs(r->videoId));
}

void DownloadManager::onRemoved(qint64 id, const QString &videoId) {
    QList<qint64> &ids = byVideo_[videoId];
    ids.removeAll(id);
    if (ids.isEmpty()) {
        byVideo_.remove(videoId);
        sidecars_.remove(videoId); // useless without the media
    }
    dirty_.remove(id);
    recount();
    emit recordsChanged();
    emit itemRemoved(static_cast<int>(id), videoId);
    emit itemChanged(static_cast<int>(id), videoId);
}

void DownloadManager::onProgress(qint64 id) {
    dirty_.insert(id);
    if (!progressTimer_.isActive()) progressTimer_.start();
}

void DownloadManager::flushProgress() {
    const QSet<qint64> ids = dirty_;
    dirty_.clear();
    for (qint64 id : ids) emit progressChanged(static_cast<int>(id));
}

void DownloadManager::recount() {
    int active = 0;
    bool keep = false;
    for (const DownloadRecord &r : engine_->records()) {
        if (isUnfinished(r.state) && r.state != State::Paused) ++active;
        if (wantsKeepAlive(r.state)) keep = true;
    }
    if (active != activeCount_) { activeCount_ = active; emit activeCountChanged(); }
    if (keep != keepAliveWanted_) { keepAliveWanted_ = keep; emit keepAliveWantedChanged(); }
}

}}}

namespace nordtile { namespace video { namespace downloads {

void DownloadManager::setSidecarSource(SidecarSource *source) {
    sidecars_.setSource(source);
    if (store_.isOpen()) ensureSidecarsForDone();
}

void DownloadManager::setSaveCaptions(bool on) {
    if (on == sidecars_.captionsWanted()) return;
    sidecars_.setWanted(on, sidecars_.segmentsWanted());
    store_.setMetaValue(QLatin1String(captionsKey), on ? QStringLiteral("1") : QStringLiteral("0"));
    emit sidecarSettingsChanged();
    if (on) ensureSidecarsForDone();
}

void DownloadManager::setSaveSegments(bool on) {
    if (on == sidecars_.segmentsWanted()) return;
    sidecars_.setWanted(sidecars_.captionsWanted(), on);
    store_.setMetaValue(QLatin1String(segmentsKey), on ? QStringLiteral("1") : QStringLiteral("0"));
    emit sidecarSettingsChanged();
    if (on) ensureSidecarsForDone();
}

void DownloadManager::ensureSidecarsForDone() {
    if (!sidecars_.source()) return;
    for (const DownloadRecord &r : engine_->records())
        if (r.state == State::Done) sidecars_.ensure(qs(r.videoId));
}

}}}
