// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#pragma once

// Zero-copy hardware frames (PLY-20): the contract between a hardware video
// sink (the engine's EglSinkBridge over droideglsink) and the render thread
// (VideoSurfaceItem's hardware node). Qt Core only and no GStreamer or EGL
// types (EGL handles travel as void *), so the QML-facing headers stay free of
// both. No SailTube types, so SailTune can pin it.
//
// Threading:
//   - frameSerial() and geometry() are callable from any thread.
//   - beginFrame() / endFrame() / contextLost() / reportRenderFailure() are
//     render-thread calls with the GL context current. beginFrame() takes the
//     source's frame lock and endFrame() releases it, so the sink cannot
//     replace or free the frame (or be torn down) during the whole draw.
//     Always pair them, also when beginFrame() returns false.

#include <QSize>
#include <QtGlobal>

namespace nordtile { namespace player {

// What the sink's caps say about the picture (and, once a frame was drawn,
// its crop rectangle).
struct HardwareVideoGeometry {
    int width = 0;           // picture width in pixels (crop applied when known)
    int height = 0;
    double pixelAspect = 1.0;

    bool valid() const { return width > 0 && height > 0; }
    // Display size after pixel-aspect correction (as VideoFrame::displaySize()).
    QSize displaySize() const;
};

// One frame handed to the renderer between beginFrame() and endFrame().
struct HardwareFrame {
    void *eglImage = nullptr;  // EGLImageKHR; valid until endFrame()
    // Normalised part of the image that holds the picture (crop meta);
    // (0, 0, 1, 1) = the whole image.
    double cropX = 0, cropY = 0, cropWidth = 1, cropHeight = 1;
    quint64 serial = 0;        // frameSerial() of the frame being drawn
};

class HardwareFrameSource {
public:
    virtual ~HardwareFrameSource() {}

    // Any thread. Bumped for every frame the sink announced while output is
    // enabled; 0 = no frame yet (nothing to show).
    virtual quint64 frameSerial() const = 0;
    virtual HardwareVideoGeometry geometry() const = 0;

    // PLY-20, render thread, GL context current. Takes the frame lock (always;
    // pair with endFrame()). Pushes `eglDisplay` (the render thread's
    // EGLDisplay) to the sink when it changed, makes the newest announced
    // frame current and returns its EGLImage in *frame. False = nothing to
    // draw (no frame yet, output disabled, source stopped, bind failed).
    virtual bool beginFrame(void *eglDisplay, HardwareFrame *frame) = 0;
    // Unbinds the frame (it stays acquired for repaints) and releases the lock.
    virtual void endFrame() = 0;

    // PLY-19, render thread: the GL context the images were used with is gone
    // or was replaced. Drops cached EGLImages (EGL calls only, never GL) and
    // re-pushes the display on the next beginFrame().
    virtual void contextLost() = 0;

    // PLY-20, render thread: frames cannot be shown here (extension missing,
    // no EGLImage, GL error). Reported once per source; the player falls back
    // to software decode for this video instead of a silent black picture.
    virtual void reportRenderFailure(const char *reason) = 0;
};

}}
