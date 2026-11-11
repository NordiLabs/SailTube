// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// PLY-20 / PLY-19: EglSinkBridge against two fake sinks built on the installed
// public headers only: one implementing NemoGstVideoTexture (texture mode),
// one with "show-frame" / "buffers-invalidated" and EGL-image memory (buffer
// mode). No GL and no droid elements; real rendering is device-only (§6 D3,
// D7). Build: tests/player_hw/run.sh
#include "Core/Player/Infrastructure/EglSinkBridge.h"
#include "Core/Player/Infrastructure/VideoFrameMailbox.h"

#include <gst/gst.h>
#include <gst/interfaces/nemoeglimagememory.h>
#include <gst/interfaces/nemovideotexture.h>
#include <gst/video/video.h>

#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <thread>

using namespace nordtile::player;

static int checks = 0, failures = 0;
#define CHECK(c) do { ++checks; if (!(c)) { std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); ++failures; } } while (0)

// ---- Fake texture sink (NemoGstVideoTexture) -------------------------------------------

struct FakeTextureSink {
    GstElement parent;
    gpointer display;
    gboolean acquireOk;
    gboolean bindOk;
    GstBuffer *metaBuffer;       // crop meta source
};
struct FakeTextureSinkClass { GstElementClass parent_class; };

enum { kAcquire, kBind, kUnbind, kRelease, kDisplaySet, kCounters };
static std::atomic<int> textureCounters[kCounters];
static void *const kImage = reinterpret_cast<void *>(0xbeef);

static FakeTextureSink *fakeTexture(NemoGstVideoTexture *iface) { return reinterpret_cast<FakeTextureSink *>(iface); }

static gboolean ftAcquire(NemoGstVideoTexture *iface) {
    ++textureCounters[kAcquire];
    return fakeTexture(iface)->acquireOk;
}
static gboolean ftBind(NemoGstVideoTexture *iface, EGLImageKHR *image) {
    ++textureCounters[kBind];
    *image = fakeTexture(iface)->bindOk ? kImage : nullptr;
    return fakeTexture(iface)->bindOk;
}
static void ftUnbind(NemoGstVideoTexture *) { ++textureCounters[kUnbind]; }
static void ftRelease(NemoGstVideoTexture *, EGLSyncKHR) { ++textureCounters[kRelease]; }
static GstMeta *ftMeta(NemoGstVideoTexture *iface, GType api) {
    GstBuffer *b = fakeTexture(iface)->metaBuffer;
    return b ? gst_buffer_get_meta(b, api) : nullptr;
}

static void ftIfaceInit(gpointer g, gpointer) {
    NemoGstVideoTextureClass *iface = static_cast<NemoGstVideoTextureClass *>(g);
    iface->acquire_frame = ftAcquire;
    iface->bind_frame = ftBind;
    iface->unbind_frame = ftUnbind;
    iface->release_frame = ftRelease;
    iface->get_frame_meta = ftMeta;
}

static void ftSetProperty(GObject *o, guint id, const GValue *v, GParamSpec *) {
    if (id == 1) {
        reinterpret_cast<FakeTextureSink *>(o)->display = g_value_get_pointer(v);
        ++textureCounters[kDisplaySet];
    }
}
static void ftGetProperty(GObject *o, guint id, GValue *v, GParamSpec *) {
    if (id == 1) g_value_set_pointer(v, reinterpret_cast<FakeTextureSink *>(o)->display);
}
static void ftClassInit(gpointer klass, gpointer) {
    GObjectClass *oc = G_OBJECT_CLASS(klass);
    oc->set_property = ftSetProperty;
    oc->get_property = ftGetProperty;
    g_object_class_override_property(oc, 1, "egl-display");
}
static void ftInit(GTypeInstance *instance, gpointer) {
    FakeTextureSink *s = reinterpret_cast<FakeTextureSink *>(instance);
    s->acquireOk = TRUE;
    s->bindOk = TRUE;
}

static GType fakeTextureSinkType() {
    static GType type = 0;
    if (!type) {
        GTypeInfo info;
        std::memset(&info, 0, sizeof(info));
        info.class_size = sizeof(FakeTextureSinkClass);
        info.class_init = ftClassInit;
        info.instance_size = sizeof(FakeTextureSink);
        info.instance_init = ftInit;
        type = g_type_register_static(GST_TYPE_ELEMENT, "SailTubeFakeTextureSink", &info, GTypeFlags(0));
        GInterfaceInfo iface;
        std::memset(&iface, 0, sizeof(iface));
        iface.interface_init = ftIfaceInit;
        g_type_add_interface_static(type, NEMO_GST_TYPE_VIDEO_TEXTURE, &iface);
    }
    return type;
}

