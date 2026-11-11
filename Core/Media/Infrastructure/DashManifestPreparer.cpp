// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "DashManifestPreparer.h"
#include "../Domain/DashManifest.h"

#include <QDir>
#include <QFile>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QPointer>
#include <QSaveFile>
#include <QTimer>
#include <QUuid>

namespace nordtile { namespace video { namespace media {

namespace {
const qint64 firstRead = 64 * 1024;
const qint64 maximumRead = 1024 * 1024;
const int partTimeoutMs = 12000;
}

struct DashManifestPreparer::Part {
    MediaFormat format;
    QNetworkReply *reply = nullptr;
    QTimer *timer = nullptr;
    QByteArray data;          // bytes of the current window
    qint64 window = 0;        // bytes requested
    bool windowFull = false;  // stopped reading because the window was filled
    Mp4Index index;
    bool done = false;
};

struct DashManifestPreparer::Job {
    quint64 id = 0;
    StreamCandidate candidate;
    double duration = 0;
    Completion completion;
    Part video, audio;
    bool finished = false;
};

DashManifestPreparer::DashManifestPreparer(QNetworkAccessManager *network, const QString &directory, QObject *parent)
    : QObject(parent), network_(network), directory_(directory) {}

DashManifestPreparer::~DashManifestPreparer() {
    for (Job *job : jobs_) {
        for (Part *part : { &job->video, &job->audio })
            if (part->reply) { part->reply->disconnect(this); part->reply->abort(); part->reply->deleteLater(); }
        delete job;
    }
    clear();
}

void DashManifestPreparer::clear() {
    QDir dir(directory_);
    for (const QString &name : dir.entryList(QStringList() << QStringLiteral("*.mpd"), QDir::Files))
        dir.remove(name);
}

quint64 DashManifestPreparer::prepare(const StreamCandidate &adaptive, double durationSeconds, Completion completion) {
    Job *job = new Job;
    job->id = nextId_++;
    job->candidate = adaptive;
    job->duration = durationSeconds;
    job->completion = std::move(completion);
    job->video.format = adaptive.video;
    job->audio.format = adaptive.audio;
    jobs_.insert(job->id, job);
    if (adaptive.kind != StreamKind::Adaptive || adaptive.video.url.empty() || adaptive.audio.url.empty()) {
        const quint64 id = job->id;
        QTimer::singleShot(0, this, [this, id]() { finish(id, false, QUrl()); });
        return id;
    }
    request(job, &job->video, firstRead);
    request(job, &job->audio, firstRead);
    return job->id;
}

void DashManifestPreparer::cancel(quint64 id) {
    Job *job = jobs_.value(id, nullptr);
    if (!job || job->finished) return;
    for (Part *part : { &job->video, &job->audio }) {
        if (part->reply) { part->reply->disconnect(this); part->reply->abort(); part->reply->deleteLater(); part->reply = nullptr; }
        if (part->timer) part->timer->deleteLater();
    }
    job->finished = true;
    jobs_.remove(id);
    delete job;
}

void DashManifestPreparer::request(Job *job, Part *part, qint64 bytes) {
    QNetworkRequest r(QUrl(QString::fromStdString(part->format.url)));
    for (const auto &header : part->format.headers)
        r.setRawHeader(QByteArray::fromStdString(header.first), QByteArray::fromStdString(header.second));
    r.setRawHeader("Range", "bytes=0-" + QByteArray::number(bytes - 1));
    r.setAttribute(QNetworkRequest::FollowRedirectsAttribute, true);
    part->reply = network_->get(r);
    if (!part->timer) {
        part->timer = new QTimer(this);
        part->timer->setSingleShot(true);
    }
    const quint64 id = job->id;
    QNetworkReply *reply = part->reply;
    // The reply is the connection context: a superseded reply drops its timeout hook.
    connect(part->timer, &QTimer::timeout, reply, [reply]() { reply->abort(); });
    part->timer->start(partTimeoutMs);
    part->data.clear();
    part->window = bytes;
    part->windowFull = false;
    // Never read past the window, even if a server ignores Range and answers 200
    // with the whole file: keep what fits and stop the transfer.
    connect(reply, &QNetworkReply::readyRead, this, [part, reply]() {
        if (part->reply != reply || part->windowFull) return;
        part->data += reply->read(part->window - part->data.size());
        if (part->data.size() >= part->window) {
            part->windowFull = true;
            reply->abort();
        }
    });
    connect(reply, &QNetworkReply::finished, this, [this, id, part, reply]() {
        Job *current = jobs_.value(id, nullptr);
        if (!current || part->reply != reply) { reply->deleteLater(); return; }
        partDone(current, part);
    });
}

void DashManifestPreparer::partDone(Job *job, Part *part) {
    QNetworkReply *reply = part->reply;
    part->reply = nullptr;
    if (part->timer) part->timer->stop();
    const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    if (!part->windowFull) part->data += reply->read(part->window - part->data.size());
    const bool transferOk = reply->error() == QNetworkReply::NoError || part->windowFull;
    reply->deleteLater();
    if ((status != 206 && status != 200) || !transferOk) { finish(job->id, false, QUrl()); return; }
    const QByteArray data = part->data;
    part->index = parseMp4Index(reinterpret_cast<const unsigned char *>(data.constData()), static_cast<std::size_t>(data.size()));
    // Ask for a larger window only when this one came back full: a short answer
    // means the file (or the server) has no more to give.
    if (part->index.status == Mp4IndexStatus::NeedMoreData && part->index.bytesNeeded <= maximumRead
            && data.size() >= part->window && part->window < maximumRead) {
        request(job, part, qMin(maximumRead, part->index.bytesNeeded + firstRead));
        return;
    }
    if (part->index.status != Mp4IndexStatus::Ok) { finish(job->id, false, QUrl()); return; }
    part->done = true;
    if (!job->video.done || !job->audio.done) return;

    const auto representation = [](const MediaFormat &f, const Mp4Index &index, bool video) {
        DashRepresentation r;
        r.id = f.id;
        r.mimeType = video ? "video/mp4" : "audio/mp4";
        r.codecs = video ? f.videoCodecTag : f.audioCodecTag;
        r.url = f.url;
        r.bandwidthBps = static_cast<int>((f.totalKbps > 0 ? f.totalKbps : f.audioKbps) * 1000);
        r.width = f.width; r.height = f.height; r.frameRate = f.fps;
        r.audioSampleRate = f.audioSampleRate;
        r.initializationEnd = index.initializationEnd;
        r.indexStart = index.indexStart; r.indexEnd = index.indexEnd;
        return r;
    };
    const std::string mpd = buildOnDemandMpd(job->duration,
        representation(job->video.format, job->video.index, true),
        representation(job->audio.format, job->audio.index, false));
    clear();
    QDir().mkpath(directory_);
    const QString path = directory_ + QLatin1Char('/') + QUuid::createUuid().toString().mid(1, 36) + QStringLiteral(".mpd");
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly)) { finish(job->id, false, QUrl()); return; }
    file.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner);
    file.write(mpd.data(), static_cast<qint64>(mpd.size()));
    finish(job->id, file.commit(), QUrl::fromLocalFile(path));
}

void DashManifestPreparer::finish(quint64 id, bool ok, const QUrl &url) {
    Job *job = jobs_.value(id, nullptr);
    if (!job || job->finished) return;
    job->finished = true;
    for (Part *part : { &job->video, &job->audio }) {
        if (part->reply) { part->reply->disconnect(this); part->reply->abort(); part->reply->deleteLater(); part->reply = nullptr; }
        if (part->timer) part->timer->deleteLater();
    }
    Completion completion = std::move(job->completion);
    jobs_.remove(id);
    delete job;
    if (completion) completion(ok, ok ? url : QUrl());
}

}}}
