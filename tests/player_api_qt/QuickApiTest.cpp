// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// NFR-TST-06: Qt Quick side of the player API that had no test yet: the
// surface's fill mode, the overlay's gesture target and cancel, GL name
// release and the hardware node's no-context path. GUI thread only; the
// minimal platform has no GL, so the hardware node is checked for doing no
// GL work and taking no frame lock without a current context (real drawing
// is device-only, see tests/player_hw). Build: tests/player_api_qt/run.sh
#include "Core/Player/Infrastructure/GlResources.h"
#include "Core/Player/Infrastructure/HardwareFrameSource.h"
#include "Core/Player/Infrastructure/HardwareVideoNode.h"
#include "Core/Player/Infrastructure/OverlayController.h"
#include "Core/Player/Infrastructure/VideoFrameMailbox.h"
#include "Core/Player/Infrastructure/VideoSurfaceItem.h"

#include <QEventLoop>
#include <QGuiApplication>
#include <QOffscreenSurface>
#include <QOpenGLContext>
#include <QOpenGLFunctions>
#include <QQuickWindow>
#include <QTimer>

#include <cmath>
#include <cstdio>
#include <memory>
#include <vector>

using namespace nordtile::player;

static int checks = 0, failures = 0;
#define CHECK(c) do { ++checks; if (!(c)) { std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); ++failures; } } while (0)

static void spin(int ms) {
    QEventLoop loop;
    QTimer::singleShot(ms, &loop, SLOT(quit()));
    loop.exec();
}

static bool near(const QRectF &a, const QRectF &b) {
    return std::fabs(a.x() - b.x()) < 0.5 && std::fabs(a.y() - b.y()) < 0.5 && std::fabs(a.width() - b.width()) < 0.5 &&
           std::fabs(a.height() - b.height()) < 0.5;
}

class FakeProvider final : public QObject, public VideoFrameProvider {
    Q_OBJECT
    Q_INTERFACES(nordtile::player::VideoFrameProvider)
public:
    VideoFrameMailbox box;
    VideoFrameMailbox *frameMailbox() override { return &box; }
};

class CountingHardwareSource final : public HardwareFrameSource {
public:
    quint64 serial = 1;
    int begins = 0, ends = 0, lost = 0, failuresReported = 0;
    quint64 frameSerial() const override { return serial; }
    HardwareVideoGeometry geometry() const override {
        HardwareVideoGeometry g;
        g.width = 1920; g.height = 1080;
        return g;
    }
    bool beginFrame(void *, HardwareFrame *) override { ++begins; return false; }
    void endFrame() override { ++ends; }
    void contextLost() override { ++lost; }
    void reportRenderFailure(const char *) override { ++failuresReported; }
};

