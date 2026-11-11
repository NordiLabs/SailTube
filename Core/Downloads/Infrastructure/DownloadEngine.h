// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#pragma once

// DL-02 download engine. Own QNetworkAccessManager (a background lane, never
// the playback one: RES-02); spawns no process. Per track a "<name>.part" file
// in the target directory, filled by bounded Range requests ("bytes=a-b") with
// the format's own headers and no cookies. Resume over Range from the part size
// after an interruption or a restart; 206 is required for a continuation (a
// 200 is accepted only for offset 0). Young-token tolerance: a 403 at byte 0
// within 10 s of the resolve is retried after a short delay (at most twice).
// A later 403/410 or an expired URL re-resolves the same format id; the new
// size must equal the stored total, else the item fails. A truncated body is
// reopened from the current offset. The final size must equal the total, then
// "<name>.part" is renamed. Video: the two tracks in turn, then the Remuxer
// (MuxedFile, tracks deleted) or a TrackPair. The thumbnail is saved next to
// the file as "<base>.jpg" (DL-07). At most 2 transfers; "unmetered only"
// holds transfers in WaitingForNetwork. Hosts: https + *.googlevideo.com for
// media, https://i.ytimg.com for thumbnails (NFR-SEC-05); tests override.

#include "Core/Downloads/Domain/DownloadPolicy.h"
#include "Core/Downloads/Domain/DownloadTypes.h"
#include "Core/Downloads/Domain/Mp4Layout.h"
#include "Core/Downloads/Infrastructure/DownloadResolver.h"
#include "Core/Downloads/Infrastructure/Remuxer.h"
#include "Core/Media/Domain/StreamSelection.h"
#include <QHash>
#include <QMap>
#include <QObject>
#include <QString>
#include <QTimer>
#include <QUrl>
#include <functional>
#include <vector>

class QNetworkAccessManager;
class QNetworkConfigurationManager;
class QNetworkReply;

namespace nordtile { namespace video { namespace downloads {

class DownloadQueueStore;

struct EngineOptions {
    int maxConcurrent = maxConcurrentTransfers;
    qint64 chunkBytes = 8 * 1024 * 1024;
    int youngTokenDelayMs = 1500;
    int transientDelayMs = 2000;
    int stallTimeoutMs = 30000;
    ResumeLimits limits;
    qint64 thumbnailMaxBytes = 512 * 1024;
    qint64 progressPersistMs = 2000;
    qint64 progressPersistBytes = 4 * 1024 * 1024;
    media::DecoderCapabilities decoders = assumedDecoders();
    // Empty: the defaults described above.
    std::function<bool(const QUrl &)> mediaHostAllowed;
    std::function<bool(const QUrl &)> thumbnailHostAllowed;
    std::function<bool()> unmetered;          // empty: active bearer is WLAN or Ethernet
    std::function<qint64()> clockMs;          // empty: wall clock (epoch ms)
    // Free bytes on the volume of a directory, -1 when unknown. Empty: defaultFreeBytes.
    std::function<qint64(const QString &directory)> freeBytes;
    // DL-06: given the record's directory (empty for a new item), where to
    // write. Required; DownloadManager supplies DownloadLocation::resolve.
    std::function<LocationDecision(const QString &currentDirectory)> locate;

    static media::DecoderCapabilities assumedDecoders(); // H.264 + VP9 available
    static bool defaultMediaHost(const QUrl &);
    static bool defaultThumbnailHost(const QUrl &);
    static qint64 defaultFreeBytes(const QString &directory); // QStorageInfo
};

struct EnqueueHints {
    QString videoId;
    QString title;
    QString channel;
    QString channelId;
    qint64 durationSeconds = -1;
    QString thumbnailUrl;
};

class DownloadEngine final : public QObject {
    Q_OBJECT
public:
    // store, resolver and remuxer (may be null: no muxer) must outlive the engine.
    DownloadEngine(DownloadQueueStore *store, DownloadResolver *resolver, Remuxer *remuxer,
                   const EngineOptions &options, QObject *parent = nullptr);
    // Stops transfers without touching their persisted state: the next start()
    // recovers them (same as a kill).
    ~DownloadEngine() override;

