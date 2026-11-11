// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "VideoSurfaceItem.h"
#include "GlResources.h"
#include "HardwareVideoNode.h"

#include <QMetaObject>
#include <QOpenGLContext>
#include <QOpenGLFunctions>
#include <QQuickWindow>
#include <QSGSimpleTextureNode>
#include <QSGTexture>
#include <QtMath>

#ifndef GL_UNPACK_ROW_LENGTH
#define GL_UNPACK_ROW_LENGTH 0x0CF2
#endif

namespace nordtile { namespace player {

namespace {

class FrameTexture final : public QSGTexture {
public:
    ~FrameTexture() override { release(); }

    void setFrame(const VideoFrame &frame) { frame_ = frame; }
    quint64 frameSerial() const { return frame_.serial; }

    int textureId() const override { ensure(); return static_cast<int>(id_); }
    QSize textureSize() const override { return QSize(frame_.width, frame_.height); }
    bool hasAlphaChannel() const override { return false; }
    bool hasMipmaps() const override { return false; }

    void bind() override {
        ensure();
        QOpenGLContext *ctx = QOpenGLContext::currentContext();
        if (!ctx || !id_) return;
        QOpenGLFunctions *gl = ctx->functions();
        gl->glBindTexture(GL_TEXTURE_2D, id_);
        if (frame_.valid() && uploaded_ != frame_.serial) upload(ctx, gl);
        // External-OES style constraints do not apply here, but keep it simple:
        // linear filtering, no mipmaps, clamp (NPOT on GLES2).
        gl->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        gl->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        gl->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        gl->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    }

private:
    // "Do I still hold resources of the current context?" — never trust a
    // pointer to the old context or a name's own state (PLY-19).
    void ensure() const {
        QOpenGLContext *ctx = QOpenGLContext::currentContext();
        purgeGlOrphans(ctx);
        const GlResourceAction action = glResourceAction(id_ != 0, !context_.isNull(), contextAddress_, ctx);
        if (action == GlResourceAction::NoContext || action == GlResourceAction::Keep) return;
        // Owner alive but not current: hand the name back to it. Owner dead:
        // the name died with it, drop it without GL calls.
        if (action == GlResourceAction::OrphanAndCreate) orphanGlName(context_, GlNameKind::Texture, id_);
        id_ = 0;
        ctx->functions()->glGenTextures(1, &id_);
        context_ = ctx;
        contextAddress_ = ctx;
        uploaded_ = 0;
        uploadedSize_ = QSize();
    }

    void upload(QOpenGLContext *ctx, QOpenGLFunctions *gl) {
        const int w = frame_.width, h = frame_.height, rowBytes = w * 4;
        gl->glPixelStorei(GL_UNPACK_ALIGNMENT, (frame_.stride % 4) == 0 ? 4 : 1);
        const bool sizeChanged = uploadedSize_ != QSize(w, h);
        const bool rowLength = !ctx->isOpenGLES() || ctx->format().majorVersion() >= 3;
        if (frame_.stride == rowBytes || rowLength) {
            if (frame_.stride != rowBytes) gl->glPixelStorei(GL_UNPACK_ROW_LENGTH, frame_.stride / 4);
            if (sizeChanged) gl->glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, frame_.data);
            else gl->glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, frame_.data);
            if (frame_.stride != rowBytes) gl->glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
        } else {
            // GLES2 without row length: allocate once, then row by row.
            if (sizeChanged) gl->glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
            for (int y = 0; y < h; ++y)
                gl->glTexSubImage2D(GL_TEXTURE_2D, 0, 0, y, w, 1, GL_RGBA, GL_UNSIGNED_BYTE,
                                    frame_.data + static_cast<std::ptrdiff_t>(y) * frame_.stride);
        }
        uploaded_ = frame_.serial;
        uploadedSize_ = QSize(w, h);
        // The pixels are on the GPU now; keep only the geometry, so the decoder's
        // buffer goes back to its pool right away.
        frame_.keepAlive.reset();
        frame_.data = nullptr;
    }