// PLY-15 / PLY-13: setFillMode() emits fillModeChanged only for a real change
// to a valid mode, and contentRect follows targetRect() for the new mode.
static void ply15_setFillModeRelaysOutTheContentRect() {
    FakeProvider provider;
    VideoSurfaceItem item;
    item.setSize(QSizeF(1000, 1000));
    item.setSource(&provider);
    int fillChanges = 0, rectChanges = 0;
    QObject::connect(&item, &VideoSurfaceItem::fillModeChanged, [&]() { ++fillChanges; });
    QObject::connect(&item, &VideoSurfaceItem::contentRectChanged, [&]() { ++rectChanges; });
    // A 16:9 software frame (32x18, square pixels).
    static unsigned char pixels[32 * 18 * 4];
    VideoFrame f;
    f.keepAlive = std::shared_ptr<const void>(pixels, [](const void *) {});
    f.data = pixels; f.width = 32; f.height = 18; f.stride = 32 * 4; f.pixelAspect = 1.0;
    provider.box.post(f);
    spin(20);
    CHECK(item.hasFrame() && item.videoSize() == QSize(32, 18));
    const QRectF bounds(0, 0, 1000, 1000);
    CHECK(item.fillMode() == VideoSurfaceItem::PreserveAspectFit);
    CHECK(near(item.contentRect(), QRectF(0, 218.75, 1000, 562.5)));
    rectChanges = 0;

    item.setFillMode(VideoSurfaceItem::PreserveAspectCrop);
    CHECK(fillChanges == 1 && item.fillMode() == VideoSurfaceItem::PreserveAspectCrop);
    CHECK(rectChanges == 1);
    CHECK(near(item.contentRect(), VideoSurfaceItem::targetRect(bounds, QSizeF(32, 18), VideoSurfaceItem::PreserveAspectCrop)));
    CHECK(near(item.contentRect(), bounds));

    item.setFillMode(VideoSurfaceItem::PreserveAspectCrop);  // same mode: nothing
    CHECK(fillChanges == 1 && rectChanges == 1);

    item.setFillMode(VideoSurfaceItem::Stretch);  // new mode, same rectangle: no contentRectChanged
    CHECK(fillChanges == 2 && item.fillMode() == VideoSurfaceItem::Stretch);
    CHECK(rectChanges == 1 && near(item.contentRect(), bounds));

    item.setFillMode(3);   // out of range: ignored
    item.setFillMode(-1);
    CHECK(fillChanges == 2 && item.fillMode() == VideoSurfaceItem::Stretch);

    item.setFillMode(VideoSurfaceItem::PreserveAspectFit);
    CHECK(fillChanges == 3 && rectChanges == 2);
    CHECK(near(item.contentRect(), VideoSurfaceItem::targetRect(bounds, QSizeF(32, 18), VideoSurfaceItem::PreserveAspectFit)));

    // Through the QML property as well (what the watch page binds).
    CHECK(item.setProperty("fillMode", int(VideoSurfaceItem::PreserveAspectCrop)));
    CHECK(fillChanges == 4 && item.fillMode() == VideoSurfaceItem::PreserveAspectCrop);

    // Without a picture the rectangle is empty whatever the mode.
    VideoSurfaceItem empty;
    empty.setSize(QSizeF(400, 300));
    empty.setFillMode(VideoSurfaceItem::Stretch);
    CHECK(empty.fillMode() == VideoSurfaceItem::Stretch && empty.contentRect().isEmpty());
}

// PLY-17: gestureTargetMs shows where the burst will land; cancelGesture()
// drops the burst without a seek (gear opened, video changed).
static void ply17_cancelGestureDropsTheBurst() {
    OverlayController c;
    std::vector<double> seeks;
    int gestureChanges = 0;
    QObject::connect(&c, &OverlayController::seekRequested, [&](double ms) { seeks.push_back(ms); });
    QObject::connect(&c, &OverlayController::gestureChanged, [&]() { ++gestureChanges; });
    CHECK(c.stepSeconds() == 10);
    // Two quick taps on the right half: a +10 s gesture from 60 s.
    c.tap(900, 1000, 60000, 600000);
    c.tap(900, 1000, 60000, 600000);
    CHECK(c.gestureActive());
    CHECK(c.gestureTargetMs() == 70000);
    CHECK(c.property("gestureTargetMs").toDouble() == 70000);
    c.tap(900, 1000, 60000, 600000);
    CHECK(c.gestureTargetMs() == 80000);       // stacks at once
    CHECK(c.gestureSeconds() == 20);
    const int before = gestureChanges;
    c.cancelGesture();
    CHECK(!c.gestureActive());
    CHECK(gestureChanges == before + 1);
    spin(400);                                 // past the 250 ms flush delay
    CHECK(seeks.empty());                      // cancelled: no seek
    CHECK(!c.gestureActive());
    // Cancelling again (nothing active) emits nothing.
    const int idle = gestureChanges;
    c.cancelGesture();
    CHECK(gestureChanges == idle);
    // The target is clamped to the duration; a later burst flushes normally.
    c.tap(900, 1000, 595000, 600000);
    c.tap(900, 1000, 595000, 600000);
    CHECK(c.gestureTargetMs() == 600000);
    spin(400);
    CHECK(seeks.size() == 1 && seeks[0] == 600000);
    // Left side counts down and is clamped at 0.
    OverlayController back;
    back.tap(100, 1000, 4000, 600000);
    back.tap(100, 1000, 4000, 600000);
    CHECK(back.gestureTargetMs() == 0);
    back.cancelGesture();
}

