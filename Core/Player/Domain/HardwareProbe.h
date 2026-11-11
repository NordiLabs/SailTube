// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#pragma once

// PLY-20 per-device hardware-decode probe: the pure parts around
// DecodeModePolicy (PipelinePlan.h). Qt-free, I/O-free, no clock.
//
//   build key     which hardware + software stack a verdict belongs to; a stored
//                 verdict with another key is ignored, so an OS, gst-droid or app
//                 update probes again ("once per build").
//   verdict text  what is persisted (one line) and parsed back.
//   result line   what playback.diagnostics and the DIA-02 report show
//                 ("hardware-decode probe result").

#include "PipelinePlan.h"

#include <cstdint>
#include <string>

namespace nordtile { namespace player {

// PLY-20: "<device>|<os>|<gst-droid>|<app>"; '|', tabs and line breaks inside a part become
// spaces, empty parts read "unknown".
std::string hardwareProbeBuildKey(const std::string &device, const std::string &osVersion,
                                  const std::string &gstDroidVersion, const std::string &appVersion);

// PLY-20 build key input: the value of `key` in an os-release style text (KEY=value or
// KEY="value" per line, e.g. /etc/hw-release, /etc/os-release); "" when absent.
std::string releaseFileValue(const std::string &text, const std::string &key);

// What the probe found, persisted per build.
struct HardwareProbeVerdict {
    HardwareProbeRecord record;   // buildKey + Passed / Failed (NotRun = nothing stored)
    std::string detail;           // why it failed ("no frame within 2 s", "droidvdec: ..."), "" when passed
    std::int64_t firstFrameMs = -1;  // passed: start -> first decoded frame
};

// PLY-20: one line "v1\t<passed|failed>\t<firstFrameMs>\t<buildKey>\t<detail>" (tabs and line
// breaks in the text fields become spaces). A NotRun verdict encodes as "".
std::string encodeProbeVerdict(const HardwareProbeVerdict &verdict);
// PLY-20: false (and *verdict untouched) for anything encodeProbeVerdict did not write.
bool decodeProbeVerdict(const std::string &text, HardwareProbeVerdict *verdict);

// Why the probe could not even start, "" when it can: the hardware elements.
std::string probeUnavailableReason(bool droidvdecPresent, bool droideglsinkPresent);

// PLY-20 / DIA-02: the "hardware-decode probe result" line, e.g.
//   "passed (first frame 412 ms)", "failed: no frame within 2 s", "running",
//   "not run (hardware decode off)", "unavailable: droidvdec missing".
std::string probeResultLine(ProbeState state, const std::string &detail, std::int64_t firstFrameMs,
                            bool hardwareRequested, const std::string &unavailableReason);

} } // namespace nordtile::player
