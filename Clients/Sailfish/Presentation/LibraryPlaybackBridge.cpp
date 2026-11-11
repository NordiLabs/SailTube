// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "LibraryPlaybackBridge.h"

#include "Core/Library/Infrastructure/LibraryStore.h"
#include "PlaybackSession.h"
#include <QMetaObject>
#include <QVariant>
#include <algorithm>

namespace nordtile { namespace video { namespace sailfish {

namespace {
const char *const behaviourKey = "resume_behaviour";
std::string u(const QString &s) { return s.toUtf8().toStdString(); }
}

LibraryPlaybackBridge::LibraryPlaybackBridge(PlaybackSession *playback, library::LibraryStore *store, QObject *parent)
    : QObject(parent), playback_(playback), store_(store), tracker_(library::limits::resumeSaveIntervalMs) {
    offerTimer_.setInterval(1000);
    connect(&offerTimer_, &QTimer::timeout, this, [this]() {
        if (--offerSecondsLeft_ <= 0) { acceptResume(); return; }
        emit resumeOfferChanged();
    });
    connect(playback, &PlaybackSession::detailsChanged, this, &LibraryPlaybackBridge::detailsChanged);
    connect(playback, &PlaybackSession::positionChanged, this, &LibraryPlaybackBridge::positionChanged);
    connect(playback, &PlaybackSession::playerChanged, this, &LibraryPlaybackBridge::playerChanged);
    connect(playback, &PlaybackSession::stateChanged, this, &LibraryPlaybackBridge::stateChanged);
    // Playback component: the session reports the natural end of stream itself.
    connect(playback, &PlaybackSession::ended, this, [this]() {
        hideOffer();
        apply(tracker_.ended(library::libraryNowMs()));
    });
    connect(playback, &PlaybackSession::positionSaveRequested, this, &LibraryPlaybackBridge::positionSaveRequested);
    // Shorts: playback's loop hint (feature-detected; absent in older sessions).
    if (playback->metaObject()->indexOfSignal("loopChanged()") >= 0)
        connect(playback, SIGNAL(loopChanged()), this, SLOT(loopChanged()));
}

bool LibraryPlaybackBridge::loopItem() const {
    return playback_ && playback_->property("loop").toBool();
}

void LibraryPlaybackBridge::loopChanged() {
    if (loopItem()) hideOffer();   // a loop item has no resume point; accepting would seek
}

void LibraryPlaybackBridge::positionSaveRequested(const QString &videoId, double positionMs) {
    if (!playback_ || loopWatched_ || videoId.isEmpty() || videoId != openedId_ || !loopItem()) return;
    const qint64 duration = static_cast<qint64>(playback_->duration());
    const qint64 last = std::max(lastPositionMs_, static_cast<qint64>(playback_->position()));
    if (!library::loopEndReached(static_cast<std::int64_t>(positionMs), last, duration)) return;
    hideOffer();
    apply(tracker_.ended(library::libraryNowMs()));
    loopWatched_ = true;
}

int LibraryPlaybackBridge::resumeBehaviour() const {
    const int value = store_->metaValue(QString::fromLatin1(behaviourKey), QStringLiteral("0")).toInt();
    return value >= 0 && value <= 2 ? value : 0;
}

void LibraryPlaybackBridge::setResumeBehaviour(int behaviour) {
    if (behaviour < 0 || behaviour > 2 || behaviour == resumeBehaviour()) return;
    store_->setMetaValue(QString::fromLatin1(behaviourKey), QString::number(behaviour));
    emit resumeBehaviourChanged();
}

void LibraryPlaybackBridge::setLinkStart(const QString &videoId, double milliseconds) {
    linkVideoId_ = videoId;
    linkStartMs_ = milliseconds >= 0 ? static_cast<qint64>(milliseconds) : -1;
}

library::VideoEntry LibraryPlaybackBridge::currentEntry() const {
    library::VideoEntry v;
    v.videoId = u(playback_->videoId());
    v.title = u(playback_->title());
    v.channel = u(playback_->channel());
    v.channelId = u(playback_->channelId());
    v.thumbnailUrl = u(playback_->thumbnail());
    const double duration = playback_->duration();
    v.durationSeconds = duration > 0 ? static_cast<std::int64_t>(duration / 1000.0) : -1;
    return v;
}

void LibraryPlaybackBridge::detailsChanged() {
    if (!playback_) return;
    const QString id = playback_->videoId();
    if (id == openedId_) {
        tracker_.updateVideo(currentEntry(), playback_->live());
        return;
    }
    hideOffer();
    openedId_ = id;
    wasPlaying_ = false;
    loopWatched_ = false;
    lastPositionMs_ = 0;
    apply(tracker_.open(currentEntry(), playback_->live(), library::libraryNowMs()));
    if (id.isEmpty()) return;

    const qint64 link = id == linkVideoId_ ? linkStartMs_ : -1;
    linkVideoId_.clear();
    linkStartMs_ = -1;
    // Shorts: a loop item starts at 0 (playback applies a link start itself) and is never offered a resume.
    if (loopItem()) return;
    const library::ResumeDecision d = library::decideResume(
        store_->resumePositionMs(id), static_cast<library::ResumeBehaviour>(resumeBehaviour()), link);
    switch (d.action) {
    case library::ResumeAction::ResumeAt:
    case library::ResumeAction::StartAtLink:
        playback_->setStartPosition(static_cast<double>(d.positionMs));
        break;
    case library::ResumeAction::AskThenResume:
        offerPositionMs_ = d.positionMs;
        offerSecondsLeft_ = d.autoResumeSeconds;
        offerVisible_ = true;
        offerTimer_.start();
        emit resumeOfferChanged();
        break;
    case library::ResumeAction::StartOver:
        break;
    }
}

void LibraryPlaybackBridge::acceptResume() {
    if (!offerVisible_) return;
    if (loopItem()) { hideOffer(); return; }
    const qint64 position = offerPositionMs_;
    hideOffer();
    if (!playback_) return;
    if (playback_->seekable()) playback_->seek(static_cast<double>(position));
    else playback_->setStartPosition(static_cast<double>(position));
}

void LibraryPlaybackBridge::declineResume() { hideOffer(); }

void LibraryPlaybackBridge::hideOffer() {
    offerTimer_.stop();
    if (!offerVisible_) return;
    offerVisible_ = false;
    offerSecondsLeft_ = 0;
    emit resumeOfferChanged();
}

void LibraryPlaybackBridge::positionChanged() {
    if (!playback_ || loopWatched_) return;
    lastPositionMs_ = static_cast<qint64>(playback_->position());
    apply(tracker_.position(static_cast<qint64>(playback_->position()), static_cast<qint64>(playback_->duration()),
                            library::libraryNowMs()));
}

void LibraryPlaybackBridge::playerChanged() {
    if (!playback_ || loopWatched_) return;
    const bool playing = playback_->playing();
    if (playing == wasPlaying_) return;
    wasPlaying_ = playing;
    apply(tracker_.playingChanged(playing, library::libraryNowMs()));
}

void LibraryPlaybackBridge::stateChanged() {
    if (playback_ && playback_->state() == PlaybackSession::Idle && tracker_.active()) {
        hideOffer();
        apply(tracker_.stop(library::libraryNowMs()));
        openedId_.clear();
    }
}

void LibraryPlaybackBridge::flush() {
    hideOffer();
    apply(tracker_.stop(library::libraryNowMs()));
    openedId_.clear();
}

void LibraryPlaybackBridge::apply(const std::vector<library::ProgressReport> &reports) {
    for (const library::ProgressReport &r : reports) store_->recordProgress(r.progress, r.newSession);
}

}}}
