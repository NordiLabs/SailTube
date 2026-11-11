// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#pragma once

// glibc allocator calls for MemoryTrimPolicy (Core/Player/Domain/MemoryTrimPolicy.h). No Qt.
// Both are no-ops where glibc is absent (e.g. the Mac host).

namespace nordtile { namespace player {

// mallopt(M_ARENA_MAX, kMallocArenaMax) unless MALLOC_ARENA_MAX is already set or the policy is
// off. First thing in main(), before any thread is created. True when it was applied.
bool limitMallocArenas();

// malloc_trim(0). Resident size in kB (from /proc/self/statm, 0 when unreadable) before and after,
// and the time malloc_trim(0) took in ms (monotonic clock). Returns true when the call ran.
// Owner thread only, never on a frame path.
bool trimMalloc(long *beforeKb, long *afterKb, double *elapsedMs = nullptr);

} } // namespace nordtile::player