    void release() {
        if (!id_) return;
        releaseGlName(context_, GlNameKind::Texture, id_);  // a dead context's name is just dropped
        id_ = 0;
    }

    mutable GLuint id_ = 0;
    mutable QPointer<QOpenGLContext> context_;
    mutable const void *contextAddress_ = nullptr;  // compared only while context_ is alive
    mutable quint64 uploaded_ = 0;
    mutable QSize uploadedSize_;
    VideoFrame frame_;
};

} // namespace

VideoSurfaceItem::VideoSurfaceItem(QQuickItem *parent) : QQuickItem(parent) {
    setFlag(ItemHasContents, true);
    connect(this, &QQuickItem::windowChanged, this, &VideoSurfaceItem::handleWindowChanged);
    paceClock_.start();
    paceTimer_.setSingleShot(true);
    paceTimer_.setTimerType(Qt::PreciseTimer);  // a coarse timer may fire up to 5 % early
    connect(&paceTimer_, &QTimer::timeout, this, &VideoSurfaceItem::paceTimeout);
}

VideoSurfaceItem::~VideoSurfaceItem() { detach(); }

void VideoSurfaceItem::setSource(QObject *source) {
    if (source == source_) return;
    detach();
    source_ = source;
    attach();
    emit sourceChanged();
    frameArrived();  // adopt what the new source already holds (size, hardware mode); updates
}

void VideoSurfaceItem::attach() {
    VideoFrameProvider *provider = source_ ? qobject_cast<VideoFrameProvider *>(source_.data()) : nullptr;
    mailbox_ = provider ? provider->frameMailbox() : nullptr;
    if (!mailbox_) return;
    if (presenting_) listen();
    if (source_)
        connect(source_.data(), &QObject::destroyed, this, [this]() { detach(); mailbox_ = nullptr; frameArrived(); });
}

void VideoSurfaceItem::listen() {
    if (!mailbox_ || listener_) return;
    listener_ = mailbox_->addListener([this]() {
        // Any thread: only post. Removal in detach() waits for a running call.
        QMetaObject::invokeMethod(this, "frameArrived", Qt::QueuedConnection);
    });
}

void VideoSurfaceItem::detach() {
    if (mailbox_ && listener_) mailbox_->removeListener(listener_);
    listener_ = 0;
    if (source_) disconnect(source_.data(), nullptr, this, nullptr);
    mailbox_ = nullptr;
}

void VideoSurfaceItem::setFillMode(int mode) {
    if (mode < PreserveAspectFit || mode > Stretch || mode == fillMode_) return;
    fillMode_ = mode;
    updateLayout();
    emit fillModeChanged();
    update();
}

double VideoSurfaceItem::videoAspect() const {
    return videoSize_.isValid() && videoSize_.height() > 0
        ? static_cast<double>(videoSize_.width()) / videoSize_.height() : 0.0;
}

void VideoSurfaceItem::repaint() { update(); }

void VideoSurfaceItem::setFrameInterval(int intervalMs) {
    frameIntervalMs_ = intervalMs > 0 ? intervalMs : 0;
    if (!frameIntervalMs_ && paceTimer_.isActive()) {
        paceTimer_.stop();
        frameArrived();
    }
}

void VideoSurfaceItem::setActive(bool active) {
    if (active == presenting_) return;
    setPresenting(active);
    emit activeChanged();
}

void VideoSurfaceItem::setPresenting(bool presenting) {
    if (presenting == presenting_) return;
    presenting_ = presenting;
    if (!presenting) {
        // Detached, so the decoder's frames cost this item nothing; the node keeps the picture.
        paceTimer_.stop();
        if (mailbox_ && listener_) mailbox_->removeListener(listener_);
        listener_ = 0;
        return;
    }
    listen();
    lastPresentMs_ = -1;  // the newest frame at once
    frameArrived();
}

