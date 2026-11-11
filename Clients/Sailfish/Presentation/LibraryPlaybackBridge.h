// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#pragma once

// Connects the player to the library without the player knowing about it:
// history and resume saves (LIB-04/05) through ProgressTracker, and the resume
// decision when a video opens (Always / Ask with a 5 s auto-resume / Never; a
// link t= wins). Reads come from the store's in-memory index, so opening a
// video never waits for library work (RES-02).
//
// Shorts loop items (PlaybackSession `loop`, read by name so this builds before the playback
// component's property lands): no resume offer or decision, and since a loop item never emits ended,
// its one end save (positionSaveRequested(id, -1) near the end) counts as watched; after it the
// replays are not tracked, so a later stop cannot store a resume point over "watched".

#include "Core/Library/Domain/LibraryPolicy.h"
#include "Core/Library/Domain/ProgressTracker.h"
#include <QObject>
#include <QPointer>
#include <QTimer>

namespace nordtile { namespace video {
namespace library { class LibraryStore; }
namespace sailfish {

class PlaybackSession;

class LibraryPlaybackBridge final : public QObject {
    Q_OBJECT
    // 0 always, 1 ask, 2 never (LIB-05); stored in the library meta table.
    Q_PROPERTY(int resumeBehaviour READ resumeBehaviour WRITE setResumeBehaviour NOTIFY resumeBehaviourChanged)
    Q_PROPERTY(bool resumeOfferVisible READ resumeOfferVisible NOTIFY resumeOfferChanged)
    Q_PROPERTY(double resumeOfferPosition READ resumeOfferPosition NOTIFY resumeOfferChanged)
    Q_PROPERTY(int resumeOfferSecondsLeft READ resumeOfferSecondsLeft NOTIFY resumeOfferChanged)
public:
    LibraryPlaybackBridge(PlaybackSession *playback, library::LibraryStore *store, QObject *parent = nullptr);

    int resumeBehaviour() const;
    void setResumeBehaviour(int behaviour);
    bool resumeOfferVisible() const { return offerVisible_; }
    double resumeOfferPosition() const { return static_cast<double>(offerPositionMs_); }
    int resumeOfferSecondsLeft() const { return offerSecondsLeft_; }

    // LNK-02: a t= from a link for the next open of this video (milliseconds).
    Q_INVOKABLE void setLinkStart(const QString &videoId, double milliseconds);
    Q_INVOKABLE void acceptResume();
    Q_INVOKABLE void declineResume();
    // Writes the current progress (application shutdown).
    Q_INVOKABLE void flush();

signals:
    void resumeBehaviourChanged();
    void resumeOfferChanged();

private slots:
    void loopChanged();

private:
    bool loopItem() const;
    void positionSaveRequested(const QString &videoId, double positionMs);
    void detailsChanged();
    void positionChanged();
    void playerChanged();
    void stateChanged();
    void apply(const std::vector<library::ProgressReport> &reports);
    void hideOffer();
    library::VideoEntry currentEntry() const;

    QPointer<PlaybackSession> playback_;
    library::LibraryStore *store_;
    library::ProgressTracker tracker_;
    QString openedId_;
    bool wasPlaying_ = false;
    bool loopWatched_ = false;   // this loop item's end was recorded; replays are not tracked
    qint64 lastPositionMs_ = 0;
    QString linkVideoId_;
    qint64 linkStartMs_ = -1;
    bool offerVisible_ = false;
    qint64 offerPositionMs_ = 0;
    int offerSecondsLeft_ = 0;
    QTimer offerTimer_;
};

}}}
