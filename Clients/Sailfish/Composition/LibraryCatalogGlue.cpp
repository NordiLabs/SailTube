// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "LibraryCatalogGlue.h"

#include "Clients/Sailfish/Presentation/CaptionTracksModel.h"
#include "Core/Catalog/Domain/Captions.h"
#include "Core/Catalog/Infrastructure/CaptionService.h"
#include "Core/Catalog/Infrastructure/CaptionTrackService.h"
#include "Core/Catalog/Infrastructure/HttpTransport.h"
#include "Core/Catalog/Infrastructure/InnerTubeClient.h"
#include "Core/Catalog/Infrastructure/SponsorBlockService.h"
#include <QLocale>
#include <QPointer>
#include <memory>

namespace nordtile { namespace video { namespace sailfish {

namespace {

const int maxOfflineTracks = 2;
const int captionTimeoutMs = 15000;

QString baseCode(const QString &code) {
    return code.section(QLatin1Char('-'), 0, 0).section(QLatin1Char('_'), 0, 0).toLower();
}

// Fetches the chosen tracks one after the other; any failure means "retry later".
struct CaptionRun {
    QList<downloads::CaptionTrackData> pending;
    QList<downloads::CaptionTrackData> saved;
    QStringList urls;
    downloads::SidecarSource::CaptionsDone done;
};

void fetchNext(catalog::HttpTransport *transport, QPointer<QObject> context, std::shared_ptr<CaptionRun> run) {
    if (run->pending.isEmpty()) { run->done(true, run->saved); return; }
    catalog::HttpRequest request;
    request.url = QUrl(run->urls.takeFirst());
    request.timeoutMs = captionTimeoutMs;
    request.headers.append(qMakePair(QByteArray("User-Agent"), catalog::webUserAgent()));
    transport->send(request, context.data(), [transport, context, run](const catalog::HttpReply &reply) {
        downloads::CaptionTrackData track = run->pending.takeFirst();
        std::vector<catalog::CaptionCue> cues;
        const catalog::Outcome outcome = reply.status == 200 ? catalog::CaptionService::parseJson3(reply.body, &cues)
                                                             : catalog::Outcome::Network;
        if (outcome == catalog::Outcome::Ok || outcome == catalog::Outcome::ValidEmpty) {
            track.json3 = reply.body;
            run->saved.append(track);
        } else if (outcome != catalog::Outcome::Unavailable) {
            run->done(false, QList<downloads::CaptionTrackData>());
            return;
        }
        if (context) fetchNext(transport, context, run);
    });
}

} // namespace

CatalogSidecarSource::CatalogSidecarSource(catalog::InnerTubeClient *client, catalog::HttpTransport *transport,
                                           catalog::SponsorBlockService *sponsor, CaptionTracksModel *captionTracks,
                                           QObject *parent)
    : QObject(parent), trackList_(client ? new catalog::CaptionTrackService(client, this) : nullptr),
      transport_(transport), sponsor_(sponsor), captionTracks_(captionTracks),
      deviceLocale_(QLocale::system().name()) {}

QStringList CatalogSidecarSource::wantedCodes(const QString &preference, const QString &deviceLocale) {
    QStringList codes;
    if (!preference.isEmpty() && preference != QLatin1String(catalog::kCaptionsOff)) codes << preference;
    const QString device = QString(deviceLocale).replace(QLatin1Char('_'), QLatin1Char('-'));
    const QString deviceBase = baseCode(device);
    if (!deviceBase.isEmpty() && deviceBase != QLatin1String("c") && deviceBase != QLatin1String("posix")) {
        bool covered = false;
        for (const QString &c : codes) covered = covered || baseCode(c) == deviceBase;
        if (!covered) codes << deviceBase;
    }
    return codes;
}

void CatalogSidecarSource::fetchCaptions(const QString &videoId, CaptionsDone done) {
    if (!trackList_ || !transport_) { done(true, QList<downloads::CaptionTrackData>()); return; }
    const QStringList codes = wantedCodes(captionTracks_ ? captionTracks_->preferred() : QString(), deviceLocale_);
    QPointer<QObject> context(this);
    catalog::HttpTransport *transport = transport_;
    const bool asked = trackList_->fetch(videoId, this, [context, transport, codes, done](const catalog::CaptionTrackList &list) {
        const QList<downloads::CaptionTrackData> none;
        // A playable video without captions: nothing to keep, and no point in asking again.
        if (list.outcome == catalog::Outcome::ValidEmpty) { done(true, none); return; }
        // The list could not be had (network, HTTP, a refused visitor id, an answer that calls the
        // video unplayable): retry later. The video was just downloaded, so "unplayable" here is
        // the provider refusing this client, as it does the anonymous WEB one, not a missing video.
        if (list.outcome != catalog::Outcome::Ok) { done(false, none); return; }
        // Only the tracks the video really has. A translation is made on request and is refused
        // (HTTP 429) to anonymous requests, so picking one would keep the download pending for ever.
        catalog::CaptionTrackSet own;
        own.real = list.tracks.real;
        std::shared_ptr<CaptionRun> run(new CaptionRun);
        run->done = done;
        QStringList keys;
        for (const QString &code : codes) {
            if (run->pending.size() >= maxOfflineTracks) break;
            const catalog::CaptionAutoPick pick = catalog::autoSelectCaption(own, code.toStdString());
            if (pick.match == catalog::CaptionMatch::None) continue;
            const catalog::CaptionTrack *track = own.find(pick.key);
            if (!track || track->translation || track->url.empty() || keys.contains(QString::fromStdString(pick.key))) continue;
            keys << QString::fromStdString(pick.key);
            downloads::CaptionTrackData data;
            data.languageCode = QString::fromStdString(track->code);
            data.name = QString::fromStdString(track->name);
            data.autoGenerated = track->autoGenerated;
            run->pending.append(data);
            run->urls << QString::fromStdString(track->url);
        }
        if (!context) return;
        fetchNext(transport, context, run);
    });
    if (!asked) done(true, QList<downloads::CaptionTrackData>());   // not a video id: nothing to keep
}

void CatalogSidecarSource::fetchSegments(const QString &videoId, SegmentsDone done) {
    if (!sponsor_) { done(true, QList<downloads::SkipSegmentData>()); return; }
    sponsor_->fetch(videoId, this, [done](const catalog::SponsorResult &r) {
        const bool final = r.outcome == catalog::Outcome::Ok || r.outcome == catalog::Outcome::ValidEmpty;
        QList<downloads::SkipSegmentData> out;
        if (!final) { done(false, out); return; }
        for (const catalog::SponsorSegment &s : r.list.segments) {
            downloads::SkipSegmentData d;
            d.category = QString::fromStdString(s.category);
            d.actionType = QString::fromStdString(s.actionType);
            d.startSeconds = s.startSeconds;
            d.endSeconds = s.endSeconds;
            out.append(d);
        }
        if (r.list.hasHighlight) {
            downloads::SkipSegmentData d;
            d.category = QStringLiteral("poi_highlight");
            d.actionType = QStringLiteral("poi");
            d.startSeconds = d.endSeconds = r.list.highlightSeconds;
            out.append(d);
        }
        done(true, out);
    });
}

}}}