// ---- Fake buffer sink (show-frame + EGL-image memory) ---------------------------------

struct FakeBufferSink { GstElement parent; };
struct FakeBufferSinkClass { GstElementClass parent_class; };
static guint showFrameSignal = 0, invalidatedSignal = 0;

static void fbClassInit(gpointer klass, gpointer) {
    const GType t = G_TYPE_FROM_CLASS(klass);
    showFrameSignal = g_signal_new("show-frame", t, G_SIGNAL_RUN_LAST, 0, nullptr, nullptr, nullptr, G_TYPE_NONE, 1,
                                   GST_TYPE_BUFFER);
    invalidatedSignal =
        g_signal_new("buffers-invalidated", t, G_SIGNAL_RUN_LAST, 0, nullptr, nullptr, nullptr, G_TYPE_NONE, 0);
}

static GType fakeBufferSinkType() {
    static GType type = 0;
    if (!type) {
        GTypeInfo info;
        std::memset(&info, 0, sizeof(info));
        info.class_size = sizeof(FakeBufferSinkClass);
        info.class_init = fbClassInit;
        info.instance_size = sizeof(FakeBufferSink);
        type = g_type_register_static(GST_TYPE_ELEMENT, "SailTubeFakeBufferSink", &info, GTypeFlags(0));
    }
    return type;
}

static std::atomic<int> imagesCreated(0);
static EGLImageKHR fakeCreateImage(GstMemory *, EGLDisplay, EGLContext) {
    return reinterpret_cast<EGLImageKHR>(static_cast<quintptr>(0x10000 + ++imagesCreated));
}
static void faFree(GstAllocator *, GstMemory *mem) { g_free(mem); }
static void faClassInit(gpointer klass, gpointer) { GST_ALLOCATOR_CLASS(klass)->free = faFree; }
static void faInit(GTypeInstance *instance, gpointer) {
    reinterpret_cast<NemoGstEglImageAllocator *>(instance)->create_image = fakeCreateImage;
}
static GType fakeAllocatorType() {
    static GType type = 0;
    if (!type) {
        GTypeInfo info;
        std::memset(&info, 0, sizeof(info));
        info.class_size = sizeof(NemoGstEglImageAllocatorClass);
        info.class_init = faClassInit;
        info.instance_size = sizeof(NemoGstEglImageAllocator);
        info.instance_init = faInit;
        type = g_type_register_static(NEMO_GST_TYPE_EGL_IMAGE_ALLOCATOR, "SailTubeFakeEglAllocator", &info, GTypeFlags(0));
    }
    return type;
}

static GstBuffer *eglBuffer(GstAllocator *allocator, int width, int height, int cropHeight) {
    GstMemory *mem = static_cast<GstMemory *>(g_malloc0(sizeof(GstMemory)));
    gst_memory_init(mem, GstMemoryFlags(0), allocator, nullptr, 16, 0, 0, 16);
    GstBuffer *b = gst_buffer_new();
    gst_buffer_append_memory(b, mem);
    gst_buffer_add_video_meta(b, GST_VIDEO_FRAME_FLAG_NONE, GST_VIDEO_FORMAT_NV12, guint(width), guint(height));
    GstVideoCropMeta *crop = gst_buffer_add_video_crop_meta(b);
    crop->x = 0; crop->y = 0; crop->width = guint(width); crop->height = guint(cropHeight);
    return b;
}

// ---- Helpers ------------------------------------------------------------------------------

struct Counts {
    std::atomic<int> announced{0}, geometry{0}, failed{0};
};

static EglSinkBridge::Callbacks callbacksFor(Counts *c, VideoFrameMailbox *box) {
    EglSinkBridge::Callbacks cb;
    cb.frameAnnounced = [c, box]() { ++c->announced; if (box) box->notifyListeners(); };
    cb.geometryChanged = [c]() { ++c->geometry; };
    cb.renderFailed = [c]() { ++c->failed; };
    return cb;
}

static void *const kDisplay = reinterpret_cast<void *>(0x1234);
static void *const kOtherDisplay = reinterpret_cast<void *>(0x5678);

static bool draw(EglSinkBridge &b, void *display, HardwareFrame *f) {
    const bool ok = b.beginFrame(display, f);
    b.endFrame();  // always paired
    return ok;
}

static void frameReady(GstElement *sink, int frame) {
    nemo_gst_video_texture_frame_ready(NEMO_GST_VIDEO_TEXTURE(sink), frame);
}