    // Loads the queue, applies restart recovery and schedules.
    void start();

    // Returns the record id; an existing (videoId, kind) record is returned as
    // is (a Failed one is retried). 0 for an invalid id or a store failure.
    qint64 enqueue(const EnqueueHints &hints, Kind kind, int qualityHeight);
    bool pause(qint64 id);
    bool resume(qint64 id);
    bool retry(qint64 id);
    // Unfinished items: stop, delete parts, remove the row.
    bool cancel(qint64 id);
    // Any item; Done/Failed keep their files unless deleteFiles.
    bool remove(qint64 id, bool deleteFiles);
    // A Done item whose files vanished: Failed with errorCodes::fileMissing.
    void markFileMissing(qint64 id);

    void setUnmeteredOnly(bool on);
    bool unmeteredOnly() const { return unmeteredOnly_; }
    // Re-evaluate the unmetered gate (bearer change, or tests).
    void networkChanged();

    const DownloadRecord *record(qint64 id) const;
    std::vector<DownloadRecord> records() const; // by sort key
    int transferCount() const;                   // Resolving + Downloading

    // Absolute paths derived from a record (empty when not derivable).
    static QString trackPath(const DownloadRecord &r, Track track);
    static QString outputPath(const DownloadRecord &r);    // the playable file (Done)
    // DL-01/DL-03: the top-level box layout of a local MP4/M4A file (headers only).
    static Mp4Layout mp4LayoutOf(const QString &path);
    static QString thumbnailPath(const DownloadRecord &r); // empty when none saved

signals:
    void recordAdded(qint64 id);
    void recordChanged(qint64 id);      // state, metadata, error, notice
    void recordRemoved(qint64 id, const QString &videoId);
    void progressed(qint64 id);         // bytes; consumers throttle
    void noticeRaised(qint64 id, const QString &key);

private:
    struct Job;
    void scheduleSoon();
    void runSchedule();
    bool networkAllowed();
    void startJob(qint64 id);
    bool requestResolve(Job *job);
    void onResolved(qint64 id, quint64 resolveId, resolver::ErrorCode code, const ResolvedVideo &video);
    void initialPick(Job *job, const ResolvedVideo &video);
    void reResolved(Job *job, const ResolvedVideo &video);
    void nextTrack(Job *job);
    void beginTrack(Job *job);
    void requestChunk(Job *job);
    bool checkResponse(Job *job, QNetworkReply *reply);
    bool readBody(Job *job, QNetworkReply *reply); // false when the job ended
    void onFinished(Job *job, QNetworkReply *reply);
    void finishTrack(Job *job);
    bool restartTrack(Job *job);
    void tracksComplete(Job *job);
    void remuxFinished(qint64 id, bool ok, const std::string &outputName);
    void flattenFinished(qint64 id, bool ok);
    void complete(Job *job);
    void fail(Job *job, const std::string &code);
    void stopJob(Job *job, bool persistBytes);
    void removeJob(Job *job);
    void fetchThumbnail(qint64 id);
    void setState(DownloadRecord &r, Event e);
    void persist(DownloadRecord &r);
    void persistProgress(Job *job, bool force);
    void deleteFiles(const DownloadRecord &r, bool outputs);
    MuxCapabilities muxCapabilities() const;
    qint64 now() const;
    bool mediaAllowed(const QUrl &) const;
    qint64 freeBytes(const QString &directory) const;
    bool thumbnailAllowed(const QUrl &) const;

    DownloadQueueStore *store_;
    DownloadResolver *resolver_;
    Remuxer *remuxer_;
    EngineOptions options_;
    QNetworkAccessManager *nam_;
    QNetworkConfigurationManager *bearers_ = nullptr;
    QMap<qint64, DownloadRecord> records_;
    QHash<qint64, Job *> jobs_;
    QHash<QNetworkReply *, qint64> thumbnails_;
    QTimer scheduleTimer_;
    bool unmeteredOnly_ = false;
};

}}}
