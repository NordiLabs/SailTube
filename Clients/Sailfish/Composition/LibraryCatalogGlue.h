// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#pragma once

// DL-07 offline extras from the catalog component's services, for the download manager.
// The catalog component constructs it from its parts and registers it with
// DownloadManager::setSidecarSource.
//  * CatalogSidecarSource (DL-07): the offline extras of a finished download.
//    Captions: the track list comes from the catalog's CaptionTrackService (the
//    anonymous WEB `player` answer lists no tracks, so the service asks with the
//    pinned shared client and an in-memory visitor id), here over the client it
//    is given. The SEL-06 auto-pick chooses the track for the caption preference
//    and for the device language (at most two tracks) among the tracks the video
//    really has, manual or auto-generated: an auto-translated track is refused
//    (HTTP 429) to anonymous requests, so none is saved. The json3 body is
//    fetched and checked with the catalog's parser. Segments: the SB-01
//    hash-prefix client. Everything runs on the background transport/lane, so
//    it never queues ahead of what the user waits for (RES-02).

#include "Core/Downloads/Infrastructure/SidecarSource.h"
#include <QObject>
#include <QStringList>

namespace nordtile { namespace video {
namespace catalog { class CaptionTrackService; class HttpTransport; class InnerTubeClient; class SponsorBlockService; }
namespace sailfish {

class CaptionTracksModel;

class CatalogSidecarSource final : public QObject, public downloads::SidecarSource {
    Q_OBJECT
public:
    // Any pointer may be null: that extra then answers "none" (final). `client` should be the
    // background client: the track list service made here sends through it.
    CatalogSidecarSource(catalog::InnerTubeClient *client, catalog::HttpTransport *transport,
                         catalog::SponsorBlockService *sponsor, CaptionTracksModel *captionTracks,
                         QObject *parent = nullptr);

    void fetchCaptions(const QString &videoId, CaptionsDone done) override;
    void fetchSegments(const QString &videoId, SegmentsDone done) override;

    // The language codes to keep for offline playback, in order: the caption
    // preference when it is a language, then the device language. Tests.
    static QStringList wantedCodes(const QString &preference, const QString &deviceLocale);
    void setDeviceLocaleForTests(const QString &locale) { deviceLocale_ = locale; }

private:
    catalog::CaptionTrackService *trackList_;   // owned (child); null without a client
    catalog::HttpTransport *transport_;
    catalog::SponsorBlockService *sponsor_;
    CaptionTracksModel *captionTracks_;
    QString deviceLocale_;
};

}}}
