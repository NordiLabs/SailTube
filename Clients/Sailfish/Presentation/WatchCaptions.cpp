// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "WatchCaptions.h"
#include "CaptionTracksModel.h"
#include "WatchInfo.h"

namespace nordtile { namespace video { namespace sailfish {

WatchCaptions::WatchCaptions(WatchInfo *watch, catalog::CaptionTrackService *service, CaptionTracksModel *tracks,
                             QObject *parent)
    : QObject(parent), watch_(watch), service_(service), tracks_(tracks) {
    if (!watch || !service || !tracks) return;
    connect(watch, &WatchInfo::videoIdChanged, this, [this] { videoChanged(); });
    connect(watch, &WatchInfo::requested, this, [this](const QString &videoId) { load(videoId, false); });
    connect(tracks, &CaptionTracksModel::reloadRequested, this, [this] {
        if (watch_) load(watch_->videoId(), true);
    });
    // The same video goes from offline to streamed (a download that is played from the network
    // instead): its list is asked for now, unless the watch data is still held back, in which
    // case requested() follows.
    wasOffline_ = watch->offline();
    connect(watch, &WatchInfo::holdChanged, this, [this] {
        if (!watch_) return;
        const bool cameOnline = wasOffline_ && !watch_->offline();
        wasOffline_ = watch_->offline();
        if (cameOnline && !watch_->deferred()) load(watch_->videoId(), false);
    });
}

void WatchCaptions::videoChanged() {
    if (!watch_ || !tracks_) return;
    ++generation_;   // an answer on its way belongs to the video just left
    listed_.clear();
    // A download's saved tracks may already be this video's (the session sets them when it opens).
    if (tracks_->videoId() == watch_->videoId()) return;
    tracks_->clear();
}

void WatchCaptions::load(const QString &videoId, bool again) {
    if (!watch_ || !tracks_ || !service_) return;
    if (videoId.isEmpty() || videoId != watch_->videoId() || watch_->offline()) return;
    if (tracks_->videoId() == videoId) {
        if (tracks_->local() || tracks_->loading()) return;
        if (!again && listed_ == videoId && !tracks_->failed()) return;   // listed already
    }
    const quint64 generation = ++generation_;
    tracks_->setLoading(videoId);
    ++fetches_;
    const bool sent = service_->fetch(videoId, this, [this, generation, videoId](const catalog::CaptionTrackList &list) {
        if (generation != generation_ || !watch_ || !tracks_ || videoId != watch_->videoId()) return;
        if (tracks_->videoId() == videoId && tracks_->local()) return;   // saved tracks arrived meanwhile
        if (catalog::isFailure(list.outcome)) {
            if (tracks_->videoId() != videoId) tracks_->setLoading(videoId);
            tracks_->setFailed(videoId);
            return;
        }
        listed_ = videoId;
        tracks_->setTracks(videoId, list.tracks);
    });
    if (!sent) tracks_->setFailed(videoId);
}

}}}