// ---- Tests --------------------------------------------------------------------------------

static void ply20_sinkWithoutInterfaceIsRefused() {
    GstElement *fake = gst_element_factory_make("fakesink", nullptr);
    std::string why;
    Counts c;
    CHECK(!EglSinkBridge::create(fake, callbacksFor(&c, nullptr), &why));
    CHECK(!why.empty());
    CHECK(!EglSinkBridge::create(nullptr, callbacksFor(&c, nullptr), &why));
    gst_object_unref(fake);
}

static void ply20_textureModeAcquireBindRelease() {
    for (int i = 0; i < kCounters; ++i) textureCounters[i] = 0;
    GstElement *sink = static_cast<GstElement *>(g_object_new(fakeTextureSinkType(), nullptr));
    gst_object_ref_sink(sink);
    FakeTextureSink *fs = reinterpret_cast<FakeTextureSink *>(sink);
    VideoFrameMailbox box;
    std::atomic<int> listener(0);
    box.addListener([&]() { ++listener; });
    Counts c;
    std::shared_ptr<EglSinkBridge> b = EglSinkBridge::create(sink, callbacksFor(&c, &box));
    CHECK(b && b->mode() == EglSinkBridge::Mode::Texture);
    HardwareFrame f;
    // Nothing announced: nothing drawn, nothing acquired.
    CHECK(b->frameSerial() == 0);
    CHECK(!draw(*b, kDisplay, &f));
    CHECK(textureCounters[kAcquire] == 0);
    // The sink announces a frame: the signal only records it and tells the listeners.
    frameReady(sink, 0);
    CHECK(c.announced == 1 && listener == 1);
    CHECK(b->frameSerial() == 1);
    CHECK(textureCounters[kAcquire] == 0);  // no sink call from the signal
    // First draw: display pushed, frame acquired and bound, unbound at endFrame.
    CHECK(draw(*b, kDisplay, &f));
    CHECK(f.eglImage == kImage && f.serial == 1);
    CHECK(fs->display == kDisplay && textureCounters[kDisplaySet] == 1);
    CHECK(textureCounters[kAcquire] == 1 && textureCounters[kBind] == 1 && textureCounters[kUnbind] == 1);
    CHECK(textureCounters[kRelease] == 0);  // kept for repaints
    // Repaint without a new frame: re-bind the held frame, no new acquire, display not re-pushed.
    CHECK(draw(*b, kDisplay, &f));
    CHECK(textureCounters[kAcquire] == 1 && textureCounters[kBind] == 2 && textureCounters[kDisplaySet] == 1);
    // A new frame: the old one is released, the new one acquired.
    frameReady(sink, 1);
    CHECK(draw(*b, kDisplay, &f));
    CHECK(f.serial == 2);
    CHECK(textureCounters[kRelease] == 1 && textureCounters[kAcquire] == 2);
    // PLY-19: context loss re-pushes the display on the next draw.
    b->contextLost();
    CHECK(draw(*b, kOtherDisplay, &f));
    CHECK(fs->display == kOtherDisplay && textureCounters[kDisplaySet] == 2);
    // Crop meta: 1920x1088 caps, 1080 visible lines.
    GstBuffer *meta = gst_buffer_new();
    GstVideoCropMeta *crop = gst_buffer_add_video_crop_meta(meta);
    crop->x = 0; crop->y = 0; crop->width = 1920; crop->height = 1080;
    fs->metaBuffer = meta;
    b->setCaps(1920, 1088, 1.0);
    CHECK(b->geometry().height == 1088);
    const int geometryBefore = c.geometry;
    CHECK(draw(*b, kOtherDisplay, &f));
    CHECK(std::fabs(f.cropHeight - 1080.0 / 1088.0) < 1e-9 && f.cropWidth == 1.0);
    CHECK(b->geometry().width == 1920 && b->geometry().height == 1080);
    CHECK(c.geometry == geometryBefore + 1);  // the surface re-lays out
    fs->metaBuffer = nullptr;
    gst_buffer_unref(meta);
    // "No frame" (frame < 0): the held frame goes back, nothing is drawn.
    const int releases = textureCounters[kRelease];
    frameReady(sink, -1);
    CHECK(b->frameSerial() == 0);
    CHECK(!draw(*b, kOtherDisplay, &f));
    CHECK(textureCounters[kRelease] == releases + 1);
    // Teardown: stopRendering gives the frame back; nothing is drawn afterwards.
    frameReady(sink, 2);
    CHECK(draw(*b, kOtherDisplay, &f));
    b->stopRendering();
    CHECK(textureCounters[kRelease] == releases + 2);
    CHECK(!draw(*b, kOtherDisplay, &f));
    b->detachSink();
    const int announced = c.announced;
    frameReady(sink, 3);  // handler disconnected
    CHECK(c.announced == announced);
    CHECK(GST_OBJECT_REFCOUNT_VALUE(sink) == 1);  // the bridge let go of the sink
    b.reset();
    gst_object_unref(sink);
}

