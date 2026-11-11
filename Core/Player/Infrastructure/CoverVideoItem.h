// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#pragma once

// Cover video: the playing video, live, in the Sailfish active cover (Sailfish
// has no picture-in-picture). A VideoSurfaceItem in reduced-rate mode:
//   - the SAME VideoFrameMailbox as the watch page's surface, as a second
//     listener: no second decoder, pipeline or mailbox producer; software
//     frames and the hardware (zero-copy) frame source both work, with the
//     surface's context-loss handling (PLY-19) unchanged;
//   - while `active`: at most one frame per kFrameIntervalMs (~15 fps);
//     intermediate frames are dropped, never queued; one single-shot precise
//     timer runs only while a frame waits (no periodic timer, NFR-PERF-06);
//   - while not `active`: the mailbox listener is detached, no updates and no
//     uploads; the last picture stays.
// Each drawn frame is also reported to the source when it has an invokable
// noteCoverFrameDrawn() (VideoPlayer counts them for diagnostics).
//
// QML (org.nordilabs.sailtube 1.0):
//   CoverVideoItem { source: playback.mediaObject
//                    active: playback.coverVideoActive && status === Cover.Active }
//
// Qt Quick only (it never touches GStreamer, so the STORE build links it); no
// SailTube types, so SailTune can pin it.

#include "VideoSurfaceItem.h"

#include <QMetaMethod>

namespace nordtile { namespace player {

class CoverVideoItem : public VideoSurfaceItem {  // not final: qmlRegisterType derives from it
    Q_OBJECT
    // `active` (VideoSurfaceItem): draw live frames (true) or keep the last picture (false,
    // the cover's default).
    // Cover video: frames handed to the scene graph since creation.
    Q_PROPERTY(int framesDrawn READ framesDrawn NOTIFY framesDrawnChanged)
public:
    // Cover video: the frame interval, at most ~15 fps (1000 / 67 = 14.9).
    static const int kFrameIntervalMs = 67;

    explicit CoverVideoItem(QQuickItem *parent = nullptr);

    int framesDrawn() const { return framesDrawn_; }

signals:
    void framesDrawnChanged();

protected:
    void framePresented() override;

private:
    void lookUpCounter();

    int framesDrawn_ = 0;
    QMetaMethod counter_;  // the source's noteCoverFrameDrawn(), if it has one
};

}}
