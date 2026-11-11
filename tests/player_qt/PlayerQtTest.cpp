// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Overlay controller (PLY-17) in real time, frame mailbox hand-over, the
// surface layout helpers (PLY-14/15 inputs), the hardware node choice
// (PLY-20), the GL resource rules (PLY-19), the cover video item (reduced-rate
// second listener of the same mailbox) and the surfaces' `active` switch (Shorts pager pages on
// one mailbox: only active ones take frames, an inactive one keeps its last picture). Real GL rendering is
// device-only (SAILTUBE_REQUIREMENTS §6 D3, D7). Build: tests/player_qt/run.sh
#include "Core/Player/Infrastructure/CoverVideoItem.h"
#include "Core/Player/Infrastructure/GlResources.h"
#include "Core/Player/Infrastructure/HardwareVideoNode.h"
#include "Core/Player/Infrastructure/OverlayController.h"
#include "Core/Player/Infrastructure/VideoFrameMailbox.h"
#include "Core/Player/Infrastructure/VideoSurfaceItem.h"

#include <QGuiApplication>
#include <QOpenGLContext>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QTimer>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <thread>
#include <vector>

using namespace nordtile::player;

static int checks = 0, failures = 0;
#define CHECK(c) do { ++checks; if (!(c)) { std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); ++failures; } } while (0)

static void spin(int ms) {
    QEventLoop loop;
    QTimer::singleShot(ms, &loop, SLOT(quit()));
    loop.exec();
}

static void ply17_doubleTapBurstIsOneFlush() {
    OverlayController c;
    std::vector<double> seeks;
    QObject::connect(&c, &OverlayController::seekRequested, [&](double ms) { seeks.push_back(ms); });
    const bool shown = c.controlsVisible();
    // Four quick taps on the right half: one burst, one seek, controls not toggled.
    for (int i = 0; i < 4; ++i) { c.tap(900, 1000, 60000, 600000); spin(60); }
    CHECK(c.gestureActive());
    CHECK(c.gestureSeconds() > 0);
    spin(400);
    CHECK(seeks.size() == 1);
    CHECK(!seeks.empty() && seeks[0] > 60000 && std::fmod(seeks[0] - 60000, 10000.0) == 0);
    CHECK(c.controlsVisible() == shown);
    spin(800);
    CHECK(!c.gestureActive());
    CHECK(seeks.size() == 1);
}

static void ply17_leftSideSeeksBack() {
    OverlayController c;
    c.setStepSeconds(5);
    CHECK(c.stepSeconds() == 5);
    std::vector<double> seeks;
    QObject::connect(&c, &OverlayController::seekRequested, [&](double ms) { seeks.push_back(ms); });
    c.tap(100, 1000, 60000, 600000); spin(50); c.tap(100, 1000, 60000, 600000);
    CHECK(c.gestureSide() == -1);
    spin(500);
    CHECK(seeks.size() == 1 && seeks[0] == 55000);
}

static void ply17_singleTapTogglesAfterWindow() {
    OverlayController c;
    int seeks = 0;
    QObject::connect(&c, &OverlayController::seekRequested, [&](double) { ++seeks; });
    const bool before = c.controlsVisible();
    c.tap(500, 1000, 0, 100000);
    spin(100);
    CHECK(c.controlsVisible() == before); // still inside the 250 ms window
    spin(300);
    CHECK(c.controlsVisible() != before);
    CHECK(seeks == 0);
}

static void ply17_autoHideWhilePlaying() {
    OverlayController c;
    c.setPlaying(true);
    c.interact();
    CHECK(c.controlsVisible());
    spin(3000);
    CHECK(c.controlsVisible());
    spin(800);
    CHECK(!c.controlsVisible());
    c.setPlaying(false); // pause shows again and stays
    CHECK(c.controlsVisible());
    spin(3800);
    CHECK(c.controlsVisible());
}

static void mailboxHandOver() {
    VideoFrameMailbox box;
    std::atomic<int> calls(0);
    const int id = box.addListener([&]() { ++calls; });
    std::atomic<int> released(0);
    {
        VideoFrame f;
        static unsigned char pixels[4 * 4 * 4];
        f.keepAlive = std::shared_ptr<const void>(pixels, [&](const void *) { ++released; });
        f.data = pixels; f.width = 4; f.height = 4; f.stride = 16; f.pixelAspect = 2.0;
        box.post(f);
    }
    CHECK(calls == 1);
    CHECK(box.latest().valid());
    CHECK(box.latest().displaySize() == QSize(8, 4));
    CHECK(released == 0);       // the mailbox still holds the frame
    box.clear();
    CHECK(released == 1);       // released once nobody holds it
    CHECK(!box.latest().valid());
    box.removeListener(id);
    // Posting from another thread after removal never calls the listener.
    std::thread t([&]() { VideoFrame f; box.post(f); });
    t.join();
    CHECK(calls == 2);
}