static void ply06_outputDisabledHoldsFramesBack() {
    GstElement *sink = static_cast<GstElement *>(g_object_new(fakeTextureSinkType(), nullptr));
    gst_object_ref_sink(sink);
    Counts c;
    std::shared_ptr<EglSinkBridge> b = EglSinkBridge::create(sink, callbacksFor(&c, nullptr));
    HardwareFrame f;
    CHECK(b->setOutputEnabled(false) == false);  // nothing recorded yet
    CHECK(b->setOutputEnabled(true) == false);
    b->setOutputEnabled(false);
    frameReady(sink, 0);                          // the preroll before the barrier seek
    CHECK(c.announced == 0);                      // no first frame, no surface update
    CHECK(b->frameSerial() == 0);
    CHECK(!draw(*b, kDisplay, &f));
    CHECK(b->framesAnnounced() == 1);
    CHECK(b->setOutputEnabled(true) == true);     // engine posts the first frame now
    CHECK(b->frameSerial() > 0);
    CHECK(draw(*b, kDisplay, &f));
    b->stopRendering();
    b->detachSink();
    gst_object_unref(sink);
}

static void ply20_renderFailureReportedOnce() {
    GstElement *sink = static_cast<GstElement *>(g_object_new(fakeTextureSinkType(), nullptr));
    gst_object_ref_sink(sink);
    reinterpret_cast<FakeTextureSink *>(sink)->bindOk = FALSE;
    Counts c;
    std::shared_ptr<EglSinkBridge> b = EglSinkBridge::create(sink, callbacksFor(&c, nullptr));
    HardwareFrame f;
    frameReady(sink, 0);
    for (int i = 0; i < 10; ++i) CHECK(!draw(*b, kDisplay, &f));
    CHECK(c.failed == 1);                         // never a silent black picture
    CHECK(!b->failureReason().empty());
    b->reportRenderFailure("again");
    CHECK(c.failed == 1);
    b->stopRendering();
    b->detachSink();
    // A failure reported after teardown reaches nobody.
    std::shared_ptr<EglSinkBridge> b2 = EglSinkBridge::create(sink, callbacksFor(&c, nullptr));
    b2->detachSink();
    b2->reportRenderFailure("late");
    CHECK(c.failed == 1);
    gst_object_unref(sink);
}

static void ply20_signalsNeverBlockOnTheDraw() {
    // A streaming thread announcing frames while the render thread draws:
    // no deadlock, every acquired frame is released by the end.
    for (int i = 0; i < kCounters; ++i) textureCounters[i] = 0;
    GstElement *sink = static_cast<GstElement *>(g_object_new(fakeTextureSinkType(), nullptr));
    gst_object_ref_sink(sink);
    Counts c;
    std::shared_ptr<EglSinkBridge> b = EglSinkBridge::create(sink, callbacksFor(&c, nullptr));
    std::atomic<bool> done(false);
    std::thread producer([&]() {
        for (int i = 0; i < 3000; ++i) frameReady(sink, i);
        done = true;
    });
    int drawn = 0;
    HardwareFrame f;
    while (!done || drawn < 10) {
        if (draw(*b, kDisplay, &f)) ++drawn;
    }
    producer.join();
    b->stopRendering();
    b->detachSink();
    CHECK(c.announced == 3000);
    CHECK(drawn >= 10);
    CHECK(textureCounters[kAcquire] == textureCounters[kRelease]);
    CHECK(textureCounters[kBind] == textureCounters[kUnbind]);
    gst_object_unref(sink);
}

