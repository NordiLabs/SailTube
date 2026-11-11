// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#pragma once

// PLY-26 / PLY-24 resume-memory decisions, shared by streams and local files.
// Header-only, Qt-free. The store itself (expiry, entry cap, save cadence
// bookkeeping) belongs to the library component (LIB-05); this header only decides
// numbers.
//
// Thresholds follow LIB-05 and LIB-04 (the spec rows with acceptance tests),
// not FinTube's import rule:
// - a saved point under 10 s, or within 15 s of the end, is stored as "none"
//   (kNoResume), so a finished or barely started video starts from 0;
// - watched = played 80 % or more, or within 15 s of the end (sticky);
// - at end of stream the stored position is the duration (PLY-24: the
//   position is unreliable there), and the resume point is cleared.

#include <cstdint>

namespace nordtile { namespace player {

const std::int64_t kNoResume = -1;
const std::int64_t kResumeMinMs = 10000;       // LIB-05: under 10 s -> none
const std::int64_t kResumeEndMarginMs = 15000; // LIB-05: within 15 s of the end -> none
const std::int64_t kResumeSaveIntervalMs = 30000; // LIB-05: save every 30 s while playing
const int kResumeAskAutoMs = 5000;             // LIB-05: Ask overlay auto-resumes after 5 s

// SET-01 "Resume behaviour", default Always.
enum class ResumeBehaviour { Always, Ask, Never };
const ResumeBehaviour kResumeBehaviourDefault = ResumeBehaviour::Always;

// LIB-05: the value to store for a position (on the 30 s tick, leave, stop).
// durationMs <= 0 (unknown, live) disables the end rule.
inline std::int64_t resumePointToStore(std::int64_t positionMs, std::int64_t durationMs) {
    if (positionMs < kResumeMinMs) return kNoResume;
    if (durationMs > 0 && positionMs >= durationMs - kResumeEndMarginMs) return kNoResume;
    return positionMs;
}

// LIB-05: periodic save while the owner plays.
inline bool resumeSaveDue(std::int64_t nowMs, std::int64_t lastSaveMs) {
    return lastSaveMs < 0 || nowMs - lastSaveMs >= kResumeSaveIntervalMs;
}

struct ResumeStart {
    std::int64_t startMs = 0;  // where playback starts (Always / link / Never)
    bool ask = false;          // Ask: show "resume at mm:ss" / "start over"
    std::int64_t offerMs = 0;  // Ask: the point offered; auto-resume after kResumeAskAutoMs
};

// LIB-05 / PLY-26: where to start. linkStartMs >= 0 (a `t=` link, LNK-02)
// wins over any stored point. A stored point is re-validated against the
// duration in case it was saved under different rules.
inline ResumeStart decideResumeStart(std::int64_t savedMs, std::int64_t durationMs, std::int64_t linkStartMs,
                                     ResumeBehaviour behaviour) {
    ResumeStart r;
    if (linkStartMs >= 0) { r.startMs = linkStartMs; return r; }
    std::int64_t point = savedMs < 0 ? kNoResume : resumePointToStore(savedMs, durationMs);
    if (point == kNoResume || behaviour == ResumeBehaviour::Never) return r;
    if (behaviour == ResumeBehaviour::Ask) { r.ask = true; r.offerMs = point; return r; }
    r.startMs = point;
    return r;
}

// LIB-04: watched = 80 % or more, or within 15 s of the end.
inline bool isWatched(std::int64_t positionMs, std::int64_t durationMs) {
    if (durationMs <= 0) return false;
    if (positionMs * 5 >= durationMs * 4) return true;
    return positionMs >= durationMs - kResumeEndMarginMs;
}

// LIB-04: the flag is sticky; a later partial view never clears it.
inline bool watchedAfter(bool wasWatched, std::int64_t positionMs, std::int64_t durationMs) {
    return wasWatched || isWatched(positionMs, durationMs);
}

// PLY-24: at end of stream record the duration (stream duration, else the
// metadata duration, else the last position).
inline std::int64_t positionToRecordAtEnd(std::int64_t streamDurationMs, std::int64_t metadataDurationMs,
                                          std::int64_t lastPositionMs) {
    if (streamDurationMs > 0) return streamDurationMs;
    if (metadataDurationMs > 0) return metadataDurationMs;
    return lastPositionMs;
}

} } // namespace nordtile::player