static void surfaceLayout() {
    const QRectF bounds(0, 0, 1080, 608);
    const QRectF fit = VideoSurfaceItem::targetRect(bounds, QSizeF(1920, 1080), VideoSurfaceItem::PreserveAspectFit);
    CHECK(std::fabs(fit.width() - 1080) < 1 && std::fabs(fit.height() - 607.5) < 1);
    // A vertical video letterboxes left and right.
    const QRectF tall = VideoSurfaceItem::targetRect(bounds, QSizeF(1080, 1920), VideoSurfaceItem::PreserveAspectFit);
    CHECK(tall.height() == 608 && tall.x() > 0 && std::fabs(tall.center().x() - 540) < 1);
    const QRectF crop = VideoSurfaceItem::sourceRect(bounds, QSizeF(1080, 1920), VideoSurfaceItem::PreserveAspectCrop);
    CHECK(crop.width() == 1 && crop.height() < 1 && crop.y() > 0);
    CHECK(VideoSurfaceItem::targetRect(bounds, QSizeF(), 0).isEmpty());
}

// ---- PLY-20: hardware source -> surface ----------------------------------------

class FakeHardwareSource final : public HardwareFrameSource {
public:
    quint64 serial = 0;
    HardwareVideoGeometry geom;
    quint64 frameSerial() const override { return serial; }
    HardwareVideoGeometry geometry() const override { return geom; }
    bool beginFrame(void *, HardwareFrame *) override { return false; }
    void endFrame() override {}
    void contextLost() override {}
    void reportRenderFailure(const char *) override {}
};

class FakeProvider final : public QObject, public VideoFrameProvider {
    Q_OBJECT
    Q_INTERFACES(nordtile::player::VideoFrameProvider)
public:
    VideoFrameMailbox box;
    VideoFrameMailbox *frameMailbox() override { return &box; }
};

static void ply20_surfaceChoosesHardwareNodeOnlyWithHardwareSource() {
    typedef VideoSurfaceItem S;
    CHECK(S::chooseNode(false, 0, false) == S::NoNode);
    CHECK(S::chooseNode(false, 0, true) == S::SoftwareNode);
    CHECK(S::chooseNode(false, 7, true) == S::SoftwareNode);   // a serial alone is not a source
    CHECK(S::chooseNode(true, 0, false) == S::NoNode);         // no hardware frame yet: black, never a stale frame
    CHECK(S::chooseNode(true, 0, true) == S::NoNode);
    CHECK(S::chooseNode(true, 3, false) == S::HardwareNode);
    CHECK(S::chooseNode(true, 3, true) == S::HardwareNode);    // software frames are ignored in hardware mode
}

static void ply20_surfaceFollowsHardwareSource() {
    FakeProvider provider;
    VideoSurfaceItem item;
    item.setSize(QSizeF(1080, 608));
    item.setSource(&provider);
    CHECK(!item.hardwareRendering());
    CHECK(!item.hasFrame());
    // A software frame (4x4, par 2).
    VideoFrame f;
    static unsigned char pixels[4 * 4 * 4];
    f.keepAlive = std::shared_ptr<const void>(pixels, [](const void *) {});
    f.data = pixels; f.width = 4; f.height = 4; f.stride = 16; f.pixelAspect = 2.0;
    provider.box.post(f);
    spin(20);
    CHECK(item.videoSize() == QSize(8, 4));
    // Hardware build: the source is attached before any frame.
    std::shared_ptr<FakeHardwareSource> hw(new FakeHardwareSource);
    hw->geom.width = 1920; hw->geom.height = 1080;
    provider.box.setHardwareSource(hw);
    spin(20);
    CHECK(item.hardwareRendering());
    CHECK(!item.hasFrame());             // nothing announced yet
    hw->serial = 1;
    provider.box.notifyListeners();      // what the sink bridge does on frame-ready
    spin(20);
    CHECK(item.hasFrame());
    CHECK(item.videoSize() == QSize(1920, 1080));
    CHECK(std::fabs(item.videoAspect() - 16.0 / 9.0) < 1e-6);
    CHECK(std::fabs(item.contentRect().width() - 1080) < 1);
    // A software frame posted meanwhile does not change the hardware picture.
    provider.box.post(f);
    spin(20);
    CHECK(item.videoSize() == QSize(1920, 1080));
    // Pixel aspect from the caps.
    hw->geom.pixelAspect = 4.0 / 3.0; hw->geom.width = 1440;
    provider.box.notifyListeners();
    spin(20);
    CHECK(item.videoSize() == QSize(1920, 1080));
    // Fallback to software: the hardware source goes away.
    provider.box.setHardwareSource(std::shared_ptr<HardwareFrameSource>());
    spin(20);
    CHECK(!item.hardwareRendering());
    CHECK(item.videoSize() == QSize(8, 4));
    CHECK(hw.use_count() == 1);          // the mailbox let go
}

