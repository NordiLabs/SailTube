// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#pragma once

// PLY-10 session continuity: one playback session shared by every watch
// context (a watch page instance, identified by a caller-chosen int token).
// Qt-free state machine; the PlaybackSession QObject feeds it page events and
// executes the returned decisions in field order:
//   savePosition(stoppedVideoId) -> stop(stoppedVideoId) -> park / reattach /
//   resolve(videoId, generation).
//
// Ownership: exactly one context, the one currently showing the player, is
// the owner. Only the owner may cause playback side effects (skips, recovery,
// autoplay, position saves, comment auto-load, prefetch, teardown): guard
// each with mayAct(contextId). While the session is parked (no watch page on
// screen, the mini-player / cover / MPRIS drive it) the owner is kAppContext.

#include <cstdint>
#include <string>
#include <vector>

namespace nordtile { namespace player {

const int kNoContext = -1;  // nobody owns the player
const int kAppContext = 0;  // app-level controller while parked; watch pages use ids > 0

// What the player is doing when a watch page leaves.
struct PlaybackSnapshot {
    bool playing = false;
    std::int64_t positionMs = 0;
    PlaybackSnapshot() {}
    PlaybackSnapshot(bool p, std::int64_t pos) : playing(p), positionMs(pos) {}
};

struct SessionDecision {
    bool savePosition = false;   // save the position of stoppedVideoId first
    bool stop = false;           // then stop it and release its stream jobs
    std::string stoppedVideoId;
    bool park = false;           // keep the session running with no watch page
    bool reattach = false;       // adopt the live session: no resolve, no restart
    bool resolve = false;        // resolve videoId; tag the answer with generation
    std::string videoId;
    unsigned generation = 0;
    int owner = kNoContext;      // owner after this decision

    bool any() const { return savePosition || stop || park || reattach || resolve; }
};

class SessionArbiter {
public:
    // PLY-10: "keep the session on leave" setting, on by default.
    void setKeepSessionOnLeave(bool on) { m_keepOnLeave = on; }
    bool keepSessionOnLeave() const { return m_keepOnLeave; }

    // PLY-10: a new watch context is shown for videoId (first show, or the
    // mini-player expanding into a new page). Same video as the live session
    // -> reattach; a different one -> save + stop the old one, resolve the new.
    // Reusing an existing contextId changes that context's video in place.
    SessionDecision open(int contextId, const std::string &videoId);

    // PLY-10: a context already on the stack became the visible page again
    // (A -> B -> back to A). It reclaims: reattach if the session plays its
    // video, otherwise save + stop the current one and resolve its video.
    // No-op for the current owner and for unknown ids, so it is safe to call
    // on every "page became active".
    SessionDecision activate(int contextId);

    // PLY-10: the owner stopped being visible (covered by another page, or
    // about to be removed). Playing, or paused above 0, with the setting on
    // -> park (owner becomes kAppContext); otherwise save + stop.
    // Non-owners: no-op.
    SessionDecision deactivate(int contextId, const PlaybackSnapshot &snapshot);

    // PLY-10: the context is removed from the page stack. deactivate() plus
    // removal; removing a non-owner has no side effects.
    SessionDecision close(int contextId, const PlaybackSnapshot &snapshot);

    // MPR-01 Stop / mini-player close: save, stop, end the session. A watch
    // page that owns the player stays owner (of an empty player).
    SessionDecision stopSession();

    // The session ended by itself (end of stream with no autoplay, fatal
    // failure after Retry was declined). No save: the caller recorded the end.
    void sessionEnded();

    // PLY-18 recovery re-resolve of the current video by its owner: bumps the
    // generation so a late answer of the failed attempt is ignored.
    SessionDecision reresolve(int contextId);

    // PLY-10: resolve answers are tagged by video id + generation. Answers for
    // another id, or an older generation, are stale and must be ignored by the
    // player (the page may still use the metadata).
    bool acceptResolve(const std::string &videoId, unsigned generation) const;

    // PLY-10: only the owner may act. Never true for kNoContext.
    bool mayAct(int contextId) const { return contextId != kNoContext && contextId == m_owner; }

    int owner() const { return m_owner; }
    bool hasSession() const { return m_active; }
    bool parked() const { return m_active && m_owner == kAppContext; }
    const std::string &sessionVideoId() const { return m_video; }
    unsigned generation() const { return m_generation; }
    // Top of the context stack (most recently opened), kNoContext when empty.
    int topContext() const { return m_stack.empty() ? kNoContext : m_stack.back().contextId; }
    std::size_t contextCount() const { return m_stack.size(); }

private:
    struct Entry { int contextId; std::string videoId; };
    Entry *find(int contextId);
    SessionDecision takeOver(int contextId, const std::string &videoId);

    std::vector<Entry> m_stack;
    std::string m_video;
    unsigned m_generation = 0;
    int m_owner = kNoContext;
    bool m_active = false;
    bool m_keepOnLeave = true;
};

} } // namespace nordtile::player
