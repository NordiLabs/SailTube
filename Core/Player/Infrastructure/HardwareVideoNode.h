// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#pragma once

// PLY-20 zero-copy render node. On the render thread, inside
// VideoSurfaceItem::updatePaintNode (GL context current, GUI thread blocked),
// it takes the HardwareFrameSource's frame lock, binds the current frame's
// EGLImage to a GL_TEXTURE_EXTERNAL_OES texture (glEGLImageTargetTexture2DOES,
// GL_OES_EGL_image_external checked at runtime, GL_LINEAR filters, no
// mipmaps), draws it through a samplerExternalOES shader into an FBO the size
// of the on-screen picture and releases the lock. The scene graph then shows
// the FBO's texture like any other, so overlays and clipping keep working and
// the whole draw happens under the frame lock.
//
// PLY-19: every draw first asks glResourceAction() whether the held program,
// textures and FBO belong to the current context; names of a dead context
// are dropped without GL calls, names of a live other context are orphaned to
// it (GlResources.h), the source is told (contextLost) and everything is
// rebuilt lazily.
//
// Qt Quick + GLES + EGL; no GStreamer, no SailTube types.

#include "HardwareFrameSource.h"

#include <QRectF>
#include <QSGSimpleTextureNode>
#include <QSize>

#include <memory>

class QQuickWindow;
class QSGTexture;

namespace nordtile { namespace player {

class HardwareVideoRenderer;

class HardwareVideoNode final : public QSGSimpleTextureNode {
public:
    HardwareVideoNode();
    ~HardwareVideoNode() override;

    // Render thread, GL context current. Draws the source's newest frame
    // (normalised `sourceRect` of the picture, from fill mode) into an FBO of
    // `pixelSize` and shows it at `target` (item coordinates). False when
    // nothing could be drawn this time.
    bool render(QQuickWindow *window, const std::shared_ptr<HardwareFrameSource> &source, const QRectF &target,
                const QRectF &sourceRect, const QSize &pixelSize);
    // A picture was drawn into the current FBO (it survives a failed draw).
    bool hasPicture() const { return hasPicture_; }

    // PLY-20 helper, pure (unit-tested): texture coordinates of the four
    // triangle-strip corners (bottom-left, bottom-right, top-left, top-right
    // of the FBO) for `sourceRect` inside the frame's crop. FBO row 0 carries
    // the picture's top row, so the scene graph (texture row 0 = top) shows
    // it upright; `flip` mirrors vertically (device diagnostics).
    static void texCoords(const HardwareFrame &frame, const QRectF &sourceRect, bool flip, float out[8]);

private:
    std::unique_ptr<HardwareVideoRenderer> renderer_;
    std::shared_ptr<HardwareFrameSource> source_;  // keeps the source alive while drawn
    std::unique_ptr<QSGTexture> texture_;          // wrapper of the FBO texture
    unsigned int shownTexture_ = 0;
    QSize shownSize_;
    bool hasPicture_ = false;
};

}}
