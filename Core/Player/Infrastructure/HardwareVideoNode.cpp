// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "HardwareVideoNode.h"
#include "GlResources.h"

#include <EGL/egl.h>

#include <QOpenGLContext>
#include <QOpenGLFunctions>
#include <QPointer>
#include <QQuickWindow>
#include <QSGTexture>

#include <cstdio>
#include <cstdlib>
#include <utility>

#ifndef GL_TEXTURE_EXTERNAL_OES
#define GL_TEXTURE_EXTERNAL_OES 0x8D65
#endif

namespace nordtile { namespace player {

namespace {

// Consecutive glEGLImageTargetTexture2DOES errors before giving up (PLY-20).
const int kMaxTargetFailures = 5;
const int kMaxFboSide = 4096;

typedef void (*ImageTargetTexture2DFn)(GLenum target, void *image);

const char *const kVertexShader =
    "attribute highp vec2 position;\n"
    "attribute highp vec2 texcoord;\n"
    "varying mediump vec2 v_texcoord;\n"
    "void main() {\n"
    "    v_texcoord = texcoord;\n"
    "    gl_Position = vec4(position, 0.0, 1.0);\n"
    "}\n";

// External-OES samplers need the extension directive before any statement.
const char *const kFragmentShader =
    "#extension GL_OES_EGL_image_external : require\n"
    "precision mediump float;\n"
    "varying mediump vec2 v_texcoord;\n"
    "uniform samplerExternalOES frame;\n"
    "void main() {\n"
    "    gl_FragColor = vec4(texture2D(frame, v_texcoord).rgb, 1.0);\n"
    "}\n";

bool flipRequested() {
    static const bool flip = std::getenv("SAILTUBE_HW_FLIP") && std::atoi(std::getenv("SAILTUBE_HW_FLIP")) != 0;
    return flip;
}

bool debugEnabled() {
    static const bool on = std::getenv("SAILTUBE_PLAYER_DEBUG") != nullptr;
    return on;
}

} // namespace

// Owns every GL name of the hardware path, all created in one context.
class HardwareVideoRenderer {
public:
    ~HardwareVideoRenderer() { release(); }

    // Draws into the FBO; *texture / *size receive its colour texture.
    bool draw(QQuickWindow *window, HardwareFrameSource *source, const QRectF &sourceRect, const QSize &pixelSize,
              GLuint *texture, QSize *size);

private:
    bool holds() const { return program_ || oesTexture_ || fbo_ || fboTexture_; }
    bool create(QOpenGLContext *ctx, HardwareFrameSource *source);
    bool ensureFbo(QOpenGLFunctions *gl, const QSize &size);
    GLuint compile(QOpenGLFunctions *gl, GLenum type, const char *code);
    void forget();   // drop the names without GL calls (their context died)
    void orphan();   // hand the names back to their (live) context
    void release();  // delete now if our context is current, else orphan