// PLY-19: releaseGlName() deletes only in the owner's current context;
// otherwise it orphans the name to the owner (deleted later there), and a
// gone owner or name 0 is nothing to do.
static void ply19_releaseGlNameOrphansOffContext() {
    const int base = glOrphanCount();
    releaseGlName(nullptr, GlNameKind::Texture, 7);   // owner gone: its names died with it
    CHECK(glOrphanCount() == base);
    QOpenGLContext *owner = new QOpenGLContext;       // never current here: no GL call may happen
    releaseGlName(owner, GlNameKind::Texture, 0);     // name 0 is nothing
    CHECK(glOrphanCount() == base);
    releaseGlName(owner, GlNameKind::Texture, 21);
    releaseGlName(owner, GlNameKind::Framebuffer, 22);
    releaseGlName(owner, GlNameKind::Program, 23);
    CHECK(glOrphanCount() == base + 3);               // queued for the owner's context
    QOpenGLContext other;
    purgeGlOrphans(&other);                           // another context: keeps them
    CHECK(glOrphanCount() == base + 3);
    delete owner;
    purgeGlOrphans(&other);                           // the owner died: forgotten without GL calls
    CHECK(glOrphanCount() == base);

    // With a real current context (only where the platform offers GL), a
    // release in the owner's own context deletes at once.
    QOffscreenSurface surface;
    surface.create();
    QOpenGLContext ctx;
    if (!ctx.create() || !ctx.makeCurrent(&surface)) {
        std::printf("  NOTE no GL context on this platform: immediate-delete path not exercised\n");
        return;
    }
    QOpenGLFunctions *gl = ctx.functions();
    GLuint tex = 0;
    gl->glGenTextures(1, &tex);
    gl->glBindTexture(GL_TEXTURE_2D, tex);
    CHECK(tex != 0 && gl->glIsTexture(tex));
    releaseGlName(&ctx, GlNameKind::Texture, tex);
    CHECK(glOrphanCount() == base);                   // deleted, not queued
    CHECK(!gl->glIsTexture(tex));
    ctx.doneCurrent();
}

// PLY-20 / PLY-19: render() draws only with a window, a source and a current
// GL context. Without one it returns false, reports no picture, takes no
// frame lock and does no GL work (no names to orphan).
static void ply20_hardwareNodeRendersNothingWithoutContext() {
    CHECK(QOpenGLContext::currentContext() == nullptr);
    HardwareVideoNode node;
    CHECK(!node.hasPicture());
    std::shared_ptr<CountingHardwareSource> source(new CountingHardwareSource);
    const QRectF target(0, 0, 640, 360), whole(0, 0, 1, 1);
    CHECK(!node.render(nullptr, source, target, whole, QSize(640, 360)));   // no window
    CHECK(!node.hasPicture());
    QQuickWindow window;
    CHECK(!node.render(&window, std::shared_ptr<HardwareFrameSource>(), target, whole, QSize(640, 360)));  // no source
    CHECK(!node.hasPicture());
    const int orphansBefore = glOrphanCount();
    CHECK(!node.render(&window, source, target, whole, QSize(640, 360)));   // no current context
    CHECK(!node.hasPicture());
    CHECK(source->begins == 0 && source->ends == 0);  // the frame lock is never taken
    CHECK(source->failuresReported == 0);             // not a render failure: just no context yet
    CHECK(glOrphanCount() == orphansBefore);
    CHECK(source.use_count() == 2);                   // the node keeps the drawn source alive
    // Another source replaces it; the old one is let go.
    std::shared_ptr<CountingHardwareSource> next(new CountingHardwareSource);
    CHECK(!node.render(&window, next, target, whole, QSize(640, 360)));
    CHECK(source.use_count() == 1 && next.use_count() == 2);
    CHECK(!node.hasPicture());
}

int main(int argc, char **argv) {
    QGuiApplication app(argc, argv);
    std::printf("ply15_setFillModeRelaysOutTheContentRect\n");
    ply15_setFillModeRelaysOutTheContentRect();
    std::printf("ply17_cancelGestureDropsTheBurst\n");
    ply17_cancelGestureDropsTheBurst();
    std::printf("ply19_releaseGlNameOrphansOffContext\n");
    ply19_releaseGlNameOrphansOffContext();
    std::printf("ply20_hardwareNodeRendersNothingWithoutContext\n");
    ply20_hardwareNodeRendersNothingWithoutContext();
    std::printf("player api quick tests: %d checks, %d failure%s\n", checks, failures, failures == 1 ? "" : "s");
    return failures ? 1 : 0;
}

#include "QuickApiTest.moc"