static void ply20_bufferModeCachesOneImagePerMemory() {
    GstElement *sink = static_cast<GstElement *>(g_object_new(fakeBufferSinkType(), nullptr));
    gst_object_ref_sink(sink);
    GstAllocator *allocator = static_cast<GstAllocator *>(g_object_new(fakeAllocatorType(), nullptr));
    gst_object_ref_sink(allocator);
    Counts c;
    std::shared_ptr<EglSinkBridge> b = EglSinkBridge::create(sink, callbacksFor(&c, nullptr));
    CHECK(b && b->mode() == EglSinkBridge::Mode::Buffer);
    GstBuffer *one = eglBuffer(allocator, 1920, 1088, 1080);
    GstBuffer *two = eglBuffer(allocator, 1920, 1088, 1080);
    HardwareFrame f;
    imagesCreated = 0;
    g_signal_emit(sink, showFrameSignal, 0, one);
    CHECK(GST_MINI_OBJECT_REFCOUNT_VALUE(one) == 2);  // held until drawn
    CHECK(draw(*b, kDisplay, &f));
    CHECK(imagesCreated == 1 && f.eglImage != nullptr);
    CHECK(std::fabs(f.cropHeight - 1080.0 / 1088.0) < 1e-9);  // normalised by the video meta
    void *imageOne = f.eglImage;
    CHECK(draw(*b, kDisplay, &f));                    // repaint: cached
    CHECK(imagesCreated == 1 && f.eglImage == imageOne);
    g_signal_emit(sink, showFrameSignal, 0, two);
    CHECK(draw(*b, kDisplay, &f));
    CHECK(imagesCreated == 2 && f.eglImage != imageOne);
    CHECK(GST_MINI_OBJECT_REFCOUNT_VALUE(one) == 1);  // the previous frame went back
    g_signal_emit(sink, showFrameSignal, 0, one);     // pool buffer comes round again
    CHECK(draw(*b, kDisplay, &f));
    CHECK(imagesCreated == 2 && f.eglImage == imageOne);
    // PLY-19: context loss -> images rebuilt lazily.
    b->contextLost();
    CHECK(draw(*b, kDisplay, &f));
    CHECK(imagesCreated == 3);
    // buffers-invalidated -> the cache is dropped too.
    g_signal_emit(sink, invalidatedSignal, 0);
    CHECK(draw(*b, kDisplay, &f));
    CHECK(imagesCreated == 4);
    // Another display -> new images.
    CHECK(draw(*b, kOtherDisplay, &f));
    CHECK(imagesCreated == 5);
    // A superseded, never drawn buffer is released at once.
    g_signal_emit(sink, showFrameSignal, 0, two);
    g_signal_emit(sink, showFrameSignal, 0, one);
    CHECK(GST_MINI_OBJECT_REFCOUNT_VALUE(two) == 1);
    // show-frame(NULL): nothing to draw.
    g_signal_emit(sink, showFrameSignal, 0, static_cast<GstBuffer *>(nullptr));
    CHECK(!draw(*b, kOtherDisplay, &f));
    CHECK(b->frameSerial() == 0);
    b->stopRendering();
    b->detachSink();
    CHECK(GST_MINI_OBJECT_REFCOUNT_VALUE(one) == 1 && GST_MINI_OBJECT_REFCOUNT_VALUE(two) == 1);
    gst_buffer_unref(one);  // frees the memory and its cached image (qdata)
    gst_buffer_unref(two);
    gst_object_unref(allocator);
    gst_object_unref(sink);
}

static void ply20_mailboxCarriesTheSource() {
    VideoFrameMailbox box;
    std::atomic<int> calls(0);
    box.addListener([&]() { ++calls; });
    GstElement *sink = static_cast<GstElement *>(g_object_new(fakeTextureSinkType(), nullptr));
    gst_object_ref_sink(sink);
    Counts c;
    std::shared_ptr<EglSinkBridge> b = EglSinkBridge::create(sink, callbacksFor(&c, &box));
    box.setHardwareSource(b);
    CHECK(calls == 1 && box.hardwareSource() == b);
    box.setHardwareSource(b);                    // unchanged: no notice
    CHECK(calls == 1);
    frameReady(sink, 0);
    CHECK(calls == 2);
    CHECK(!box.latest().valid());                // software frames untouched
    b->stopRendering();
    b->detachSink();
    box.setHardwareSource(std::shared_ptr<HardwareFrameSource>());
    CHECK(calls == 3 && !box.hardwareSource());
    gst_object_unref(sink);
}

int main(int argc, char **argv) {
    gst_init(&argc, &argv);
    ply20_sinkWithoutInterfaceIsRefused();
    ply20_textureModeAcquireBindRelease();
    ply06_outputDisabledHoldsFramesBack();
    ply20_renderFailureReportedOnce();
    ply20_signalsNeverBlockOnTheDraw();
    ply20_bufferModeCachesOneImagePerMemory();
    ply20_mailboxCarriesTheSource();
    std::printf("player hw tests: %s (%d checks, %d failures)\n", failures ? "FAIL" : "PASS", checks, failures);
    return failures ? 1 : 0;
}