// ---- Cover video ---------------------------------------------------------------

class CountingProvider final : public QObject, public VideoFrameProvider {
    Q_OBJECT
    Q_INTERFACES(nordtile::player::VideoFrameProvider)
public:
    VideoFrameMailbox box;
    int coverFrames = 0;
    VideoFrameMailbox *frameMailbox() override { return &box; }
    Q_INVOKABLE void noteCoverFrameDrawn() { ++coverFrames; }
};

static VideoFrame coverFrame(int w, int h) {
    static unsigned char pixels[64 * 64 * 4];
    VideoFrame f;
    f.keepAlive = std::shared_ptr<const void>(pixels, [](const void *) {});
    f.data = pixels; f.width = w; f.height = h; f.stride = w * 4;
    return f;
}

// Posts `count` frames at ~60 fps from another thread (the decoder's), spinning the GUI loop.
static void pushFrames(VideoFrameMailbox *box, int count, int w, int h) {
    std::atomic<bool> done(false);
    std::thread t([&]() {
        for (int i = 0; i < count; ++i) {
            box->post(coverFrame(w, h));
            std::this_thread::sleep_for(std::chrono::microseconds(16667));
        }
        done = true;
    });
    while (!done) { QCoreApplication::processEvents(QEventLoop::AllEvents, 5); std::this_thread::sleep_for(std::chrono::milliseconds(1)); }
    t.join();
    spin(120);  // the last waiting frame, if any
}

static void cover_frameGateDelay() {
    typedef VideoSurfaceItem S;
    CHECK(S::frameGateDelayMs(0, -1, 67) == 0);    // nothing presented yet: now
    CHECK(S::frameGateDelayMs(10, 0, 67) == 57);
    CHECK(S::frameGateDelayMs(66, 0, 67) == 1);
    CHECK(S::frameGateDelayMs(67, 0, 67) == 0);
    CHECK(S::frameGateDelayMs(500, 0, 67) == 0);
    CHECK(S::frameGateDelayMs(5, 10, 67) == 0);    // clock went back: never stall
    CHECK(S::frameGateDelayMs(10, 0, 0) == 0);     // the watch page: every frame
    CHECK(CoverVideoItem::kFrameIntervalMs * 15 >= 1000);
}

// 60 fps in, at most ~15 per second drawn while active; the counter reaches the source.
static void cover_drawsAtMost15PerSecondWhileActive() {
    CountingProvider provider;
    CoverVideoItem item;
    CHECK(!item.active() && item.fillMode() == VideoSurfaceItem::PreserveAspectFit);
    item.setSize(QSizeF(400, 200));
    item.setSource(&provider);
    item.setActive(true);
    QElapsedTimer t;
    t.start();
    pushFrames(&provider.box, 60, 16, 9);
    const double seconds = t.elapsed() / 1000.0;
    CHECK(provider.box.serial() == 60);            // the item never posts: one producer
    CHECK(item.hasFrame());
    CHECK(item.framesDrawn() >= 8);
    CHECK(item.framesDrawn() <= 16 * seconds + 1);
    std::printf("cover: %d frames drawn of 60 in %.2f s\n", item.framesDrawn(), seconds);
    CHECK(provider.coverFrames == item.framesDrawn());
    // Idle: nothing more is drawn (no periodic timer).
    const int drawn = item.framesDrawn();
    spin(300);
    CHECK(item.framesDrawn() == drawn);
    // Hardware frame source: the same gate.
    std::shared_ptr<FakeHardwareSource> hw(new FakeHardwareSource);
    hw->geom.width = 1920; hw->geom.height = 1080;
    provider.box.setHardwareSource(hw);
    spin(100);
    const int before = item.framesDrawn();
    t.restart();
    while (t.elapsed() < 1000) {
        ++hw->serial;
        provider.box.notifyListeners();
        spin(16);
    }
    spin(120);
    CHECK(item.hardwareRendering());
    CHECK(item.framesDrawn() - before >= 8 && item.framesDrawn() - before <= 17);
    provider.box.setHardwareSource(std::shared_ptr<HardwareFrameSource>());
}

