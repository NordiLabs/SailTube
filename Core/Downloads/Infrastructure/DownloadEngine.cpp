// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "DownloadEngine.h"
#include "DownloadQueueStore.h"
#include "Core/Catalog/Domain/Video.h"
#include "Core/Downloads/Domain/FileNaming.h"

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QNetworkAccessManager>
#include <QNetworkConfiguration>
#include <QNetworkConfigurationManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QPointer>
#include <QSaveFile>
#include <QStorageInfo>
#include <cerrno>
#include <climits>
#include <cstdio>
#include <memory>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

namespace nordtile { namespace video { namespace downloads {

struct DownloadEngine::Job {
    qint64 id = 0;
    quint64 resolveId = 0;
    bool reResolving = false;
    media::MediaFormat format[2];  // by Track
    qint64 resolvedAtMs = 0;       // of the URLs in format[]
    Track track = Track::Video;
    QNetworkReply *reply = nullptr;
    QFile file;
    QString partPath;              // file's path (opened by descriptor, so QFile has no name)
    QUrl url;                      // current request URL (after redirects)
    qint64 requestOffset = 0;
    qint64 requestEnd = -1;        // inclusive
    qint64 body = 0;               // body bytes written from the current response
    bool checked = false;
    bool accepted = false;
    bool stalled = false;
    std::string fatal;
    int status = 0;
    int youngRetries = 0;
    int reResolves = 0;
    int transientRetries = 0;
    int redirects = 0;
    QTimer *retry = nullptr;
    QTimer *stall = nullptr;
    qint64 persistedBytes = 0;
    qint64 persistedAtMs = 0;
    quint64 remuxId = 0;
    bool remuxing = false;
};

namespace {

QString qs(const std::string &s) { return QString::fromStdString(s); }

int slot(Track t) { return t == Track::Video ? 0 : 1; }

bool hasTrack(const DownloadRecord &r, Track t) {
    if (r.kind == Kind::Audio) return t == Track::Audio && !r.audioFormatId.empty();
    return t == Track::Video ? !r.videoFormatId.empty() : !r.audioFormatId.empty();
}

bool isPair(const DownloadRecord &r) {
    return r.kind == Kind::Video && !r.videoFormatId.empty() && !r.audioFormatId.empty();
}

std::int64_t &total(DownloadRecord &r, Track t) { return t == Track::Video ? r.videoBytesTotal : r.audioBytesTotal; }
std::int64_t &done(DownloadRecord &r, Track t) { return t == Track::Video ? r.videoBytesDone : r.audioBytesDone; }

std::string extensionFor(const DownloadRecord &r, Track t) {
    if (t == Track::Video) return r.videoContainer;
    return r.audioContainer == "webm" ? "webm" : "m4a";
}

std::string trackFile(const DownloadRecord &r, Track t) {
    if (r.baseName.empty()) return std::string();
    if (!isPair(r)) return fileName(r.baseName, extensionFor(r, t));
    return trackFileName(r.baseName, t, extensionFor(r, t));
}

const media::MediaFormat *findFormat(const std::vector<media::MediaFormat> &formats, const std::string &id) {
    for (const media::MediaFormat &f : formats)
        if (f.id == id && !f.url.empty()) return &f;
    return nullptr;
}

qint64 sizeOf(const QString &path) {
    QFileInfo info(path);
    return info.isFile() ? info.size() : 0;
}

// Bytes of a track already on disk (finished file or part).
qint64 onDisk(const DownloadRecord &r, Track t) {
    const QString path = DownloadEngine::trackPath(r, t);
    if (path.isEmpty()) return 0;
    const qint64 final = sizeOf(path);
    return final > 0 ? final : sizeOf(path + QStringLiteral(".part"));
}

bool trackComplete(DownloadRecord &r, Track t) {
    const QString path = DownloadEngine::trackPath(r, t);
    return !path.isEmpty() && total(r, t) > 0 && sizeOf(path) == total(r, t);
}

void noCookies(QNetworkRequest *req) {
    req->setAttribute(QNetworkRequest::CookieLoadControlAttribute, QNetworkRequest::Manual);
    req->setAttribute(QNetworkRequest::CookieSaveControlAttribute, QNetworkRequest::Manual);
    req->setAttribute(QNetworkRequest::CacheLoadControlAttribute, QNetworkRequest::AlwaysNetwork);
    req->setAttribute(QNetworkRequest::CacheSaveControlAttribute, false);
}

bool forbiddenHeader(const QByteArray &name) {
    const QByteArray n = name.toLower();
    return n == "cookie" || n == "authorization" || n == "host" || n == "range" || n == "content-length"
        || n == "connection" || n == "accept-encoding";
}

// Bytes still to transfer over every track of the record.
qint64 remainingBytes(DownloadRecord &r) {
    qint64 left = 0;
    for (Track t : { Track::Video, Track::Audio }) {
        if (hasTrack(r, t) && total(r, t) > 0) left += qMax<qint64>(0, total(r, t) - done(r, t));
    }
    return left;
}

// One more copy of the output that a remux (pair) or flatten (single MP4-family file) writes
// next to the tracks before they are removed.
qint64 outputCopyBytes(DownloadRecord &r) {
    if (isPair(r)) return r.muxExtension.empty() ? 0 : qMax<qint64>(0, total(r, Track::Video)) + qMax<qint64>(0, total(r, Track::Audio));
    const Track t = r.kind == Kind::Audio ? Track::Audio : Track::Video;
    return isMp4Family(extensionFor(r, t)) ? qMax<qint64>(0, total(r, t)) : 0;
}

// NFR-SEC-05 confinement: a symlink planted in the download folder is never written through.
bool isSymLink(const QString &path) {
    struct stat st;
    return ::lstat(QFile::encodeName(path).constData(), &st) == 0 && S_ISLNK(st.st_mode);
}

// Removes a link planted at a path the engine (or the remuxer) is about to write; the link's
// target is never touched.
void removePlantedLink(const QString &path) {
    if (isSymLink(path)) ::unlink(QFile::encodeName(path).constData());
}

// Opens `path` for appending without following a link at the last component.
bool openNoFollow(QFile *file, const QString &path) {
    removePlantedLink(path);
    int fd;
    do {
        fd = ::open(QFile::encodeName(path).constData(), O_WRONLY | O_APPEND | O_CREAT | O_NOFOLLOW | O_CLOEXEC, 0644);
    } while (fd < 0 && errno == EINTR);
    if (fd < 0) return false;
    struct stat st;
    if (::fstat(fd, &st) != 0 || !S_ISREG(st.st_mode)) { ::close(fd); return false; }
    if (!file->open(fd, QIODevice::WriteOnly | QIODevice::Append, QFileDevice::AutoCloseHandle)) {
        ::close(fd);
        return false;
    }
    return true;
}

} // namespace

// ---- options -------------------------------------------------------------------

media::DecoderCapabilities EngineOptions::assumedDecoders() {
    media::DecoderCapabilities c;
    c.h264.available = true;
    c.h264.hardware = true;
    c.vp9.available = true;
    return c;
}

// The same rule as the resolvers' accepted media URLs (https, a googlevideo.com host, the default
// port, no userinfo, the /videoplayback path); it alone guards redirect targets.
bool EngineOptions::defaultMediaHost(const QUrl &url) {
    return url.isValid() && url.scheme() == QLatin1String("https") && url.userInfo().isEmpty()
        && (url.port() == -1 || url.port() == 443)
        && url.host().toLower().endsWith(QLatin1String(".googlevideo.com"))
        && url.path() == QLatin1String("/videoplayback");
}

qint64 EngineOptions::defaultFreeBytes(const QString &directory) {
    const QStorageInfo info(directory);
    return info.isValid() && info.isReady() ? info.bytesAvailable() : -1;
}

bool EngineOptions::defaultThumbnailHost(const QUrl &url) {
    return url.scheme() == QLatin1String("https") && url.host() == QLatin1String("i.ytimg.com");
}

// ---- lifecycle ------------------------------------------------------------------

DownloadEngine::DownloadEngine(DownloadQueueStore *store, DownloadResolver *resolver, Remuxer *remuxer,
                               const EngineOptions &options, QObject *parent)
    : QObject(parent), store_(store), resolver_(resolver), remuxer_(remuxer), options_(options),
      nam_(new QNetworkAccessManager(this)) {
    scheduleTimer_.setSingleShot(true);
    scheduleTimer_.setInterval(0);
    connect(&scheduleTimer_, &QTimer::timeout, this, &DownloadEngine::runSchedule);
}

DownloadEngine::~DownloadEngine() {
    const QList<Job *> jobs = jobs_.values();
    for (Job *j : jobs) stopJob(j, true);
    for (QNetworkReply *reply : thumbnails_.keys()) {
        disconnect(reply, nullptr, this, nullptr);
        reply->abort();
        reply->deleteLater();
    }
    thumbnails_.clear();
}

void DownloadEngine::start() {
    records_.clear();
    for (DownloadRecord &r : store_->loadAll()) {
        const Transition t = transition(r.state, Event::Recover);
        if (t.allowed) {
            r.state = t.next;
            persist(r);
        }
        records_.insert(r.id, r);
    }
    scheduleSoon();
}

qint64 DownloadEngine::now() const {
    return options_.clockMs ? options_.clockMs() : QDateTime::currentMSecsSinceEpoch();
}

qint64 DownloadEngine::freeBytes(const QString &directory) const {
    return options_.freeBytes ? options_.freeBytes(directory) : EngineOptions::defaultFreeBytes(directory);
}

bool DownloadEngine::mediaAllowed(const QUrl &url) const {
    return options_.mediaHostAllowed ? options_.mediaHostAllowed(url) : EngineOptions::defaultMediaHost(url);
}

bool DownloadEngine::thumbnailAllowed(const QUrl &url) const {
    return options_.thumbnailHostAllowed ? options_.thumbnailHostAllowed(url) : EngineOptions::defaultThumbnailHost(url);
}

MuxCapabilities DownloadEngine::muxCapabilities() const {
    MuxCapabilities m;
    if (!remuxer_) return m;
    const MuxerAvailability a = remuxer_->availability();
    m.mp4 = a.loaded && a.mp4;
    m.webm = a.loaded && a.webm;
    return m;
}

// ---- public operations --------------------------------------------------------

qint64 DownloadEngine::enqueue(const EnqueueHints &hints, Kind kind, int qualityHeight) {
    const std::string videoId = hints.videoId.toStdString();
    if (!catalog::validVideoId(videoId) || !store_ || !store_->isOpen()) return 0;
    for (auto it = records_.begin(); it != records_.end(); ++it) {
        if (it->videoId != videoId || it->kind != kind) continue;
        if (it->state == State::Failed) retry(it->id);
        return it->id;
    }
    DownloadRecord r;
    r.videoId = videoId;
    r.kind = kind;
    r.qualityHeight = kind == Kind::Video ? qMax(0, qualityHeight) : 0;
    r.state = State::Queued;
    r.title = hints.title.left(1000).toStdString();
    r.channel = hints.channel.left(500).toStdString();
    r.channelId = hints.channelId.left(64).toStdString();
    r.durationSeconds = hints.durationSeconds;
    r.thumbnailUrl = hints.thumbnailUrl.left(4096).toStdString();
    r.createdMs = r.updatedMs = now();
    r.sortKey = store_->nextSortKey();
    if (!store_->insert(&r)) return 0;
    records_.insert(r.id, r);
    emit recordAdded(r.id);
    scheduleSoon();
    return r.id;
}

bool DownloadEngine::pause(qint64 id) {
    auto it = records_.find(id);
    if (it == records_.end() || !transition(it->state, Event::Pause).allowed) return false;
    if (Job *j = jobs_.value(id)) stopJob(j, true);
    it = records_.find(id);
    setState(*it, Event::Pause);
    persist(*it);
    emit recordChanged(id);
    scheduleSoon();
    return true;
}

bool DownloadEngine::resume(qint64 id) {
    auto it = records_.find(id);
    if (it == records_.end() || !transition(it->state, Event::Resume).allowed) return false;
    setState(*it, Event::Resume);
    persist(*it);
    emit recordChanged(id);
    scheduleSoon();
    return true;
}

bool DownloadEngine::retry(qint64 id) {
    auto it = records_.find(id);
    if (it == records_.end() || !transition(it->state, Event::Retry).allowed) return false;
    setState(*it, Event::Retry);
    it->error.clear();
    it->restarts = 0;
    persist(*it);
    emit recordChanged(id);
    scheduleSoon();
    return true;
}

bool DownloadEngine::cancel(qint64 id) {
    auto it = records_.find(id);
    if (it == records_.end() || it->state == State::Done) return false;
    if (Job *j = jobs_.value(id)) stopJob(j, false);
    const DownloadRecord r = records_.take(id);
    deleteFiles(r, true);
    store_->remove(id);
    emit recordRemoved(id, qs(r.videoId));
    scheduleSoon();
    return true;
}

bool DownloadEngine::remove(qint64 id, bool deleteOutputs) {
    auto it = records_.find(id);
    if (it == records_.end()) return false;
    if (isUnfinished(it->state)) return cancel(id);
    const DownloadRecord r = records_.take(id);
    deleteFiles(r, deleteOutputs);
    store_->remove(id);
    emit recordRemoved(id, qs(r.videoId));
    return true;
}

void DownloadEngine::markFileMissing(qint64 id) {
    auto it = records_.find(id);
    if (it == records_.end() || it->state != State::Done) return;
    // Outside the event table on purpose: only a vanished file turns Done into Failed.
    it->state = State::Failed;
    it->error = errorCodes::fileMissing;
    persist(*it);
    emit recordChanged(id);
}

void DownloadEngine::setUnmeteredOnly(bool on) {
    unmeteredOnly_ = on;
    if (on && !options_.unmetered && !bearers_) {
        bearers_ = new QNetworkConfigurationManager(this);
        connect(bearers_, &QNetworkConfigurationManager::onlineStateChanged, this, &DownloadEngine::networkChanged);
        connect(bearers_, &QNetworkConfigurationManager::configurationChanged, this, &DownloadEngine::networkChanged);
    }
    scheduleSoon();
}

void DownloadEngine::networkChanged() { scheduleSoon(); }

const DownloadRecord *DownloadEngine::record(qint64 id) const {
    auto it = records_.constFind(id);
    return it == records_.constEnd() ? nullptr : &it.value();
}

std::vector<DownloadRecord> DownloadEngine::records() const {
    std::vector<DownloadRecord> out;
    for (const DownloadRecord &r : records_) out.push_back(r);
    std::stable_sort(out.begin(), out.end(), [](const DownloadRecord &a, const DownloadRecord &b) {
        return a.sortKey != b.sortKey ? a.sortKey < b.sortKey : a.id < b.id;
    });
    return out;
}

int DownloadEngine::transferCount() const {
    int n = 0;
    for (const DownloadRecord &r : records_)
        if (isTransfer(r.state)) ++n;
    return n;
}

QString DownloadEngine::trackPath(const DownloadRecord &r, Track t) {
    const std::string name = trackFile(r, t);
    if (name.empty() || r.directory.empty()) return QString();
    return qs(r.directory) + QLatin1Char('/') + qs(name);
}

QString DownloadEngine::outputPath(const DownloadRecord &r) {
    if (r.fileName.empty() || r.directory.empty() || !safeFileName(r.fileName)) return QString();
    return qs(r.directory) + QLatin1Char('/') + qs(r.fileName);
}

QString DownloadEngine::thumbnailPath(const DownloadRecord &r) {
    if (r.thumbnailFile.empty() || r.directory.empty() || !safeFileName(r.thumbnailFile)) return QString();
    return qs(r.directory) + QLatin1Char('/') + qs(r.thumbnailFile);
}

// ---- scheduling -------------------------------------------------------------------

void DownloadEngine::scheduleSoon() {
    if (!scheduleTimer_.isActive()) scheduleTimer_.start();
}

bool DownloadEngine::networkAllowed() {
    if (!unmeteredOnly_) return true;
    if (options_.unmetered) return options_.unmetered();
    if (!bearers_) return false;
    for (const QNetworkConfiguration &c : bearers_->allConfigurations(QNetworkConfiguration::Active)) {
        const QNetworkConfiguration::BearerType b = c.bearerType();
        if (b == QNetworkConfiguration::BearerWLAN || b == QNetworkConfiguration::BearerEthernet) return true;
    }
    return false;
}

void DownloadEngine::runSchedule() {
    std::vector<ScheduleItem> items;
    for (const DownloadRecord &r : records_) {
        ScheduleItem i;
        i.id = r.id; i.state = r.state; i.sortKey = r.sortKey;
        items.push_back(i);
    }
    const ScheduleDecision d = schedule(items, networkAllowed(), options_.maxConcurrent);
    for (qint64 id : d.gate) {
        if (Job *j = jobs_.value(id)) stopJob(j, true);
        auto it = records_.find(id);
        if (it == records_.end()) continue;
        setState(*it, Event::NetworkGated);
        persist(*it);
        emit recordChanged(id);
    }
    for (qint64 id : d.ungate) {
        auto it = records_.find(id);
        if (it == records_.end()) continue;
        setState(*it, Event::NetworkOpen);
        persist(*it);
        emit recordChanged(id);
    }
    for (qint64 id : d.start) startJob(id);
}

void DownloadEngine::setState(DownloadRecord &r, Event e) {
    const Transition t = transition(r.state, e);
    if (t.allowed) r.state = t.next;
}

void DownloadEngine::persist(DownloadRecord &r) {
    r.updatedMs = now();
    r.extras = DownloadQueueStore::writeExtras(r);
    store_->update(r);
}

void DownloadEngine::persistProgress(Job *j, bool force) {
    auto it = records_.find(j->id);
    if (it == records_.end()) return;
    const qint64 bytes = it->videoBytesDone + it->audioBytesDone;
    const qint64 t = now();
    if (!force && !progressPersistDue(bytes - j->persistedBytes, t - j->persistedAtMs, options_.progressPersistMs,
                                      options_.progressPersistBytes))
        return;
    store_->updateProgress(j->id, it->videoBytesDone, it->audioBytesDone, t);
    j->persistedBytes = bytes;
    j->persistedAtMs = t;
}

// ---- job ---------------------------------------------------------------------------

void DownloadEngine::startJob(qint64 id) {
    auto it = records_.find(id);
    if (it == records_.end() || jobs_.contains(id) || !transition(it->state, Event::Start).allowed) return;
    setState(*it, Event::Start);
    Job *j = new Job;
    j->id = id;
    j->retry = new QTimer;
    j->retry->setSingleShot(true);
    j->stall = new QTimer;
    j->stall->setSingleShot(true);
    connect(j->retry, &QTimer::timeout, this, [this, id]() {
        if (Job *job = jobs_.value(id)) requestChunk(job);
    });
    connect(j->stall, &QTimer::timeout, this, [this, id]() {
        Job *job = jobs_.value(id);
        if (!job || !job->reply) return;
        job->stalled = true;
        job->reply->abort();
    });
    j->persistedAtMs = now();
    jobs_.insert(id, j);

    DownloadRecord &r = *it;
    const LocationDecision where = options_.locate ? options_.locate(qs(r.directory)) : LocationDecision();
    if (where.directory.empty() || !QDir().mkpath(qs(where.directory))) {
        fail(j, errorCodes::location);
        return;
    }
    if (!r.directory.empty() && r.directory != where.directory) {
        // The old folder vanished (SD card removed): its parts are out of reach.
        r.videoBytesDone = r.audioBytesDone = 0;
    }
    r.directory = where.directory;
    if (where.fellBack) r.notice = where.noticeKey;
    persist(r);
    emit recordChanged(id);
    if (where.fellBack) emit noticeRaised(id, qs(where.noticeKey));
    requestResolve(j);
}

bool DownloadEngine::requestResolve(Job *j) {
    const qint64 id = j->id;
    auto it = records_.find(id);
    if (!resolver_ || it == records_.end()) {
        fail(j, "resolve_runtime_unavailable");
        return false;
    }
    QPointer<DownloadEngine> self(this);
    std::shared_ptr<quint64> own = std::make_shared<quint64>(0);
    const quint64 rid = resolver_->resolve(qs(it->videoId),
        [self, id, own](resolver::ErrorCode code, const ResolvedVideo &video) {
            if (self) self->onResolved(id, *own, code, video);
        });
    *own = rid;
    if (!rid) {
        fail(j, "resolve_runtime_unavailable");
        return false;
    }
    j->resolveId = rid;
    return true;
}

void DownloadEngine::onResolved(qint64 id, quint64 rid, resolver::ErrorCode code, const ResolvedVideo &video) {
    Job *j = jobs_.value(id);
    if (!j || j->resolveId != rid) return;
    j->resolveId = 0;
    if (code != resolver::ErrorCode::None) {
        fail(j, std::string("resolve_") + resolver::errorLabel(code));
        return;
    }
    DownloadRecord &r = records_[id];
    if (r.title.empty()) r.title = video.title.left(1000).toStdString();
    if (r.channel.empty()) r.channel = video.channel.left(500).toStdString();
    if (r.channelId.empty()) r.channelId = video.channelId.left(64).toStdString();
    if (r.durationSeconds < 0) r.durationSeconds = video.durationSeconds;
    if (r.thumbnailUrl.empty()) r.thumbnailUrl = video.thumbnailUrl.left(4096).toStdString();
    j->resolvedAtMs = video.resolvedAtMs > 0 ? video.resolvedAtMs : now();
    if (j->reResolving) reResolved(j, video);
    else initialPick(j, video);
}

void DownloadEngine::initialPick(Job *j, const ResolvedVideo &video) {
    DownloadRecord &r = records_[j->id];
    const Track tracks[] = { Track::Video, Track::Audio };
    bool known = !r.videoFormatId.empty() || !r.audioFormatId.empty();
    for (Track t : tracks) {
        if (!known || !hasTrack(r, t)) continue;
        const media::MediaFormat *f = findFormat(video.formats, t == Track::Video ? r.videoFormatId : r.audioFormatId);
        if (!f) { known = false; break; }
        j->format[slot(t)] = *f;
    }
    if (known) {
        // Same formats as before a restart: the parts are kept only if the size still matches.
        for (Track t : tracks) {
            if (!hasTrack(r, t)) continue;
            const qint64 newTotal = contentLengthOf(j->format[slot(t)]);
            if (onDisk(r, t) > 0 && resumeAfterReResolve(total(r, t), newTotal) == ResumeAction::Fail) {
                fail(j, errorCodes::sizeMismatch);
                return;
            }
            if (onDisk(r, t) == 0 && newTotal > 0) total(r, t) = newTotal;
        }
    } else {
        deleteFiles(r, false);
        for (Track t : tracks) {
            const QString path = trackPath(r, t);
            if (!path.isEmpty()) QFile::remove(path);
        }
        r.videoFormatId.clear(); r.audioFormatId.clear();
        r.videoContainer.clear(); r.audioContainer.clear();
        r.videoCodec.clear(); r.audioCodec.clear();
        r.videoBytesTotal = r.videoBytesDone = r.audioBytesTotal = r.audioBytesDone = 0;
        r.muxExtension.clear();
        r.layout = OutputLayout::SingleFile;
        if (r.kind == Kind::Audio) {
            const AudioPick p = pickAudio(video.formats);
            if (!p.ok) { fail(j, errorCodes::noStream); return; }
            r.audioFormatId = p.format.id;
            r.audioContainer = p.extension;
            r.audioCodec = p.format.audioCodecTag;
            r.audioBytesTotal = contentLengthOf(p.format);
            j->format[slot(Track::Audio)] = p.format;
        } else {
            const VideoPick p = pickVideo(video.formats, r.qualityHeight, options_.decoders, muxCapabilities());
            if (!p.ok) { fail(j, errorCodes::noStream); return; }
            r.videoFormatId = p.video.id;
            r.videoContainer = p.videoExtension;
            r.videoCodec = p.video.videoCodecTag;
            r.videoBytesTotal = contentLengthOf(p.video);
            j->format[slot(Track::Video)] = p.video;
            if (!p.progressive) {
                r.audioFormatId = p.audio.id;
                r.audioContainer = p.audioExtension;
                r.audioCodec = p.audio.audioCodecTag;
                r.audioBytesTotal = contentLengthOf(p.audio);
                r.layout = p.layout;
                r.muxExtension = p.outputExtension;
                j->format[slot(Track::Audio)] = p.audio;
            }
        }
    }
    if (r.baseName.empty()) r.baseName = baseName(r.title, r.videoId);
    for (Track t : tracks) {
        if (hasTrack(r, t) && trackFile(r, t).empty()) { fail(j, errorCodes::noStream); return; }
    }
    setState(r, Event::Resolved);
    persist(r);
    emit recordChanged(j->id);
    if (r.thumbnailFile.empty()) fetchThumbnail(j->id);
    nextTrack(j);
}

void DownloadEngine::reResolved(Job *j, const ResolvedVideo &video) {
    j->reResolving = false;
    DownloadRecord &r = records_[j->id];
    const Track tracks[] = { Track::Video, Track::Audio };
    for (Track t : tracks) {
        if (!hasTrack(r, t) || (t != j->track && trackComplete(r, t))) continue;
        const media::MediaFormat *f = findFormat(video.formats, t == Track::Video ? r.videoFormatId : r.audioFormatId);
        if (!f) { fail(j, errorCodes::forbidden); return; }
        const qint64 newTotal = contentLengthOf(*f);
        const qint64 have = t == j->track ? done(r, t) : onDisk(r, t);
        if (have > 0 && resumeAfterReResolve(total(r, t), newTotal) == ResumeAction::Fail) {
            fail(j, errorCodes::sizeMismatch);
            return;
        }
        if (total(r, t) <= 0 || (have == 0 && newTotal > 0)) total(r, t) = newTotal;
        j->format[slot(t)] = *f;
    }
    j->url = QUrl(qs(j->format[slot(j->track)].url));
    j->youngRetries = 0;
    j->redirects = 0;
    persist(r);
    requestChunk(j);
}

void DownloadEngine::nextTrack(Job *j) {
    DownloadRecord &r = records_[j->id];
    const Track tracks[] = { Track::Video, Track::Audio };
    for (Track t : tracks) {
        if (!hasTrack(r, t)) continue;
        if (trackComplete(r, t)) { done(r, t) = total(r, t); continue; }
        j->track = t;
        beginTrack(j);
        return;
    }
    tracksComplete(j);
}

void DownloadEngine::beginTrack(Job *j) {
    DownloadRecord &r = records_[j->id];
    const Track t = j->track;
    const QString final = trackPath(r, t);
    if (QFileInfo(final).exists()) QFile::remove(final); // a wrong-size leftover
    j->youngRetries = j->reResolves = j->transientRetries = j->redirects = 0;
    j->url = QUrl(qs(j->format[slot(t)].url));
    j->file.close();
    j->partPath = final + QStringLiteral(".part");
    if (!openNoFollow(&j->file, j->partPath)) {
        fail(j, errorCodes::write);
        return;
    }
    switch (resumeAtStart(j->file.size(), total(r, t))) {
    case ResumeAction::Complete:
        done(r, t) = j->file.size();
        finishTrack(j);
        return;
    case ResumeAction::RestartFromZero:
        if (!restartTrack(j)) return;
        break;
    default:
        break;
    }
    done(r, t) = j->file.size();
    if (!enoughSpace(freeBytes(qs(r.directory)), spaceNeeded(remainingBytes(r), outputCopyBytes(r)))) {
        fail(j, errorCodes::noSpace);
        return;
    }
    j->persistedBytes = r.videoBytesDone + r.audioBytesDone;
    emit progressed(j->id);
    requestChunk(j);
}

bool DownloadEngine::restartTrack(Job *j) {
    DownloadRecord &r = records_[j->id];
    if (++r.restarts > 2) {
        fail(j, errorCodes::sizeMismatch);
        return false;
    }
    j->file.resize(0);
    done(r, j->track) = 0;
    persist(r);
    return true;
}

void DownloadEngine::requestChunk(Job *j) {
    DownloadRecord &r = records_[j->id];
    const Track t = j->track;
    const qint64 offset = done(r, t);
    const qint64 tot = total(r, t);
    if (tot > 0 && offset >= tot) {
        finishTrack(j);
        return;
    }
    if (urlExpired(j->format[slot(t)].url, now() / 1000)) {
        if (j->reResolves >= options_.limits.reResolves) { fail(j, errorCodes::forbidden); return; }
        ++j->reResolves;
        j->reResolving = true;
        requestResolve(j);
        return;
    }
    if (!j->url.isValid() || !mediaAllowed(j->url)) {
        fail(j, errorCodes::hostRefused);
        return;
    }
    QNetworkRequest req(j->url);
    for (const auto &h : j->format[slot(t)].headers) {
        const QByteArray name = QByteArray::fromStdString(h.first);
        if (!forbiddenHeader(name)) req.setRawHeader(name, QByteArray::fromStdString(h.second));
    }
    req.setRawHeader("Range", QByteArray::fromStdString(rangeHeader(offset, tot, options_.chunkBytes)));
    req.setRawHeader("Accept-Encoding", "identity");
    noCookies(&req);
    j->requestOffset = offset;
    j->requestEnd = rangeEnd(offset, tot, options_.chunkBytes);
    j->body = 0;
    j->checked = j->accepted = j->stalled = false;
    j->fatal.clear();
    j->status = 0;
    QNetworkReply *reply = nam_->get(req);
    j->reply = reply;
    const qint64 id = j->id;
    connect(reply, &QNetworkReply::readyRead, this, [this, id, reply]() {
        Job *job = jobs_.value(id);
        if (job && job->reply == reply) readBody(job, reply);
    });
    connect(reply, &QNetworkReply::finished, this, [this, id, reply]() {
        Job *job = jobs_.value(id);
        if (job && job->reply == reply) onFinished(job, reply);
        else reply->deleteLater();
    });
    j->stall->start(options_.stallTimeoutMs);
}

bool DownloadEngine::checkResponse(Job *j, QNetworkReply *reply) {
    if (j->checked) return j->accepted;
    j->checked = true;
    j->status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    DownloadRecord &r = records_[j->id];
    std::int64_t &tot = total(r, j->track);
    if ((j->status == 206 || j->status == 200)
            && !mediaContentTypeAllowed(reply->rawHeader("Content-Type").toStdString())) {
        j->fatal = errorCodes::notMedia;   // an error page, not media, whatever its size
        return false;
    }
    if (j->status == 206) {
        std::int64_t first = 0, last = 0, rangeTotal = 0;
        if (!parseContentRange(reply->rawHeader("Content-Range").toStdString(), &first, &last, &rangeTotal)
            || first != j->requestOffset) {
            j->fatal = errorCodes::http;
            return false;
        }
        if (tot > 0 && rangeTotal > 0 && rangeTotal != tot) { j->fatal = errorCodes::sizeMismatch; return false; }
        if (tot <= 0) {
            if (rangeTotal <= 0) { j->fatal = errorCodes::http; return false; }
            tot = rangeTotal;
        }
        j->requestEnd = last;
        j->accepted = true;
    } else if (j->status == 200 && j->requestOffset == 0) {
        const qint64 length = reply->header(QNetworkRequest::ContentLengthHeader).toLongLong();
        if (tot > 0 && length > 0 && length != tot) { j->fatal = errorCodes::sizeMismatch; return false; }
        if (tot <= 0) {
            if (length <= 0) { j->fatal = errorCodes::http; return false; }
            tot = length;
        }
        j->requestEnd = tot - 1;
        j->accepted = true;
    }
    return j->accepted;
}

bool DownloadEngine::readBody(Job *j, QNetworkReply *reply) {
    if (!checkResponse(j, reply)) {
        // Refused or unusable: end it now, onFinished decides.
        if (reply->isRunning()) reply->abort();
        return false;
    }
    j->stall->start(options_.stallTimeoutMs);
    DownloadRecord &r = records_[j->id];
    std::int64_t &d = done(r, j->track);
    QByteArray data = reply->readAll();
    const qint64 room = j->requestEnd + 1 - d;
    if (data.size() > room) data.truncate(static_cast<int>(qMax<qint64>(0, room)));
    if (data.isEmpty()) return true;
    if (j->file.write(data) != data.size()) {
        fail(j, errorCodes::write);
        return false;
    }
    d += data.size();
    j->body += data.size();
    j->transientRetries = 0;
    persistProgress(j, false);
    emit progressed(j->id);
    return true;
}

void DownloadEngine::onFinished(Job *j, QNetworkReply *reply) {
    j->stall->stop();
    checkResponse(j, reply);
    if (j->accepted && !readBody(j, reply)) return; // write failure: the job has ended
    j->reply = nullptr;
    reply->deleteLater();
    const int status = j->status;
    if (status >= 300 && status < 400 && status != 304) {
        const QUrl target = j->url.resolved(QUrl::fromEncoded(reply->rawHeader("Location")));
        if (!target.isValid() || !mediaAllowed(target) || j->redirects >= 3) { fail(j, errorCodes::hostRefused); return; }
        ++j->redirects;
        j->url = target;
        requestChunk(j);
        return;
    }
    if (!j->fatal.empty()) { fail(j, j->fatal); return; }
    DownloadRecord &r = records_[j->id];
    const Track t = j->track;
    if (j->accepted) {
        j->file.flush();
        persistProgress(j, false);
        if (total(r, t) > 0 && done(r, t) >= total(r, t)) { finishTrack(j); return; }
        const bool truncated = j->stalled || reply->error() != QNetworkReply::NoError
            || j->body < j->requestEnd - j->requestOffset + 1;
        if (!truncated) {
            j->redirects = 0;
            requestChunk(j);
            return;
        }
    }
    ResponseFacts f;
    f.httpStatus = j->accepted ? 206 : (j->stalled ? 0 : status);
    f.requestOffset = j->requestOffset;
    f.nowMs = now();
    f.resolvedAtMs = j->resolvedAtMs;
    f.urlExpired = urlExpired(j->format[slot(t)].url, f.nowMs / 1000);
    f.truncated = j->accepted;
    f.youngRetries = j->youngRetries;
    f.reResolves = j->reResolves;
    f.transientRetries = j->transientRetries;
    switch (resumeAfterResponse(f, options_.limits)) {
    case ResumeAction::RetryAfterYoungTokenDelay:
        ++j->youngRetries;
        j->retry->start(options_.youngTokenDelayMs);
        return;
    case ResumeAction::ReResolveSameFormat:
        ++j->reResolves;
        j->reResolving = true;
        requestResolve(j);
        return;
    case ResumeAction::RestartFromZero:
        if (restartTrack(j)) j->retry->start(0);
        return;
    case ResumeAction::ContinueFromOffset:
        ++j->transientRetries;
        // A cut-off body that brought bytes is reopened at once; silence waits.
        j->retry->start(j->accepted && j->body > 0 ? 0 : options_.transientDelayMs);
        return;
    case ResumeAction::Complete:
        finishTrack(j);
        return;
    case ResumeAction::Fail:
        break;
    }
    const int s = f.httpStatus;
    fail(j, s == 403 || s == 410 ? errorCodes::forbidden : s == 0 || j->accepted ? errorCodes::network : errorCodes::http);
}

void DownloadEngine::finishTrack(Job *j) {
    DownloadRecord &r = records_[j->id];
    const Track t = j->track;
    j->file.flush();
    const qint64 size = j->file.size();
    const QString part = j->partPath;
    j->file.close();
    if (isSymLink(part)) {   // swapped for a link while open: never rename a link into place
        removePlantedLink(part);
        fail(j, errorCodes::write);
        return;
    }
    if (total(r, t) <= 0 || size != total(r, t)) {
        fail(j, errorCodes::sizeMismatch);
        return;
    }
    const QString final = trackPath(r, t);
    QFile::remove(final);
    if (!QFile::rename(part, final)) {
        fail(j, errorCodes::write);
        return;
    }
    done(r, t) = total(r, t);
    persist(r);
    emit progressed(j->id);
    nextTrack(j);
}

void DownloadEngine::tracksComplete(Job *j) {
    DownloadRecord &r = records_[j->id];
    if (!isPair(r)) {
        const Track t = r.kind == Kind::Audio ? Track::Audio : Track::Video;
        r.fileName = trackFile(r, t);
        r.layout = OutputLayout::SingleFile;
        // DL-01/DL-03: a DASH track (ftyp dash, sidx, moof/mdat) is rewritten as a
        // flat file (moov + one mdat); the device player cannot read fragments from disk.
        const QString path = trackPath(r, t);
        if (isMp4Family(extensionFor(r, t)) && isFragmentedMp4(mp4LayoutOf(path))) {
            if (remuxer_ && muxCapabilities().mp4) {
                setState(r, Event::TracksDone);
                persist(r);
                emit recordChanged(j->id);
                const qint64 id = j->id;
                QPointer<DownloadEngine> self(this);
                j->remuxing = true;
                removePlantedLink(path + QStringLiteral(".flat"));
                removePlantedLink(path + QStringLiteral(".flat.part"));
                removePlantedLink(path + QStringLiteral(".flat.faststart.part"));
                j->remuxId = remuxer_->remux(t == Track::Video ? path : QString(), t == Track::Audio ? path : QString(),
                                             path + QStringLiteral(".flat"), MuxContainer::Mp4,
                                             [self, id](bool ok, const QString &) { if (self) self->flattenFinished(id, ok); });
                return;
            }
            r.notice = noticeKeys::remuxFailed; // kept as downloaded; the system player may still read it
            emit noticeRaised(j->id, qs(r.notice));
        }
        complete(j);
        return;
    }
    const MuxTarget target = r.muxExtension == "mp4" ? MuxTarget::Mp4 : r.muxExtension == "webm" ? MuxTarget::WebM : MuxTarget::None;
    const std::string out = fileName(r.baseName, r.muxExtension);
    if (remuxer_ && !out.empty() && outputLayoutFor(target, muxCapabilities()) == OutputLayout::MuxedFile) {
        setState(r, Event::TracksDone);
        persist(r);
        emit recordChanged(j->id);
        const qint64 id = j->id;
        QPointer<DownloadEngine> self(this);
        j->remuxing = true;
        const QString outPath = qs(r.directory) + QLatin1Char('/') + qs(out);
        removePlantedLink(outPath);
        removePlantedLink(outPath + QStringLiteral(".part"));
        removePlantedLink(outPath + QStringLiteral(".faststart.part"));
        j->remuxId = remuxer_->remux(trackPath(r, Track::Video), trackPath(r, Track::Audio), outPath,
                                     target == MuxTarget::Mp4 ? MuxContainer::Mp4 : MuxContainer::WebM,
                                     [self, id, out](bool ok, const QString &) {
                                         if (self) self->remuxFinished(id, ok, out);
                                     });
        return;
    }
    r.layout = OutputLayout::TrackPair;
    r.fileName = trackFile(r, Track::Video);
    complete(j);
}

void DownloadEngine::remuxFinished(qint64 id, bool ok, const std::string &out) {
    Job *j = jobs_.value(id);
    if (!j || !j->remuxing) return;
    j->remuxing = false;
    j->remuxId = 0;
    DownloadRecord &r = records_[id];
    const QString outPath = qs(r.directory) + QLatin1Char('/') + qs(out);
    if (ok && sizeOf(outPath) > 0) {
        const QString video = trackPath(r, Track::Video), audio = trackPath(r, Track::Audio);
        r.fileName = out;
        r.layout = OutputLayout::MuxedFile;
        complete(j);
        // Only after Done is persisted: a crash in between re-runs the remux instead of losing the tracks.
        QFile::remove(video);
        QFile::remove(audio);
        return;
    }
    QFile::remove(outPath + QStringLiteral(".part"));
    QFile::remove(outPath);
    r.layout = OutputLayout::TrackPair;
    r.fileName = trackFile(r, Track::Video);
    r.notice = noticeKeys::remuxFailed;
    emit noticeRaised(id, qs(r.notice));
    complete(j);
}

void DownloadEngine::flattenFinished(qint64 id, bool ok) {
    Job *j = jobs_.value(id);
    if (!j || !j->remuxing) return;
    j->remuxing = false;
    j->remuxId = 0;
    DownloadRecord &r = records_[id];
    const QString path = trackPath(r, r.kind == Kind::Audio ? Track::Audio : Track::Video);
    const QString flat = path + QStringLiteral(".flat");
    // POSIX rename replaces the fragmented file atomically; a crash before it
    // leaves the original, and the flatten runs again on the next attempt.
    if (ok && isFlatMp4(mp4LayoutOf(flat))
            && std::rename(QFile::encodeName(flat).constData(), QFile::encodeName(path).constData()) == 0) {
        complete(j);
        return;
    }
    QFile::remove(flat + QStringLiteral(".part"));
    QFile::remove(flat);
    r.notice = noticeKeys::remuxFailed;
    emit noticeRaised(id, qs(r.notice));
    complete(j);
}

Mp4Layout DownloadEngine::mp4LayoutOf(const QString &path) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) return Mp4Layout();
    const Mp4Reader read = [&file](std::uint64_t offset, char *buffer, std::size_t length) -> std::size_t {
        if (!file.seek(static_cast<qint64>(offset))) return 0;
        const qint64 n = file.read(buffer, static_cast<qint64>(length));
        return n > 0 ? static_cast<std::size_t>(n) : 0;
    };
    return scanMp4(read, static_cast<std::uint64_t>(file.size()));
}

void DownloadEngine::complete(Job *j) {
    const qint64 id = j->id;
    DownloadRecord &r = records_[id];
    setState(r, Event::Finished);
    r.error.clear();
    removeJob(j);
    persist(r);
    emit progressed(id);
    emit recordChanged(id);
    scheduleSoon();
}

void DownloadEngine::fail(Job *j, const std::string &code) {
    const qint64 id = j->id;
    stopJob(j, true);
    auto it = records_.find(id);
    if (it == records_.end()) return;
    it->state = State::Failed;
    it->error = code;
    persist(*it);
    emit recordChanged(id);
    scheduleSoon();
}

void DownloadEngine::stopJob(Job *j, bool persistBytes) {
    if (j->reply) {
        QNetworkReply *reply = j->reply;
        j->reply = nullptr;
        disconnect(reply, nullptr, this, nullptr);
        reply->abort();
        reply->deleteLater();
    }
    if (j->resolveId && resolver_) resolver_->cancel(j->resolveId);
    j->resolveId = 0;
    if (j->remuxId && remuxer_) remuxer_->cancel(j->remuxId);
    j->remuxId = 0;
    j->remuxing = false;
    j->file.flush();
    if (persistBytes) persistProgress(j, true);
    removeJob(j);
}

void DownloadEngine::removeJob(Job *j) {
    jobs_.remove(j->id);
    j->file.close();
    j->retry->stop();
    j->stall->stop();
    j->retry->deleteLater();
    j->stall->deleteLater();
    delete j;
}

void DownloadEngine::deleteFiles(const DownloadRecord &r, bool outputs) {
    const Track tracks[] = { Track::Video, Track::Audio };
    for (Track t : tracks) {
        const QString path = trackPath(r, t);
        if (path.isEmpty()) continue;
        QFile::remove(path + QStringLiteral(".part"));
        if (outputs) QFile::remove(path);
    }
    if (!r.baseName.empty() && !r.muxExtension.empty() && !r.directory.empty()) {
        const std::string muxed = fileName(r.baseName, r.muxExtension);
        if (!muxed.empty()) {
            const QString path = qs(r.directory) + QLatin1Char('/') + qs(muxed);
            QFile::remove(path + QStringLiteral(".part"));
            if (outputs) QFile::remove(path);
        }
    }
    if (outputs) {
        const QString out = outputPath(r);
        if (!out.isEmpty()) QFile::remove(out);
        const QString thumb = thumbnailPath(r);
        if (!thumb.isEmpty()) QFile::remove(thumb);
    }
}

// ---- DL-07 thumbnail -------------------------------------------------------------

void DownloadEngine::fetchThumbnail(qint64 id) {
    auto it = records_.find(id);
    if (it == records_.end()) return;
    for (QNetworkReply *pending : thumbnails_.keys())
        if (thumbnails_.value(pending) == id) return;
    QUrl url(qs(it->thumbnailUrl));
    if (!thumbnailAllowed(url)) {
        url = QUrl(QStringLiteral("https://i.ytimg.com/vi/") + qs(it->videoId) + QStringLiteral("/mqdefault.jpg"));
        if (!thumbnailAllowed(url)) return;
    }
    QNetworkRequest req(url);
    noCookies(&req);
    QNetworkReply *reply = nam_->get(req);
    thumbnails_.insert(reply, id);
    const qint64 limit = options_.thumbnailMaxBytes;
    connect(reply, &QNetworkReply::downloadProgress, this, [reply, limit](qint64 received, qint64) {
        if (received > limit) reply->abort();
    });
    connect(reply, &QNetworkReply::finished, this, [this, reply, limit]() {
        const qint64 rid = thumbnails_.take(reply);
        reply->deleteLater();
        auto r = records_.find(rid);
        if (r == records_.end() || r->baseName.empty() || r->directory.empty()) return;
        if (reply->error() != QNetworkReply::NoError
            || reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt() != 200)
            return;
        const QByteArray data = reply->readAll();
        if (data.size() < 4 || data.size() > limit || !data.startsWith("\xFF\xD8\xFF")) return; // JPEG only
        const std::string name = thumbnailFileName(r->baseName);
        if (!safeFileName(name)) return;
        QSaveFile file(qs(r->directory) + QLatin1Char('/') + qs(name));
        if (!file.open(QIODevice::WriteOnly) || file.write(data) != data.size() || !file.commit()) return;
        r->thumbnailFile = name;
        persist(*r);
        emit recordChanged(rid);
    });
}

}}}