int VideoSurfaceItem::frameGateDelayMs(qint64 nowMs, qint64 lastPresentedMs, int intervalMs) {
    if (intervalMs <= 0 || lastPresentedMs < 0) return 0;
    const qint64 since = nowMs - lastPresentedMs;
    if (since < 0 || since >= intervalMs) return 0;  // a clock step back never stalls the picture
    return static_cast<int>(intervalMs - since);
}

void VideoSurfaceItem::frameArrived() {
    if (!presenting_) return;
    if (frameIntervalMs_ > 0) {
        // A frame already waits for the timer: this one replaces it in the mailbox (dropped).
        if (paceTimer_.isActive()) return;
        const qint64 now = paceClock_.elapsed();
        const int wait = frameGateDelayMs(now, lastPresentMs_, frameIntervalMs_);
        if (wait > 0) {
            paceTimer_.start(wait);
            return;
        }
        // Only a new frame starts the interval: a state change (source set, no frame yet)
        // never delays the first picture.
        if (present()) lastPresentMs_ = now;
        return;
    }
    present();
}

void VideoSurfaceItem::paceTimeout() {
    if (!presenting_) return;
    const qint64 now = paceClock_.elapsed();
    if (present()) lastPresentMs_ = now;  // the newest frame the mailbox holds now
}

bool VideoSurfaceItem::present() {
    const std::shared_ptr<HardwareFrameSource> hw = mailbox_ ? mailbox_->hardwareSource() : nullptr;
    QSize size;
    quint64 serial = 0;
    if (hw) {
        serial = hw->frameSerial();
        if (serial) size = hw->geometry().displaySize();
    } else {
        const VideoFrame frame = mailbox_ ? mailbox_->latest() : VideoFrame();
        if (frame.valid()) {
            size = frame.displaySize();
            serial = frame.serial;
        }
    }
    if (bool(hw) != hardwareRendering_) {
        hardwareRendering_ = bool(hw);
        emit hardwareRenderingChanged();
    }
    if (size != videoSize_) {
        videoSize_ = size;
        updateLayout();
        emit frameChanged();
    }
    const bool fresh = serial && serial != presentedSerial_;
    if (fresh) {
        presentedSerial_ = serial;
        framePresented();
    }
    update();
    return fresh;
}

void VideoSurfaceItem::handleWindowChanged(QQuickWindow *window) {
    if (window) connect(window, &QQuickWindow::sceneGraphInvalidated, this, &VideoSurfaceItem::sceneGraphInvalidated,
                        Qt::DirectConnection);
}

void VideoSurfaceItem::sceneGraphInvalidated() {
    // Render thread, context about to go: nodes and textures are deleted by Qt;
    // the next paint rebuilds from the held frame.
    resourcesLost_ = true;
}

void VideoSurfaceItem::releaseResources() { resourcesLost_ = true; }

void VideoSurfaceItem::geometryChanged(const QRectF &newGeometry, const QRectF &oldGeometry) {
    QQuickItem::geometryChanged(newGeometry, oldGeometry);
    updateLayout();
    update();
}

QRectF VideoSurfaceItem::targetRect(const QRectF &bounds, const QSizeF &picture, int fillMode) {
    if (picture.isEmpty() || bounds.isEmpty()) return QRectF();
    if (fillMode == Stretch || fillMode == PreserveAspectCrop) return bounds;
    QSizeF scaled = picture.scaled(bounds.size(), Qt::KeepAspectRatio);
    return QRectF(bounds.x() + (bounds.width() - scaled.width()) / 2,
                  bounds.y() + (bounds.height() - scaled.height()) / 2, scaled.width(), scaled.height());
}

QRectF VideoSurfaceItem::sourceRect(const QRectF &bounds, const QSizeF &picture, int fillMode) {
    if (picture.isEmpty() || bounds.isEmpty() || fillMode != PreserveAspectCrop) return QRectF(0, 0, 1, 1);
    // Normalised part of the picture that covers the bounds.
    const double pictureAspect = picture.width() / picture.height();
    const double boundsAspect = bounds.width() / bounds.height();
    if (pictureAspect > boundsAspect) {
        const double w = boundsAspect / pictureAspect;
        return QRectF((1 - w) / 2, 0, w, 1);
    }
    const double h = pictureAspect / boundsAspect;
    return QRectF(0, (1 - h) / 2, 1, h);
}