// Inactive: no frames taken, the last picture (and its size) kept; active again: the newest at once.
static void cover_inactiveKeepsLastFrame() {
    CountingProvider provider;
    CoverVideoItem item;
    item.setSize(QSizeF(400, 200));
    item.setSource(&provider);
    item.setActive(true);
    provider.box.post(coverFrame(16, 9));
    spin(30);
    CHECK(item.framesDrawn() == 1 && item.videoSize() == QSize(16, 9));
    int activeChanges = 0;
    QObject::connect(&item, &CoverVideoItem::activeChanged, [&activeChanges]() { ++activeChanges; });
    item.setActive(false);
    CHECK(activeChanges == 1);
    pushFrames(&provider.box, 30, 8, 8);           // a different size meanwhile
    CHECK(item.framesDrawn() == 1);                // 0 uploads while inactive
    CHECK(item.hasFrame() && item.videoSize() == QSize(16, 9));
    provider.box.clear();
    spin(30);
    CHECK(item.hasFrame() && item.videoSize() == QSize(16, 9));
    item.repaint();                                // a PLY-09 repaint takes nothing new either
    spin(30);
    CHECK(item.framesDrawn() == 1);
    provider.box.post(coverFrame(8, 8));
    item.setActive(true);                          // the newest frame at once, no wait
    CHECK(item.framesDrawn() == 2 && item.videoSize() == QSize(8, 8));
    CHECK(provider.coverFrames == 2);
}

// ux deadlock guard: active before any frame (hasFrame false) attaches at once and draws the
// first frame a source pushes later.
static void cover_activeBeforeFirstFrameDrawsIt() {
    CountingProvider provider;
    CoverVideoItem item;
    item.setSize(QSizeF(400, 200));
    item.setActive(true);
    item.setSource(&provider);
    CHECK(!item.hasFrame() && item.framesDrawn() == 0);
    spin(200);
    CHECK(!item.hasFrame());
    provider.box.post(coverFrame(16, 9));
    spin(30);
    CHECK(item.hasFrame() && item.framesDrawn() > 0);
    // Also when the source is set first and activation comes before the frames.
    CountingProvider late;
    CoverVideoItem second;
    second.setSource(&late);
    second.setActive(true);
    CHECK(!second.hasFrame());
    pushFrames(&late.box, 5, 16, 9);
    CHECK(second.hasFrame() && second.framesDrawn() > 0);
}

// Letterbox through the shared targetRect; the watch page's surface on the same mailbox still
// sees every frame (the cover is a second listener, never a producer).
static void cover_letterboxAndSharedMailbox() {
    CountingProvider provider;
    VideoSurfaceItem page;
    CoverVideoItem cover;
    page.setSize(QSizeF(1080, 608));
    cover.setSize(QSizeF(400, 200));
    page.setSource(&provider);
    cover.setSource(&provider);
    cover.setActive(true);
    provider.box.post(coverFrame(16, 9));
    spin(30);
    const QRectF r = cover.contentRect();
    CHECK(r == VideoSurfaceItem::targetRect(QRectF(0, 0, 400, 200), QSizeF(16, 9), VideoSurfaceItem::PreserveAspectFit));
    CHECK(std::fabs(r.height() - 200) < 1e-9 && std::fabs(r.width() - 200 * 16.0 / 9.0) < 1e-6 && r.x() > 0);
    cover.setFillMode(VideoSurfaceItem::PreserveAspectCrop);
    CHECK(cover.contentRect() == QRectF(0, 0, 400, 200));
    int pageChanges = 0;
    QObject::connect(&page, &VideoSurfaceItem::frameChanged, [&pageChanges]() { ++pageChanges; });
    int sizes[] = {8, 16, 8, 16, 8, 16};
    for (int i = 0; i < 6; ++i) { provider.box.post(coverFrame(sizes[i], sizes[i])); spin(5); }
    spin(30);
    CHECK(pageChanges == 6);                       // every frame on the watch page
    CHECK(cover.framesDrawn() < 1 + 6);            // gated on the cover
    CHECK(provider.box.serial() == 7);
}

