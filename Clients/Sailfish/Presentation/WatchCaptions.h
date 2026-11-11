// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#pragma once

// Fills the caption track list of the playing streamed video (SEL-06): the `captionTracks`
// model behind CaptionsPage and behind playback.captionTracks / captionTrackId / captionText
// (the session reads the same model through its PlaybackCaptionSource).
//
// It follows the watch page's WatchInfo (the context `watchInfo`, bound to playback.videoId):
//  * a new video empties the model, so the previous video's tracks never show on this one;
//  * when the watch data is asked for (WatchInfo::requested: after the hold that keeps discovery
//    out of the way of the first video bytes, or at once for a cached page) the track list is
//    asked for too, through CaptionTrackService. Nothing is asked while the video plays offline;
//  * the answer fills the model: real tracks, then translations, and the stored language is
//    picked automatically. A video without captions leaves the list empty; a failed request
//    leaves it empty with `failed` set, and captionTracks.reload() (or reloading the watch
//    data) asks again. An answer for a video the user has already left is dropped.
//
// A video that plays offline asks for nothing, even when its watch data is still cached; when
// it goes online again its list is asked for then.
// A download's saved tracks (`captionTracks.local`) are never replaced. Only the list is asked
// for here; the cues of a track are fetched by CaptionCues once a track is current, which
// without a stored language means: once the user turns captions on.

#include "Core/Catalog/Infrastructure/CaptionTrackService.h"

#include <QObject>
#include <QPointer>
#include <QString>

namespace nordtile { namespace video { namespace sailfish {

class CaptionTracksModel;
class WatchInfo;

class WatchCaptions final : public QObject {
    Q_OBJECT
public:
    // Nothing is owned; all three must outlive this object or be destroyed with it.
    WatchCaptions(WatchInfo *watch, catalog::CaptionTrackService *service, CaptionTracksModel *tracks,
                  QObject *parent = nullptr);

    // Track lists asked for so far (tests).
    int fetches() const { return fetches_; }

private:
    void videoChanged();
    void load(const QString &videoId, bool again);

    QPointer<WatchInfo> watch_;
    QPointer<catalog::CaptionTrackService> service_;
    QPointer<CaptionTracksModel> tracks_;
    QString listed_;          // the video whose list is in the model
    quint64 generation_ = 0;
    int fetches_ = 0;
    bool wasOffline_ = false;
};

}}}