void VideoSurfaceItem::updateLayout() {
    const QRectF r = targetRect(QRectF(0, 0, width(), height()), QSizeF(videoSize_), fillMode_);
    if (r != contentRect_) {
        contentRect_ = r;
        emit contentRectChanged();
    }
}

VideoSurfaceItem::NodeKind VideoSurfaceItem::chooseNode(bool hardwareSource, quint64 hardwareSerial,
                                                        bool softwareFrameValid) {
    if (hardwareSource) return hardwareSerial ? HardwareNode : NoNode;
    return softwareFrameValid ? SoftwareNode : NoNode;
}

QSGNode *VideoSurfaceItem::updatePaintNode(QSGNode *old, UpdatePaintNodeData *) {
    if (resourcesLost_) {
        // Qt already deleted the old tree when the scene graph went away; a
        // node passed back here would belong to the new graph.
        resourcesLost_ = false;
    }
    if (!old) paintedKind_ = NoNode;
    // Not presenting (inactive: cover not visible, pager page off screen): the last picture stays
    // as it is; no upload, no hardware draw. Only a rebuilt scene graph (no old node, PLY-19) shows the held frame again.
    if (!presenting_ && old && paintedKind_ != NoNode) return old;
    const std::shared_ptr<HardwareFrameSource> hw = mailbox_ ? mailbox_->hardwareSource() : nullptr;
    const VideoFrame frame = mailbox_ && !hw ? mailbox_->latest() : VideoFrame();
    const NodeKind kind = chooseNode(bool(hw), hw ? hw->frameSerial() : 0, frame.valid());
    if (old && kind != paintedKind_) {
        delete old;  // software <-> hardware switch (fallback, new video)
        old = nullptr;
    }
    paintedKind_ = NoNode;
    // No picture (new video, teardown, no hardware frame yet): show nothing,
    // the page draws black.
    if (kind == NoNode) { delete old; return nullptr; }
    const QRectF bounds(0, 0, width(), height());
    if (kind == HardwareNode) {
        const HardwareVideoGeometry g = hw->geometry();
        const QSizeF picture = g.valid() ? QSizeF(g.displaySize()) : bounds.size();
        const QRectF target = targetRect(bounds, picture, fillMode_);
        if (target.isEmpty() || !window()) { delete old; return nullptr; }
        HardwareVideoNode *node = old ? static_cast<HardwareVideoNode *>(old) : new HardwareVideoNode();
        const qreal dpr = window()->devicePixelRatio();
        const QSize pixels(qCeil(target.width() * dpr), qCeil(target.height() * dpr));
        if (!node->render(window(), hw, target, sourceRect(bounds, picture, fillMode_), pixels) &&
            !node->hasPicture()) {
            delete node;
            return nullptr;
        }
        paintedKind_ = HardwareNode;
        return node;
    }
    QSGSimpleTextureNode *node = static_cast<QSGSimpleTextureNode *>(old);
    const QSizeF picture(frame.displaySize());
    const QRectF target = targetRect(bounds, picture, fillMode_);
    if (target.isEmpty()) { delete node; return nullptr; }
    paintedKind_ = SoftwareNode;
    if (!node) {
        node = new QSGSimpleTextureNode();
        node->setOwnsTexture(true);
        node->setFiltering(QSGTexture::Linear);
        node->setTexture(new FrameTexture());
    }
    FrameTexture *texture = static_cast<FrameTexture *>(node->texture());
    if (frame.serial != texture->frameSerial()) {
        texture->setFrame(frame);
        node->markDirty(QSGNode::DirtyMaterial);
    }
    node->setRect(target);
    // setSourceRect takes texture pixels.
    const QRectF n = sourceRect(bounds, picture, fillMode_);
    node->setSourceRect(QRectF(n.x() * frame.width, n.y() * frame.height, n.width() * frame.width, n.height() * frame.height));
    return node;
}

}}
