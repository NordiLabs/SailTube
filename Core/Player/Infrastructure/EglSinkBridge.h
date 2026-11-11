// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#pragma once

// PLY-20: HardwareFrameSource over a zero-copy EGL video sink (droideglsink),
// using only public interfaces installed with nemo-gstreamer-interfaces-1.0:
//
//   Texture mode (default): the sink implements NemoGstVideoTexture. Its
//     "frame-ready" signal (gint frame; < 0 = no frame) announces frames; the
//     render thread calls acquire_frame / bind_frame (-> EGLImageKHR) /
//     unbind_frame / release_frame and reads GstVideoCropMeta through
//     get_frame_meta. The sink creates the images on the "egl-display" we push.
//   Buffer mode (fallback, or SAILTUBE_HW_RENDER=buffer): the sink has a
//     "show-frame" signal carrying a GstBuffer whose memory comes from a
//     NemoGstEglImageAllocator. We create one EGLImage per GstMemory with
//     nemo_gst_egl_image_memory_create_image, cache it on the memory (qdata,
//     destroyed with eglDestroyImageKHR when the memory dies) and drop the
//     cache on "buffers-invalidated", a context loss or a display change.
//
// Threading and locks:
//   stateMutex_  sink signal handlers only record "a new frame" here (and
//                ref/unref a buffer), then post to the mailbox's listeners.
//                Never held while calling into the sink.
//   frameMutex_  held by the render thread for the whole draw (beginFrame to
//                endFrame) and by stopRendering()/detachSink(), so the frame
//                and the sink outlive any draw (render vs delete).
//   callbackMutex_ held while a callback runs and while detachSink() clears
//                them, so no callback reaches the engine after teardown.
//   Order: frameMutex_ -> stateMutex_, frameMutex_ -> callbackMutex_; a
//   callback never takes a bridge lock.
//
// Engine side: create after the sink exists (state NULL), stopRendering()
// before the pipeline goes to NULL (gives held frames back), detachSink()
// after (no streaming thread can emit any more). The surface may keep the
// object (shared_ptr) longer; it then draws nothing.

#include "HardwareFrameSource.h"

#include <QMutex>

#include <functional>
#include <memory>
#include <string>

typedef struct _GstElement GstElement;
typedef struct _GstBuffer GstBuffer;

namespace nordtile { namespace player {

class EglSinkBridge final : public HardwareFrameSource {
public:
    enum class Mode { Texture, Buffer };

    // Invoked under callbackMutex_ only (never with stateMutex_ held), so
    // detachSink() can wait for a running one; they must not call back into
    // the bridge.
    struct Callbacks {
        // Streaming thread: a frame was announced while output is enabled
        // (first-frame accounting + surface update).
        std::function<void()> frameAnnounced;
        // Any thread: the picture changed without a new frame (crop, output
        // re-enabled, buffers invalidated): update the surface.
        std::function<void()> geometryChanged;
        // Render thread: reportRenderFailure() (first call only).
        std::function<void()> renderFailed;
    };

    // PLY-20: null when `sink` offers neither mode (*why says which). Refs the sink.
    static std::shared_ptr<EglSinkBridge> create(GstElement *sink, const Callbacks &callbacks, std::string *why = nullptr);
    ~EglSinkBridge() override;

    Mode mode() const { return mode_; }

    // Streaming thread (video sink pad caps): picture size and pixel aspect.
    void setCaps(int width, int height, double pixelAspect);
    // PLY-06: while disabled, announced frames are recorded but neither shown
    // nor reported. Enabling reports a recorded frame. Returns true when a
    // frame had been recorded (the engine then posts the first-frame notice).
    bool setOutputEnabled(bool enabled);
    // Frames announced so far (diagnostics, first-frame accounting).
    quint64 framesAnnounced() const;
    // Why rendering failed ("" = it did not), for the fallback notice.
    std::string failureReason() const;

    // Engine teardown, owner thread. See the header comment.
    void stopRendering();
    void detachSink();

    // HardwareFrameSource
    quint64 frameSerial() const override;
    HardwareVideoGeometry geometry() const override;
    bool beginFrame(void *eglDisplay, HardwareFrame *frame) override;
    void endFrame() override;
    void contextLost() override;
    void reportRenderFailure(const char *reason) override;

    // C signal trampolines (EglSinkBridge.cpp).
    struct Signals;

private:
    EglSinkBridge(GstElement *sink, Mode mode, const Callbacks &callbacks);
    void announce(GstBuffer *buffer, bool hasFrame);
    void invalidateBuffers();
    void releaseHeldLocked();          // frameMutex_ held
    bool beginTextureFrame(HardwareFrame *frame);
    bool beginBufferFrame(void *eglDisplay, HardwareFrame *frame);
    void noteCrop(int x, int y, int w, int h, int imageWidth, int imageHeight, HardwareFrame *frame);

    const Mode mode_;
    QMutex callbackMutex_;
    Callbacks callbacks_;              // cleared by detachSink()
    const quint64 id_;                 // owner tag of cached images

    QMutex frameMutex_;
    GstElement *sink_ = nullptr;       // ref; null after detachSink()
    bool stopped_ = false;
    bool held_ = false;                // Texture: a frame is acquired
    bool bound_ = false;               // Texture: bind_frame succeeded this draw
    GstBuffer *current_ = nullptr;     // Buffer: the frame being shown (ref)
    quint64 heldSerial_ = 0;
    void *pushedDisplay_ = nullptr;    // egl-display last pushed to the sink
    quint64 imageEpoch_ = 1;           // Buffer: cached images of other epochs are stale
    int bindFailures_ = 0;             // consecutive draws without an EGLImage
    unsigned long handlers_[3] = {0, 0, 0};

    mutable QMutex stateMutex_;
    quint64 announced_ = 0;            // frames announced (any output state)
    quint64 serial_ = 0;               // what frameSerial() reports
    bool outputEnabled_ = true;
    bool hasFrame_ = false;            // last announcement carried a frame
    GstBuffer *pending_ = nullptr;     // Buffer: newest announced buffer (ref)
    bool invalidated_ = false;         // Buffer: "buffers-invalidated" seen
    bool failed_ = false;              // reportRenderFailure() was called
    std::string failure_;
    HardwareVideoGeometry caps_;
    HardwareVideoGeometry cropped_;    // caps_ with the last crop applied
};

}}
