// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "EglSinkBridge.h"

#include <EGL/egl.h>
#include <EGL/eglext.h>

#include <gst/gst.h>
#include <gst/interfaces/nemoeglimagememory.h>
#include <gst/interfaces/nemovideotexture.h>
#include <gst/video/video.h>

#include <QMutexLocker>

#include <atomic>
#include <cstdlib>
#include <cstring>

namespace nordtile { namespace player {

namespace {

// Consecutive draws with an acquired frame but no EGLImage before the source
// reports a render failure (PLY-20: never a silent black picture).
const int kMaxBindFailures = 5;

std::atomic<quint64> gNextBridgeId(1);

typedef EGLBoolean (*DestroyImageFn)(EGLDisplay, EGLImageKHR);

DestroyImageFn destroyImageFn() {
    static DestroyImageFn fn = reinterpret_cast<DestroyImageFn>(eglGetProcAddress("eglDestroyImageKHR"));
    return fn;
}

// Buffer mode: the EGLImage of one GstMemory, kept as qdata on the memory so
// it dies with it (pool memories are reused, so an address is never reused
// while the entry lives).
struct ImageEntry {
    EGLDisplay display;
    EGLImageKHR image;
    quint64 owner;   // bridge id
    quint64 epoch;   // bridge image epoch at creation
};

void destroyEntry(gpointer data) {
    ImageEntry *e = static_cast<ImageEntry *>(data);
    // EGLImages belong to the display, not to a context: safe on any thread.
    if (e->image && destroyImageFn()) destroyImageFn()(e->display, e->image);
    delete e;
}

GQuark imageQuark() {
    static const GQuark q = g_quark_from_static_string("sailtube-egl-image");
    return q;
}

// True when `object` has signal `name` with `nParams` parameters (the first
// of type `param0`, or G_TYPE_POINTER) and no return value.
bool hasSignal(GstElement *object, const char *name, guint nParams, GType param0) {
    const guint id = g_signal_lookup(name, G_OBJECT_TYPE(object));
    if (!id) return false;
    GSignalQuery q;
    g_signal_query(id, &q);
    if (q.n_params != nParams || q.return_type != G_TYPE_NONE) return false;
    if (nParams == 0) return true;
    const GType t = q.param_types[0] & ~G_SIGNAL_TYPE_STATIC_SCOPE;
    return t == param0 || t == G_TYPE_POINTER;
}

bool hasProperty(GstElement *object, const char *name) {
    return g_object_class_find_property(G_OBJECT_GET_CLASS(object), name) != nullptr;
}

} // namespace

struct EglSinkBridge::Signals {
    static void frameReady(GstElement *, gint frame, gpointer data) {
        static_cast<EglSinkBridge *>(data)->announce(nullptr, frame >= 0);
    }
    static void showFrame(GstElement *, gpointer buffer, gpointer data) {
        GstBuffer *b = buffer && GST_IS_BUFFER(buffer) ? static_cast<GstBuffer *>(buffer) : nullptr;
        static_cast<EglSinkBridge *>(data)->announce(b, b != nullptr);
    }
    static void buffersInvalidated(GstElement *, gpointer data) {
        static_cast<EglSinkBridge *>(data)->invalidateBuffers();
    }
};

std::shared_ptr<EglSinkBridge> EglSinkBridge::create(GstElement *sink, const Callbacks &callbacks, std::string *why) {
    if (!sink) {
        if (why) *why = "no video sink";
        return std::shared_ptr<EglSinkBridge>();
    }
    const bool texture = NEMO_GST_IS_VIDEO_TEXTURE(sink);
    const bool buffer = hasSignal(sink, "show-frame", 1, GST_TYPE_BUFFER);
    const char *force = std::getenv("SAILTUBE_HW_RENDER");  // D3 A/B: "texture" or "buffer"
    Mode mode;
    if (force && std::strcmp(force, "buffer") == 0 && buffer) mode = Mode::Buffer;
    else if (force && std::strcmp(force, "texture") == 0 && texture) mode = Mode::Texture;
    else if (texture) mode = Mode::Texture;
    else if (buffer) mode = Mode::Buffer;
    else {
        if (why) *why = std::string(GST_ELEMENT_NAME(sink)) + " offers neither NemoGstVideoTexture nor show-frame";
        return std::shared_ptr<EglSinkBridge>();
    }
    std::shared_ptr<EglSinkBridge> b(new EglSinkBridge(sink, mode, callbacks));
    if (mode == Mode::Texture) {
        b->handlers_[0] = g_signal_connect(sink, "frame-ready", G_CALLBACK(&Signals::frameReady), b.get());
    } else {
        b->handlers_[0] = g_signal_connect(sink, "show-frame", G_CALLBACK(&Signals::showFrame), b.get());
        if (hasSignal(sink, "buffers-invalidated", 0, G_TYPE_NONE))
            b->handlers_[1] =
                g_signal_connect(sink, "buffers-invalidated", G_CALLBACK(&Signals::buffersInvalidated), b.get());
    }
    return b;
}

EglSinkBridge::EglSinkBridge(GstElement *sink, Mode mode, const Callbacks &callbacks)
    : mode_(mode), callbacks_(callbacks), id_(gNextBridgeId++) {
    sink_ = static_cast<GstElement *>(gst_object_ref(sink));
}

EglSinkBridge::~EglSinkBridge() {
    detachSink();
    QMutexLocker lock(&frameMutex_);
    releaseHeldLocked();
}

// ---- Sink side (streaming threads) ------------------------------------------------

void EglSinkBridge::announce(GstBuffer *buffer, bool hasFrame) {
    GstBuffer *dropped = nullptr;
    bool notify = false;
    {
        QMutexLocker lock(&stateMutex_);
        ++announced_;
        hasFrame_ = hasFrame;
        if (mode_ == Mode::Buffer) {
            dropped = pending_;  // never shown: superseded
            pending_ = buffer ? gst_buffer_ref(buffer) : nullptr;
        }
        if (outputEnabled_) {
            ++serial_;
            notify = true;
        }
    }
    if (dropped) gst_buffer_unref(dropped);
    if (notify) {
        QMutexLocker lock(&callbackMutex_);
        if (callbacks_.frameAnnounced) callbacks_.frameAnnounced();
    }
}

void EglSinkBridge::invalidateBuffers() {
    GstBuffer *dropped = nullptr;
    {
        QMutexLocker lock(&stateMutex_);
        invalidated_ = true;  // the next draw drops the cached images
        dropped = pending_;
        pending_ = nullptr;
    }
    if (dropped) gst_buffer_unref(dropped);
    QMutexLocker lock(&callbackMutex_);
    if (callbacks_.geometryChanged) callbacks_.geometryChanged();  // repaint soon
}

void EglSinkBridge::setCaps(int width, int height, double pixelAspect) {
    QMutexLocker lock(&stateMutex_);
    caps_.width = width;
    caps_.height = height;
    caps_.pixelAspect = pixelAspect > 0 ? pixelAspect : 1.0;
    cropped_ = HardwareVideoGeometry();
}

bool EglSinkBridge::setOutputEnabled(bool enabled) {
    bool recorded = false;
    {
        QMutexLocker lock(&stateMutex_);
        if (outputEnabled_ == enabled) return false;
        outputEnabled_ = enabled;
        if (enabled && hasFrame_) {
            ++serial_;
            recorded = true;
        }
    }
    if (recorded) {
        QMutexLocker lock(&callbackMutex_);
        if (callbacks_.geometryChanged) callbacks_.geometryChanged();  // show it
    }
    return recorded;
}

quint64 EglSinkBridge::framesAnnounced() const {
    QMutexLocker lock(&stateMutex_);
    return announced_;
}

std::string EglSinkBridge::failureReason() const {
    QMutexLocker lock(&stateMutex_);
    return failure_;
}

// ---- Teardown (owner thread) ---------------------------------------------------------

void EglSinkBridge::stopRendering() {
    GstBuffer *dropped = nullptr;
    {
        QMutexLocker lock(&frameMutex_);  // waits for a draw in progress
        stopped_ = true;
        releaseHeldLocked();
        QMutexLocker state(&stateMutex_);
        dropped = pending_;
        pending_ = nullptr;
    }
    if (dropped) gst_buffer_unref(dropped);
}

void EglSinkBridge::detachSink() {
    GstElement *sink = nullptr;
    {
        QMutexLocker lock(&frameMutex_);
        stopped_ = true;
        releaseHeldLocked();
        if (sink_) {
            for (int i = 0; i < 3; ++i)
                if (handlers_[i]) g_signal_handler_disconnect(sink_, handlers_[i]);
            handlers_[0] = handlers_[1] = handlers_[2] = 0;
        }
        sink = sink_;
        sink_ = nullptr;
    }
    {
        QMutexLocker lock(&callbackMutex_);  // waits for a callback in progress
        callbacks_ = Callbacks();
    }
    GstBuffer *dropped = nullptr;
    {
        QMutexLocker lock(&stateMutex_);
        dropped = pending_;
        pending_ = nullptr;
    }
    if (dropped) gst_buffer_unref(dropped);
    if (sink) gst_object_unref(sink);
}

void EglSinkBridge::releaseHeldLocked() {
    if (mode_ == Mode::Texture && held_ && sink_) {
        if (bound_) nemo_gst_video_texture_unbind_frame(NEMO_GST_VIDEO_TEXTURE(sink_));
        // No EGLSyncKHR: the frame was last drawn before the previous swap.
        nemo_gst_video_texture_release_frame(NEMO_GST_VIDEO_TEXTURE(sink_), nullptr);
    }
    bound_ = false;
    held_ = false;
    heldSerial_ = 0;
    if (current_) {
        gst_buffer_unref(current_);
        current_ = nullptr;
    }
}

// ---- Render thread ------------------------------------------------------------------------

quint64 EglSinkBridge::frameSerial() const {
    QMutexLocker lock(&stateMutex_);
    return outputEnabled_ && hasFrame_ ? serial_ : 0;
}

HardwareVideoGeometry EglSinkBridge::geometry() const {
    QMutexLocker lock(&stateMutex_);
    return cropped_.valid() ? cropped_ : caps_;
}

bool EglSinkBridge::beginFrame(void *eglDisplay, HardwareFrame *frame) {
    frameMutex_.lock();  // released by endFrame()
    bound_ = false;
    if (stopped_ || !sink_ || !frame) return false;
    {
        QMutexLocker lock(&stateMutex_);
        if (failed_) return false;
    }
    if (eglDisplay && eglDisplay != pushedDisplay_) {
        if (hasProperty(sink_, "egl-display")) g_object_set(sink_, "egl-display", eglDisplay, nullptr);
        if (pushedDisplay_) ++imageEpoch_;  // images of another display are stale
        pushedDisplay_ = eglDisplay;
    }
    *frame = HardwareFrame();
    return mode_ == Mode::Texture ? beginTextureFrame(frame) : beginBufferFrame(eglDisplay, frame);
}

bool EglSinkBridge::beginTextureFrame(HardwareFrame *frame) {
    quint64 want;
    bool has, enabled;
    {
        QMutexLocker lock(&stateMutex_);
        want = serial_;
        has = hasFrame_;
        enabled = outputEnabled_;
    }
    if (!has) {
        releaseHeldLocked();  // the sink said there is no frame any more
        return false;
    }
    if (!enabled) return false;
    NemoGstVideoTexture *texture = NEMO_GST_VIDEO_TEXTURE(sink_);
    if (!held_ || heldSerial_ != want) {
        releaseHeldLocked();
        if (nemo_gst_video_texture_acquire_frame(texture)) {
            held_ = true;
            heldSerial_ = want;
        }
    }
    if (!held_) return false;
    EGLImageKHR image = nullptr;
    if (!nemo_gst_video_texture_bind_frame(texture, &image) || !image) {
        if (++bindFailures_ >= kMaxBindFailures) reportRenderFailure("the sink bound no EGLImage");
        return false;
    }
    bindFailures_ = 0;
    bound_ = true;
    frame->eglImage = image;
    frame->serial = heldSerial_;
    const GstVideoCropMeta *crop = reinterpret_cast<const GstVideoCropMeta *>(
        nemo_gst_video_texture_get_frame_meta(texture, GST_VIDEO_CROP_META_API_TYPE));
    if (crop) noteCrop(int(crop->x), int(crop->y), int(crop->width), int(crop->height), 0, 0, frame);
    return true;
}

bool EglSinkBridge::beginBufferFrame(void *eglDisplay, HardwareFrame *frame) {
    GstBuffer *next = nullptr;
    bool has, enabled, invalidated;
    quint64 want;
    {
        QMutexLocker lock(&stateMutex_);
        next = pending_;
        pending_ = nullptr;
        has = hasFrame_;
        enabled = outputEnabled_;
        invalidated = invalidated_;
        invalidated_ = false;
        want = serial_;
    }
    if (invalidated) ++imageEpoch_;
    if (next) {
        if (current_) gst_buffer_unref(current_);
        current_ = next;
        heldSerial_ = want;
    }
    if (!has) {
        releaseHeldLocked();
        return false;
    }
    if (!enabled || !current_ || !eglDisplay) return false;
    if (gst_buffer_n_memory(current_) < 1) return false;
    GstMemory *mem = gst_buffer_peek_memory(current_, 0);
    if (!mem || !mem->allocator || !NEMO_GST_IS_EGL_IMAGE_ALLOCATOR(mem->allocator)) {
        reportRenderFailure("the sink's buffers are not EGL-image memory");
        return false;
    }
    ImageEntry *entry = static_cast<ImageEntry *>(gst_mini_object_get_qdata(GST_MINI_OBJECT_CAST(mem), imageQuark()));
    if (!entry || entry->owner != id_ || entry->epoch != imageEpoch_ || entry->display != eglDisplay) {
        // Native-buffer images take no context; try the current one as a fallback.
        EGLImageKHR image = nemo_gst_egl_image_memory_create_image(mem, eglDisplay, EGL_NO_CONTEXT);
        if (!image) image = nemo_gst_egl_image_memory_create_image(mem, eglDisplay, eglGetCurrentContext());
        if (!image) {
            if (++bindFailures_ >= kMaxBindFailures) reportRenderFailure("no EGLImage for the sink's buffer");
            return false;
        }
        entry = new ImageEntry{eglDisplay, image, id_, imageEpoch_};
        // Replacing the qdata destroys the stale entry (and its image).
        gst_mini_object_set_qdata(GST_MINI_OBJECT_CAST(mem), imageQuark(), entry, &destroyEntry);
    }
    bindFailures_ = 0;
    frame->eglImage = entry->image;
    frame->serial = heldSerial_;
    const GstVideoCropMeta *crop = gst_buffer_get_video_crop_meta(current_);
    const GstVideoMeta *vmeta = gst_buffer_get_video_meta(current_);
    if (crop)
        noteCrop(int(crop->x), int(crop->y), int(crop->width), int(crop->height), vmeta ? int(vmeta->width) : 0,
                 vmeta ? int(vmeta->height) : 0, frame);
    return true;
}

void EglSinkBridge::noteCrop(int x, int y, int w, int h, int imageW, int imageH, HardwareFrame *frame) {
    bool changed = false;
    {
        QMutexLocker lock(&stateMutex_);
        // The image size: the buffer's video meta when known, else the caps.
        const int iw = imageW > 0 ? imageW : caps_.width;
        const int ih = imageH > 0 ? imageH : caps_.height;
        if (iw <= 0 || ih <= 0 || w <= 0 || h <= 0 || x < 0 || y < 0 || x + w > iw || y + h > ih) return;
        frame->cropX = double(x) / iw;
        frame->cropY = double(y) / ih;
        frame->cropWidth = double(w) / iw;
        frame->cropHeight = double(h) / ih;
        if (cropped_.width != w || cropped_.height != h) {
            cropped_.width = w;
            cropped_.height = h;
            cropped_.pixelAspect = caps_.pixelAspect;
            changed = true;
        }
    }
    if (changed) {
        QMutexLocker lock(&callbackMutex_);
        if (callbacks_.geometryChanged) callbacks_.geometryChanged();
    }
}

void EglSinkBridge::endFrame() {
    if (bound_ && sink_ && mode_ == Mode::Texture) nemo_gst_video_texture_unbind_frame(NEMO_GST_VIDEO_TEXTURE(sink_));
    bound_ = false;
    frameMutex_.unlock();
}

void EglSinkBridge::contextLost() {
    QMutexLocker lock(&frameMutex_);
    ++imageEpoch_;            // Buffer: recreate images lazily
    pushedDisplay_ = nullptr;  // re-push the (possibly new) display
}

void EglSinkBridge::reportRenderFailure(const char *reason) {
    {
        QMutexLocker lock(&stateMutex_);
        if (failed_) return;
        failed_ = true;
        failure_ = reason ? reason : "hardware frames cannot be shown";
    }
    QMutexLocker lock(&callbackMutex_);
    if (callbacks_.renderFailed) callbacks_.renderFailed();
}

}}