static void ply20_texCoordsCoverCropAndFill() {
    HardwareFrame frame;
    float c[8];
    HardwareVideoNode::texCoords(frame, QRectF(0, 0, 1, 1), false, c);
    // BL, BR, TL, TR of the FBO; FBO row 0 carries the picture's top (t = 0).
    const float whole[8] = {0, 0, 1, 0, 0, 1, 1, 1};
    bool same = true;
    for (int i = 0; i < 8; ++i) same = same && std::fabs(c[i] - whole[i]) < 1e-6f;
    CHECK(same);
    HardwareVideoNode::texCoords(frame, QRectF(0, 0, 1, 1), true, c);
    CHECK(c[1] == 1 && c[5] == 0);       // flipped
    // 1920x1088 buffer with a 1080-line crop: the padding rows are never sampled.
    frame.cropHeight = 1080.0 / 1088.0;
    HardwareVideoNode::texCoords(frame, QRectF(0, 0, 1, 1), false, c);
    CHECK(std::fabs(c[5] - 1080.0f / 1088.0f) < 1e-6f && c[1] == 0);
    // PreserveAspectCrop of a vertical video inside the crop.
    frame = HardwareFrame();
    frame.cropX = 0.1; frame.cropWidth = 0.8;
    HardwareVideoNode::texCoords(frame, QRectF(0, 0.25, 1, 0.5), false, c);
    CHECK(std::fabs(c[0] - 0.1f) < 1e-6f && std::fabs(c[2] - 0.9f) < 1e-6f);
    CHECK(std::fabs(c[1] - 0.25f) < 1e-6f && std::fabs(c[5] - 0.75f) < 1e-6f);
}

// ---- PLY-19: GL resource rules --------------------------------------------------

static void ply19_resourceActionKeysOffHeldResources() {
    int a = 0, b = 0;  // fake context ids
    const void *ctxA = &a, *ctxB = &b;
    CHECK(glResourceAction(false, false, nullptr, nullptr) == GlResourceAction::NoContext);
    CHECK(glResourceAction(true, true, ctxA, nullptr) == GlResourceAction::NoContext);
    CHECK(glResourceAction(false, false, nullptr, ctxA) == GlResourceAction::Create);
    CHECK(glResourceAction(true, true, ctxA, ctxA) == GlResourceAction::Keep);
    // Display off/on: the scene graph got a new context.
    CHECK(glResourceAction(true, true, ctxA, ctxB) == GlResourceAction::OrphanAndCreate);
    CHECK(glResourceAction(true, false, ctxA, ctxB) == GlResourceAction::AbandonAndCreate);
    // The new context reuses the dead one's address: still rebuilt, no GL deletes.
    CHECK(glResourceAction(true, false, ctxA, ctxA) == GlResourceAction::AbandonAndCreate);
    // Nothing held (e.g. nodes deleted with the old graph): just create.
    CHECK(glResourceAction(false, false, ctxA, ctxB) == GlResourceAction::Create);
}

static void ply19_orphansOfDeadContextsAreDropped() {
    const int base = glOrphanCount();
    orphanGlName(nullptr, GlNameKind::Texture, 5);  // owner already gone: dropped
    CHECK(glOrphanCount() == base);
    QOpenGLContext *alive = new QOpenGLContext;     // never made current: no GL calls happen
    QOpenGLContext *dying = new QOpenGLContext;
    QOpenGLContext other;
    orphanGlName(alive, GlNameKind::Texture, 11);
    orphanGlName(dying, GlNameKind::Framebuffer, 12);
    orphanGlName(dying, GlNameKind::Program, 13);
    orphanGlName(alive, GlNameKind::Texture, 0);    // name 0 is nothing
    CHECK(glOrphanCount() == base + 3);
    delete dying;
    purgeGlOrphans(&other);  // another context: deletes nothing, forgets the dead one's names
    CHECK(glOrphanCount() == base + 1);
    purgeGlOrphans(nullptr);
    CHECK(glOrphanCount() == base + 1);
    delete alive;
    purgeGlOrphans(&other);
    CHECK(glOrphanCount() == base);
}

// ---- Shorts pager: VideoSurfaceItem.active ------------------------------------------------------

// A surface that counts the frames it handed to the scene graph.
class CountingSurface : public VideoSurfaceItem {
public:
    int presented = 0;
protected:
    void framePresented() override { ++presented; }
};

