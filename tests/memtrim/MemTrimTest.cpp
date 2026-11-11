// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Memory trim policy (Core/Player/Domain/MemoryTrimPolicy.h): the idle trim fires once per idle
// period, never while a pipeline is loading or playing; the teardown trim fires once per teardown
// and never while busy; plus the allocator calls (no-ops without glibc).
#include "Core/Player/Domain/MemoryTrimPolicy.h"
#include "Core/Player/Infrastructure/MemoryTrim.h"

#include <cstdio>
#include <cstdlib>
#include <vector>

using namespace nordtile::player;
static int failures = 0;
#define CHECK(cond) do { if (!(cond)) { std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); ++failures; } } while (0)

int main() {
    CHECK(kMemoryTrimEnabled && kMallocArenaMax == 2 && kTrimIdleMs == 15000);
    CHECK(!kTrimAfterTeardownEnabled);                         // shipped default: teardown trim off
    {   // idle: once, then never until busy again
        MemoryTrimPolicy p;
        CHECK(p.idleTimerWanted());
        CHECK(p.shouldTrimIdle());
        CHECK(!p.shouldTrimIdle() && !p.idleTimerWanted());
        p.setBusy(true);
        CHECK(!p.shouldTrimIdle() && !p.idleTimerWanted());   // playing: never
        p.setBusy(false);
        CHECK(p.idleTimerWanted() && p.shouldTrimIdle());      // paused/idle again: once more
        CHECK(!p.shouldTrimIdle());
    }
    {   // a timer that fires while busy does nothing and does not consume the turn
        MemoryTrimPolicy p;
        p.setBusy(true);
        CHECK(!p.shouldTrimIdle());
        p.setBusy(false);
        CHECK(p.shouldTrimIdle());
    }
    {   // teardown: once per teardown, not while the next pipeline loads
        MemoryTrimPolicy p(true);
        CHECK(!p.shouldTrimAfterTeardown());                   // nothing torn down
        p.teardownDone();
        CHECK(p.shouldTrimAfterTeardown());
        CHECK(!p.shouldTrimAfterTeardown());
        CHECK(!p.shouldTrimIdle());                            // the teardown trim covered this idle period
        p.setBusy(true);
        p.teardownDone();
        CHECK(!p.shouldTrimAfterTeardown());                   // busy: skipped, not left pending
        p.setBusy(false);
        CHECK(!p.shouldTrimAfterTeardown());
        CHECK(p.shouldTrimIdle());                             // the idle timer covers it
    }
    {   // teardown trim off (the default): never fires, the idle trim still does
        MemoryTrimPolicy p(false);
        CHECK(!p.teardownTrimEnabled());
        p.teardownDone();
        CHECK(!p.teardownPending() && !p.shouldTrimAfterTeardown());
        CHECK(p.idleTimerWanted() && p.shouldTrimIdle());
        CHECK(!p.shouldTrimIdle());
        MemoryTrimPolicy d;                                    // default follows the constant
        CHECK(d.teardownTrimEnabled() == (kMemoryTrimEnabled && kTrimAfterTeardownEnabled));
    }
    {   // allocator calls: callable, memory stays valid
        std::vector<void *> blocks;
        for (int i = 0; i < 64; ++i) blocks.push_back(std::malloc(64 * 1024));
        for (size_t i = 0; i < blocks.size(); i += 2) std::free(blocks[i]);
        long before = -1, after = -1;
        double ms = -1;
        const bool ran = trimMalloc(&before, &after, &ms);
        CHECK(ms >= 0 && (ran || ms == 0));
        CHECK(ran || (before == 0 && after == 0));
        for (size_t i = 1; i < blocks.size(); i += 2) std::free(blocks[i]);
        (void)limitMallocArenas();
    }
    std::printf(failures ? "memtrim: FAIL\n" : "memtrim: PASS\n");
    return failures ? 1 : 0;
}
