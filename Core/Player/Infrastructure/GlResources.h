// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#pragma once

// GL resource lifetime for the video renderers (PLY-19, PLT-07), shared by
// the software texture and the hardware (EGLImage) node.
//
// The rule: at paint time a resource holder asks "do I still hold resources,
// and do they belong to the current context?". It never trusts a pointer to
// the old context (a dead context's QPointer is already null, and a new
// context can reuse its address) nor a name's own state (a shader program can
// still report itself linked after its context died). Names of a dead context
// are dropped without GL calls. Names released while their (live) context is
// not current go to an orphan list and are deleted later, in their own
// context, on the render thread.
//
// Qt Gui only; no SailTube types.

#include <QOpenGLContext>
#include <QPointer>
#include <qopengl.h>

namespace nordtile { namespace player {

enum class GlResourceAction {
    NoContext,         // nothing is current: do no GL work this paint
    Keep,              // held names belong to the current context
    Create,            // nothing held: create in the current context
    AbandonAndCreate,  // owner context died: forget the names (no GL calls), recreate
    OrphanAndCreate    // owner alive but not current: orphan the names to it, recreate
};

// PLY-19, pure (unit-tested with fake context ids). `ownerAlive` must come
// from a weak reference (QPointer), never from comparing the stale pointer:
// a dead owner whose address equals `current` is still AbandonAndCreate.
GlResourceAction glResourceAction(bool holdsResources, bool ownerAlive, const void *owner, const void *current);

enum class GlNameKind { Texture, Framebuffer, Program };

// PLY-19: queue `name` for deletion in `owner` (dropped if `owner` is gone).
void orphanGlName(QOpenGLContext *owner, GlNameKind kind, GLuint name);
// PLY-19: delete the orphans of `current` (call with it current, render
// thread) and forget those whose context died.
void purgeGlOrphans(QOpenGLContext *current);
// Diagnostics / tests: orphans still queued.
int glOrphanCount();

// PLY-19: delete `name` now when `owner` is current, otherwise orphan it.
void releaseGlName(QOpenGLContext *owner, GlNameKind kind, GLuint name);

}}
