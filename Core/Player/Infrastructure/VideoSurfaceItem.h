// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#pragma once

// QML item that paints the latest frame of a VideoFrameMailbox.
//   Software decode: one upload per new frame straight from the decoder's
//     mapped buffer into a GL texture; no CPU deep copy (PLY-02).
//   Hardware decode (PLY-20): when the mailbox carries a HardwareFrameSource
//     the item uses a HardwareVideoNode instead (EGLImage -> external-OES
//     texture, zero copy, HardwareVideoNode.h). Software frames are ignored
//     while a hardware source is attached.
//
// Context loss (PLY-19, PLT-07): the texture checks at every bind whether it
// still holds names created in the *current* context. If not (display off/on
// with a non-persistent scene graph, or a new context), the old names are
// dropped without GL calls (they died with their context) and the texture is
// rebuilt lazily. Names released on a thread without a current context are
// deleted later on the render thread, in their own context.
//
// Reduced-rate mode (CoverVideoItem, the live picture in the Sailfish active
// cover): the same mailbox, a second listener (no second decoder, pipeline or
// producer), at most one frame per frame interval (intermediate frames are
// dropped, never queued; one single-shot precise timer runs only while a
// frame waits, never when idle, NFR-PERF-06). The watch page's surface
// presents every frame, as before.
//
// `active` (Shorts pager, cover video): while false the item takes nothing from
// the mailbox (listener detached, no updates, no uploads) and keeps its last
// picture; true attaches again and presents the newest frame at once. Several
// surfaces may share one mailbox (pager pages, watch page, cover); only active
// ones consume frames. Default true (the watch page is unchanged);
// CoverVideoItem starts inactive.
//
// Qt Quick only; no SailTube types, so SailTune can pin it.

#include "VideoFrameMailbox.h"
#include <QPointer>
#include <QQuickItem>
#include <QRectF>
#include <QSize>
#include <QElapsedTimer>
#include <QTimer>

namespace nordtile { namespace player {

class VideoSurfaceItem : public QQuickItem {  // not final: qmlRegisterType derives from it
    Q_OBJECT
    Q_PROPERTY(QObject *source READ source WRITE setSource NOTIFY sourceChanged)
    Q_PROPERTY(int fillMode READ fillMode WRITE setFillMode NOTIFY fillModeChanged)
    Q_PROPERTY(bool hasFrame READ hasFrame NOTIFY frameChanged)
    Q_PROPERTY(QSize videoSize READ videoSize NOTIFY frameChanged)
    // Aspect of the picture (width / height), 0 when unknown. Watch-page layout
    // (vertical-video fullscreen, PLY-13/14) reads this.
    Q_PROPERTY(double videoAspect READ videoAspect NOTIFY frameChanged)
    // Where the picture is drawn, in item coordinates (cutout handling, PLY-15).
    Q_PROPERTY(QRectF contentRect READ contentRect NOTIFY contentRectChanged)
    // PLY-20: frames come from a hardware (zero-copy) source.
    Q_PROPERTY(bool hardwareRendering READ hardwareRendering NOTIFY hardwareRenderingChanged)
    // Shorts pager / cover video: present frames (true) or keep the last picture (false).
    Q_PROPERTY(bool active READ active WRITE setActive NOTIFY activeChanged)
public:
    enum FillMode { PreserveAspectFit = 0, PreserveAspectCrop = 1, Stretch = 2 };
    Q_ENUM(FillMode)
    // Which scene-graph node a paint uses (chooseNode()).
    enum NodeKind { NoNode = 0, SoftwareNode = 1, HardwareNode = 2 };

    explicit VideoSurfaceItem(QQuickItem *parent = nullptr);
    ~VideoSurfaceItem() override;

    QObject *source() const { return source_; }
    void setSource(QObject *source);
    int fillMode() const { return fillMode_; }
    void setFillMode(int mode);
    bool hasFrame() const { return videoSize_.isValid(); }
    QSize videoSize() const { return videoSize_; }
    double videoAspect() const;
    QRectF contentRect() const { return contentRect_; }
    bool hardwareRendering() const { return hardwareRendering_; }
    // Shorts pager / cover video: see `active` above.
    bool active() const { return presenting_; }
    void setActive(bool active);

    // Re-upload the held frame on the (possibly rebuilt) GL context. Called
    // 250 ms after the application becomes active again (PLY-09, PLT-07).
    Q_INVOKABLE void repaint();

    // Pure layout helper, unit-tested: where a picture of `picture` size lands
    // inside `bounds` for a fill mode, and which part of the source is shown.
    static QRectF targetRect(const QRectF &bounds, const QSizeF &picture, int fillMode);
    static QRectF sourceRect(const QRectF &bounds, const QSizeF &picture, int fillMode);
    // PLY-20, pure (unit-tested): the hardware node only with a hardware
    // source that announced a frame; a stale software frame never shows while
    // a hardware source is attached.
    static NodeKind chooseNode(bool hardwareSource, quint64 hardwareSerial, bool softwareFrameValid);
    // Cover video, pure (unit-tested): ms to wait before the next frame may be presented when
    // the last one was presented at `lastPresentedMs` (-1 = none yet); 0 = present now.
    static int frameGateDelayMs(qint64 nowMs, qint64 lastPresentedMs, int intervalMs);

signals:
    void sourceChanged();
    void fillModeChanged();
    void frameChanged();
    void contentRectChanged();
    void hardwareRenderingChanged();
    void activeChanged();

protected:
    // Cover video: present at most one frame every `intervalMs` (0 = every frame).
    void setFrameInterval(int intervalMs);
    int frameInterval() const { return frameIntervalMs_; }
    // `active` without the notification (a subclass's initial state): false = no mailbox
    // listener, no updates, no uploads; the last picture stays (only a scene-graph rebuild,
    // PLY-19, uploads the mailbox's frame again). true = listen again and present the newest
    // frame at once. Default true.
    void setPresenting(bool presenting);
    bool presenting() const { return presenting_; }
    // A frame with a new serial was handed to the scene graph (GUI thread).
    virtual void framePresented() {}

    QSGNode *updatePaintNode(QSGNode *old, UpdatePaintNodeData *) override;
    void geometryChanged(const QRectF &newGeometry, const QRectF &oldGeometry) override;
    void releaseResources() override;

private slots:
    void frameArrived();
    void handleWindowChanged(QQuickWindow *window);
    void sceneGraphInvalidated();

private:
    void attach();
    void detach();
    void listen();
    bool present();  // true: a frame with a new serial went to the scene graph
    void paceTimeout();
    void updateLayout();

    QPointer<QObject> source_;
    VideoFrameMailbox *mailbox_ = nullptr;
    int listener_ = 0;
    int fillMode_ = PreserveAspectFit;
    QSize videoSize_;
    QRectF contentRect_;
    bool hardwareRendering_ = false;
    bool resourcesLost_ = false;
    NodeKind paintedKind_ = NoNode;  // render thread (GUI blocked): kind of the node returned last
    // Cover video pacing (GUI thread).
    bool presenting_ = true;
    int frameIntervalMs_ = 0;
    qint64 lastPresentMs_ = -1;
    quint64 presentedSerial_ = 0;
    QElapsedTimer paceClock_;
    QTimer paceTimer_;               // single shot, only while a frame waits
};

}}
