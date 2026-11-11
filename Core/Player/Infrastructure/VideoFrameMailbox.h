// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#pragma once

// Hand-over point between a decoder thread and the render thread: holds the
// latest decoded RGBA frame (no queue; a newer frame replaces an older one).
// The frame's pixels are not copied: `keepAlive` owns whatever backs `data`
// (for GStreamer a mapped GstSample) and releases it when the last holder
// drops it, on whichever thread that happens. Qt Core only; no SailTube types.
//
// Hardware decode (PLY-20) bypasses the RGBA frames: the engine attaches a
// HardwareFrameSource for the lifetime of a hardware pipeline and the surface
// renders from it instead (zero copy). Its frame announcements reach the
// same listeners.

#include "HardwareFrameSource.h"

#include <QMutex>
#include <QSize>
#include <functional>
#include <memory>
#include <utility>
#include <vector>

namespace nordtile { namespace player {

struct VideoFrame {
    std::shared_ptr<const void> keepAlive; // owns the memory behind data
    const unsigned char *data = nullptr;   // RGBA, top-down rows
    int width = 0;
    int height = 0;
    int stride = 0;                        // bytes per row
    double pixelAspect = 1.0;              // par_n / par_d from caps
    quint64 serial = 0;                    // set by the mailbox

    bool valid() const { return data && width > 0 && height > 0 && stride >= width * 4; }
    // Display size after pixel-aspect correction.
    QSize displaySize() const;
};

class VideoFrameMailbox final {
public:
    // Called (from any thread) after a new frame was posted. Must be cheap and
    // thread-safe; typically a queued QQuickItem::update().
    typedef std::function<void()> Listener;

    void post(const VideoFrame &frame);
    // Latest frame (shares ownership). serial 0 = none.
    VideoFrame latest() const;
    // Drops the held frame (teardown, new video); listeners are told.
    void clear();
    quint64 serial() const;
    // One listener per surface; returns an id for removeListener.
    int addListener(const Listener &listener);
    void removeListener(int id);

    // PLY-20: the hardware frame source of the current pipeline, or null in
    // software mode. Setting it (also to null) tells the listeners.
    void setHardwareSource(const std::shared_ptr<HardwareFrameSource> &source);
    std::shared_ptr<HardwareFrameSource> hardwareSource() const;
    // PLY-20: the hardware source announced a frame (any thread).
    void notifyListeners() { notify(); }

private:
    void notify();
    mutable QMutex mutex_;          // frame_, serial_ and hardware_
    mutable QMutex listenerMutex_;  // listeners_; held while notifying, so a removed
                                    // listener is never called afterwards
    VideoFrame frame_;
    quint64 serial_ = 0;
    std::shared_ptr<HardwareFrameSource> hardware_;
    int nextListener_ = 1;
    std::vector<std::pair<int, Listener>> listeners_;
};

// Implemented by the player object a surface shows (the GStreamer engine's
// VideoPlayer). Qt Core only, so players need no Qt Quick headers.
class VideoFrameProvider {
public:
    virtual ~VideoFrameProvider() {}
    virtual VideoFrameMailbox *frameMailbox() = 0;
};

}}

#include <QtPlugin>
Q_DECLARE_INTERFACE(nordtile::player::VideoFrameProvider, "org.nordilabs.player.VideoFrameProvider/1.0")
