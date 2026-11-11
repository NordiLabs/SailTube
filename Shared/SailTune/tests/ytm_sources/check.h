// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Minimal assert harness for the ytm-sources tests (no Qt5Test in the native container).
#pragma once
#include <cstdio>
#include <cstdlib>
#include <string>

static int g_failures = 0;
static int g_checks = 0;
static int g_notes = 0;
#define CHECK(cond) do { ++g_checks; if (!(cond)) { std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); ++g_failures; } } while (0)
#define CHECK_EQ(a, b) do { ++g_checks; if (!((a) == (b))) { std::fprintf(stderr, "FAIL %s:%d: %s == %s\n", __FILE__, __LINE__, #a, #b); ++g_failures; } } while (0)
// The haystack must not contain the needle (secret-leak scans). Never prints either value.
#define CHECK_NOT_CONTAINS(hay, needle) do { ++g_checks; if (std::string(hay).find(needle) != std::string::npos) { \
    std::fprintf(stderr, "FAIL %s:%d: secret sentinel found in %s\n", __FILE__, __LINE__, #hay); ++g_failures; } } while (0)
// A check that cannot run in this environment is reported, never skipped silently.
#define NOTE_SKIPPED(why) do { ++g_notes; std::printf("NOTE: not run: %s (%s:%d)\n", why, __FILE__, __LINE__); } while (0)
inline int finish(const char *name) {
    std::printf("%s: %s (%d checks, %d failure(s), %d check group(s) not run)\n", name, g_failures ? "FAILED" : "ok", g_checks, g_failures, g_notes);
    return g_failures ? 1 : 0;
}
