// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "Core/Player/Infrastructure/MemoryTrim.h"
#include "Core/Player/Domain/MemoryTrimPolicy.h"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#ifdef __GLIBC__
#include <malloc.h>
#include <unistd.h>
#endif

namespace nordtile { namespace player {

bool limitMallocArenas() {
#ifdef __GLIBC__
    if (!kMemoryTrimEnabled || std::getenv("MALLOC_ARENA_MAX")) return false;
    return mallopt(M_ARENA_MAX, kMallocArenaMax) == 1;
#else
    return false;
#endif
}

#ifdef __GLIBC__
static long residentKb() {
    std::FILE *f = std::fopen("/proc/self/statm", "r");
    if (!f) return 0;
    long size = 0, resident = 0;
    const int n = std::fscanf(f, "%ld %ld", &size, &resident);
    std::fclose(f);
    if (n != 2) return 0;
    const long page = sysconf(_SC_PAGESIZE);
    return page > 0 ? resident * (page / 1024) : 0;
}
#endif

bool trimMalloc(long *beforeKb, long *afterKb, double *elapsedMs) {
    if (elapsedMs) *elapsedMs = 0;
    if (beforeKb) *beforeKb = 0;
    if (afterKb) *afterKb = 0;
#ifdef __GLIBC__
    if (!kMemoryTrimEnabled) return false;
    if (beforeKb) *beforeKb = residentKb();
    const std::chrono::steady_clock::time_point t0 = std::chrono::steady_clock::now();
    malloc_trim(0);
    if (elapsedMs)
        *elapsedMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    if (afterKb) *afterKb = residentKb();
    return true;
#else
    return false;
#endif
}

} } // namespace nordtile::player
