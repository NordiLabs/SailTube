// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#pragma once

#include <cstddef>

namespace nordtile { namespace music { namespace beta {

// A network adapter that has just received an HTTPS answer offers its "Date" header here. In a time-limited beta the
// date raises the clock reading (a tester cannot revive an expired beta by setting the phone back while online); in every
// other build this is an empty inline function, so the adapters carry no beta code and no beta state. It never starts
// a request, contacts a host or stores the value anywhere but in memory.
#ifdef SAILTUNE_BETA
void noteServerDate(const char *value, std::size_t size);      // Core/Beta/Application/ServerDateSink.cpp
#else
inline void noteServerDate(const char *, std::size_t) {}
#endif

}}}