    QPointer<QOpenGLContext> owner_;
    const void *ownerAddress_ = nullptr;  // compared only while owner_ is alive
    GLuint program_ = 0, oesTexture_ = 0, fbo_ = 0, fboTexture_ = 0;
    GLint samplerLoc_ = -1;
    QSize fboSize_;
    ImageTargetTexture2DFn imageTarget_ = nullptr;
    int targetFailures_ = 0;
};

void HardwareVideoRenderer::forget() {
    program_ = oesTexture_ = fbo_ = fboTexture_ = 0;
    fboSize_ = QSize();
    owner_.clear();
    ownerAddress_ = nullptr;
}

void HardwareVideoRenderer::orphan() {
    orphanGlName(owner_, GlNameKind::Program, program_);
    orphanGlName(owner_, GlNameKind::Texture, oesTexture_);
    orphanGlName(owner_, GlNameKind::Framebuffer, fbo_);
    orphanGlName(owner_, GlNameKind::Texture, fboTexture_);
    forget();
}

void HardwareVideoRenderer::release() {
    if (!holds()) return;
    QOpenGLContext *owner = owner_.data();
    if (!owner) { forget(); return; }  // died with its context
    releaseGlName(owner, GlNameKind::Program, program_);
    releaseGlName(owner, GlNameKind::Texture, oesTexture_);
    releaseGlName(owner, GlNameKind::Framebuffer, fbo_);
    releaseGlName(owner, GlNameKind::Texture, fboTexture_);
    forget();
}

GLuint HardwareVideoRenderer::compile(QOpenGLFunctions *gl, GLenum type, const char *code) {
    GLuint shader = gl->glCreateShader(type);
    gl->glShaderSource(shader, 1, &code, nullptr);
    gl->glCompileShader(shader);
    GLint ok = 0;
    gl->glGetShaderiv(shader, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[512] = {0};
        gl->glGetShaderInfoLog(shader, sizeof(log) - 1, nullptr, log);
        std::fprintf(stderr, "player: external-OES shader compile failed: %s\n", log);
        gl->glDeleteShader(shader);
        return 0;
    }
    return shader;
}

bool HardwareVideoRenderer::create(QOpenGLContext *ctx, HardwareFrameSource *source) {
    if (!ctx->hasExtension(QByteArrayLiteral("GL_OES_EGL_image_external"))) {
        source->reportRenderFailure("GL_OES_EGL_image_external is not supported");
        return false;
    }
    imageTarget_ = reinterpret_cast<ImageTargetTexture2DFn>(ctx->getProcAddress("glEGLImageTargetTexture2DOES"));
    if (!imageTarget_) {
        source->reportRenderFailure("glEGLImageTargetTexture2DOES is missing");
        return false;
    }
    QOpenGLFunctions *gl = ctx->functions();
    const GLuint vs = compile(gl, GL_VERTEX_SHADER, kVertexShader);
    const GLuint fs = vs ? compile(gl, GL_FRAGMENT_SHADER, kFragmentShader) : 0;
    if (!vs || !fs) {
        if (vs) gl->glDeleteShader(vs);
        source->reportRenderFailure("the samplerExternalOES shader does not compile");
        return false;
    }
    GLuint program = gl->glCreateProgram();
    gl->glAttachShader(program, vs);  // attached once, at creation
    gl->glAttachShader(program, fs);
    gl->glBindAttribLocation(program, 0, "position");
    gl->glBindAttribLocation(program, 1, "texcoord");
    gl->glLinkProgram(program);
    // Flagged for deletion; they live as long as the program.
    gl->glDeleteShader(vs);
    gl->glDeleteShader(fs);
    GLint linked = 0;
    gl->glGetProgramiv(program, GL_LINK_STATUS, &linked);
    if (!linked) {
        gl->glDeleteProgram(program);
        source->reportRenderFailure("the samplerExternalOES shader does not link");
        return false;
    }
    owner_ = ctx;
    ownerAddress_ = ctx;
    program_ = program;
    samplerLoc_ = gl->glGetUniformLocation(program_, "frame");

    gl->glGenTextures(1, &oesTexture_);
    gl->glBindTexture(GL_TEXTURE_EXTERNAL_OES, oesTexture_);
    // External-OES textures reject mipmap filters (INVALID_ENUM): linear only.
    gl->glTexParameteri(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    gl->glTexParameteri(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    gl->glTexParameteri(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    gl->glTexParameteri(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    gl->glBindTexture(GL_TEXTURE_EXTERNAL_OES, 0);
    targetFailures_ = 0;
    return true;
}

bool HardwareVideoRenderer::ensureFbo(QOpenGLFunctions *gl, const QSize &size) {
    if (fbo_ && fboSize_ == size) return true;
    if (fbo_) gl->glDeleteFramebuffers(1, &fbo_);
    if (fboTexture_) gl->glDeleteTextures(1, &fboTexture_);
    fbo_ = fboTexture_ = 0;
    fboSize_ = QSize();
    gl->glGenTextures(1, &fboTexture_);
    gl->glBindTexture(GL_TEXTURE_2D, fboTexture_);
    gl->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    gl->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    gl->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    gl->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    gl->glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, size.width(), size.height(), 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
    gl->glBindTexture(GL_TEXTURE_2D, 0);
    gl->glGenFramebuffers(1, &fbo_);
    gl->glBindFramebuffer(GL_FRAMEBUFFER, fbo_);
    gl->glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, fboTexture_, 0);
    if (gl->glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
        std::fprintf(stderr, "player: hardware video FBO %dx%d incomplete\n", size.width(), size.height());
        return false;
    }
    fboSize_ = size;
    return true;
}

bool HardwareVideoRenderer::draw(QQuickWindow *window, HardwareFrameSource *source, const QRectF &sourceRect,
                                 const QSize &pixelSize, GLuint *texture, QSize *size) {
    QOpenGLContext *ctx = QOpenGLContext::currentContext();
    purgeGlOrphans(ctx);
    // PLY-19: "do I still hold resources of the current context?"
    switch (glResourceAction(holds(), !owner_.isNull(), ownerAddress_, ctx)) {
    case GlResourceAction::NoContext:
        return false;
    case GlResourceAction::Keep:
        break;
    case GlResourceAction::AbandonAndCreate:
    case GlResourceAction::OrphanAndCreate:
    case GlResourceAction::Create:
        if (holds()) {
            if (owner_) orphan();
            else forget();
            if (debugEnabled()) std::fprintf(stderr, "player: hardware renderer rebuilt for a new GL context\n");
        }
        source->contextLost();  // cached EGLImages / pushed display belong to the old one
        if (!create(ctx, source)) {
            release();
            return false;
        }
        break;
    }

    QOpenGLFunctions *gl = ctx->functions();
    const QSize fboSize(qBound(1, pixelSize.width(), kMaxFboSide), qBound(1, pixelSize.height(), kMaxFboSide));
    HardwareFrame frame;
    // The frame lock is held from here to endFrame(): the whole draw.
    const bool haveFrame = source->beginFrame(eglGetCurrentDisplay(), &frame);
    bool drawn = false;
    if (haveFrame && ensureFbo(gl, fboSize)) {
        gl->glBindFramebuffer(GL_FRAMEBUFFER, fbo_);
        gl->glViewport(0, 0, fboSize.width(), fboSize.height());
        gl->glDisable(GL_BLEND);
        gl->glDisable(GL_DEPTH_TEST);
        gl->glDisable(GL_STENCIL_TEST);
        gl->glDisable(GL_SCISSOR_TEST);
        gl->glDisable(GL_CULL_FACE);
        gl->glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
        gl->glUseProgram(program_);
        gl->glActiveTexture(GL_TEXTURE0);
        gl->glBindTexture(GL_TEXTURE_EXTERNAL_OES, oesTexture_);
        while (gl->glGetError() != GL_NO_ERROR) {}  // only our own errors below
        imageTarget_(GL_TEXTURE_EXTERNAL_OES, frame.eglImage);
        const GLenum error = gl->glGetError();
        if (error == GL_NO_ERROR) {
            targetFailures_ = 0;
            gl->glUniform1i(samplerLoc_, 0);
            static const GLfloat positions[8] = {-1, -1, 1, -1, -1, 1, 1, 1};
            GLfloat coords[8];
            HardwareVideoNode::texCoords(frame, sourceRect, flipRequested(), coords);
            gl->glBindBuffer(GL_ARRAY_BUFFER, 0);
            gl->glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, positions);
            gl->glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 0, coords);
            gl->glEnableVertexAttribArray(0);
            gl->glEnableVertexAttribArray(1);
            gl->glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
            gl->glDisableVertexAttribArray(0);
            gl->glDisableVertexAttribArray(1);
            drawn = true;
        } else {
            if (debugEnabled()) std::fprintf(stderr, "player: glEGLImageTargetTexture2DOES error 0x%x\n", error);
            if (++targetFailures_ >= kMaxTargetFailures)
                source->reportRenderFailure("glEGLImageTargetTexture2DOES rejects the sink's EGLImage");
        }
        gl->glBindTexture(GL_TEXTURE_EXTERNAL_OES, 0);
        gl->glUseProgram(0);
    }
    source->endFrame();
    // Give the scene graph the state it expects (default FBO, no program...).
    window->resetOpenGLState();
    *texture = fboTexture_;
    *size = fboSize_;
    return drawn;
}

// ---- Node ------------------------------------------------------------------------------

HardwareVideoNode::HardwareVideoNode() : renderer_(new HardwareVideoRenderer) {
    setOwnsTexture(false);  // texture_ owns the wrapper; the renderer owns the GL name
    setFiltering(QSGTexture::Linear);
}

HardwareVideoNode::~HardwareVideoNode() {
    renderer_.reset();  // GL names: deleted here if our context is current, else orphaned
}

bool HardwareVideoNode::render(QQuickWindow *window, const std::shared_ptr<HardwareFrameSource> &source,
                               const QRectF &target, const QRectF &sourceRect, const QSize &pixelSize) {
    if (!window || !source) return false;
    if (source != source_) {
        source_ = source;
        hasPicture_ = false;
    }
    GLuint texture = 0;
    QSize size;
    const bool drawn = renderer_->draw(window, source.get(), sourceRect, pixelSize, &texture, &size);
    if (!texture || !size.isValid()) {
        hasPicture_ = false;
        return false;
    }
    if (drawn) hasPicture_ = true;
    if (texture != shownTexture_ || size != shownSize_) {
        // A new FBO (first draw, resize, new context): wrap its texture. The
        // renderer keeps owning the GL name.
        std::unique_ptr<QSGTexture> wrapper(window->createTextureFromId(texture, size));
        setTexture(wrapper.get());
        texture_ = std::move(wrapper);  // the previous wrapper goes after setTexture()
        shownTexture_ = texture;
        shownSize_ = size;
        if (!drawn) hasPicture_ = false;
    }
    setRect(target);
    setSourceRect(QRectF(0, 0, size.width(), size.height()));
    markDirty(QSGNode::DirtyMaterial);
    return drawn;
}

void HardwareVideoNode::texCoords(const HardwareFrame &frame, const QRectF &sourceRect, bool flip, float out[8]) {
    const double u0 = frame.cropX + sourceRect.left() * frame.cropWidth;
    const double u1 = frame.cropX + sourceRect.right() * frame.cropWidth;
    double top = frame.cropY + sourceRect.top() * frame.cropHeight;
    double bottom = frame.cropY + sourceRect.bottom() * frame.cropHeight;
    if (flip) std::swap(top, bottom);
    // Strip order: FBO bottom-left, bottom-right, top-left, top-right. FBO
    // row 0 (bottom in GL) takes the picture's top row.
    const double coords[8] = {u0, top, u1, top, u0, bottom, u1, bottom};
    for (int i = 0; i < 8; ++i) out[i] = static_cast<float>(coords[i]);
}

}}