// Two pager pages on the player's one mailbox: the active one takes every frame; after the swipe
// the previous page (active: false) keeps its last picture while the previous Short is stopped
// (mailbox cleared) and the next Short's frames go to the new active page only. Default active
// true; CoverVideoItem keeps its own default (false) and behaviour.
static void shorts_inactiveSurfaceKeepsFrameWhileActiveOneUpdates() {
    CountingProvider provider;
    CountingSurface previous, next;
    CHECK(previous.active() && next.active());          // the watch page's default: unchanged
    CHECK(!CoverVideoItem().active());
    previous.setSize(QSizeF(540, 960));
    next.setSize(QSizeF(540, 960));
    previous.setSource(&provider);
    next.setSource(&provider);
    int activeChanges = 0;
    QObject::connect(&next, &VideoSurfaceItem::activeChanged, [&activeChanges]() { ++activeChanges; });
    CHECK(next.setProperty("active", false));            // the QML property; the page off screen
    CHECK(!next.active() && activeChanges == 1);
    next.setActive(false);                               // same value: no signal
    CHECK(activeChanges == 1);
    provider.box.post(coverFrame(9, 16));                 // the playing Short
    spin(30);
    CHECK(previous.presented == 1 && previous.videoSize() == QSize(9, 16));
    CHECK(next.presented == 0 && !next.hasFrame());
    // The swipe: the previous page goes inactive, the next one active (it shows what the mailbox
    // holds at once: the previous Short's frame until open() of the next one stops it).
    previous.setActive(false);
    next.setActive(true);
    CHECK(activeChanges == 2 && next.presented == 1);
    provider.box.clear();
    spin(30);
    CHECK(previous.hasFrame() && previous.videoSize() == QSize(9, 16));
    CHECK(!next.hasFrame());
    // The next Short's frames: only the active page takes them.
    int previousChanges = 0, nextChanges = 0;
    QObject::connect(&previous, &VideoSurfaceItem::frameChanged, [&previousChanges]() { ++previousChanges; });
    QObject::connect(&next, &VideoSurfaceItem::frameChanged, [&nextChanges]() { ++nextChanges; });
    const int sizes[] = {8, 12, 8, 12, 8, 12};
    for (int i = 0; i < 6; ++i) { provider.box.post(coverFrame(sizes[i], sizes[i] * 16 / 9)); spin(5); }
    spin(30);
    if (next.presented != 7 || nextChanges != 6)
        std::printf("FAIL next page: %d presented, %d size changes, %dx%d\n", next.presented, nextChanges,
                    next.videoSize().width(), next.videoSize().height());
    CHECK(next.presented == 1 + 6 && nextChanges == 6 && next.videoSize() == QSize(12, 21));
    CHECK(previous.presented == 1 && previousChanges == 0 && previous.videoSize() == QSize(9, 16));
    previous.repaint();                                   // a PLY-09 repaint takes nothing new either
    spin(30);
    CHECK(previous.presented == 1 && previous.videoSize() == QSize(9, 16));
    CHECK(provider.coverFrames == 0);                     // plain surfaces are not cover frames
    // Swiping back: active again, the newest frame at once.
    previous.setActive(true);
    CHECK(previous.presented == 2 && previous.videoSize() == QSize(12, 21));
}

int main(int argc, char **argv) {
    QGuiApplication app(argc, argv);
    ply17_doubleTapBurstIsOneFlush();
    ply17_leftSideSeeksBack();
    ply17_singleTapTogglesAfterWindow();
    ply17_autoHideWhilePlaying();
    mailboxHandOver();
    surfaceLayout();
    ply20_surfaceChoosesHardwareNodeOnlyWithHardwareSource();
    ply20_surfaceFollowsHardwareSource();
    ply20_texCoordsCoverCropAndFill();
    ply19_resourceActionKeysOffHeldResources();
    ply19_orphansOfDeadContextsAreDropped();
    cover_frameGateDelay();
    cover_drawsAtMost15PerSecondWhileActive();
    cover_inactiveKeepsLastFrame();
    cover_activeBeforeFirstFrameDrawsIt();
    cover_letterboxAndSharedMailbox();
    shorts_inactiveSurfaceKeepsFrameWhileActiveOneUpdates();
    std::printf("player qt tests: %s (%d checks, %d failures)\n", failures ? "FAIL" : "PASS", checks, failures);
    return failures ? 1 : 0;
}

#include "PlayerQtTest.moc"
