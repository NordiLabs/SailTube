// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "GlResources.h"

#include <QMutex>
#include <QMutexLocker>
#include <QOpenGLFunctions>
#include <vector>

namespace nordtile { namespace player {

namespace {

struct Orphan {
    QPointer<QOpenGLContext> context;
    GlNameKind kind;
    GLuint name;
};

QMutex orphanMutex;
std::vector<Orphan> orphans;

void deleteNow(QOpenGLContext *ctx, GlNameKind kind, GLuint name) {
    QOpenGLFunctions *gl = ctx->functions();
    switch (kind) {
    case GlNameKind::Texture: gl->glDeleteTextures(1, &name); break;
    case GlNameKind::Framebuffer: gl->glDeleteFramebuffers(1, &name); break;
    case GlNameKind::Program: gl->glDeleteProgram(name); break;
    }
}

} // namespace

GlResourceAction glResourceAction(bool holdsResources, bool ownerAlive, const void *owner, const void *current) {
    if (!current) return GlResourceAction::NoContext;
    if (!holdsResources) return GlResourceAction::Create;
    if (!ownerAlive) return GlResourceAction::AbandonAndCreate;
    if (owner != current) return GlResourceAction::OrphanAndCreate;
    return GlResourceAction::Keep;
}

void orphanGlName(QOpenGLContext *owner, GlNameKind kind, GLuint name) {
    if (!owner || !name) return;  // a dead context took its names with it
    QMutexLocker lock(&orphanMutex);
    orphans.push_back(Orphan{owner, kind, name});
}

void purgeGlOrphans(QOpenGLContext *current) {
    if (!current) return;
    std::vector<Orphan> mine;
    {
        QMutexLocker lock(&orphanMutex);
        for (auto it = orphans.begin(); it != orphans.end();) {
            if (!it->context) { it = orphans.erase(it); continue; }
            if (it->context == current) { mine.push_back(*it); it = orphans.erase(it); continue; }
            ++it;
        }
    }
    for (size_t i = 0; i < mine.size(); ++i) deleteNow(current, mine[i].kind, mine[i].name);
}

int glOrphanCount() {
    QMutexLocker lock(&orphanMutex);
    return static_cast<int>(orphans.size());
}

void releaseGlName(QOpenGLContext *owner, GlNameKind kind, GLuint name) {
    if (!owner || !name) return;
    if (QOpenGLContext::currentContext() == owner) deleteNow(owner, kind, name);
    else orphanGlName(owner, kind, name);
}

}}
