// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "HardwareProbe.h"

#include <cstdlib>
#include <sstream>
#include <vector>

namespace nordtile { namespace player {

namespace {

std::string flatten(const std::string &s, bool pipeToo) {
    std::string out(s);
    for (std::size_t i = 0; i < out.size(); ++i)
        if (out[i] == '\t' || out[i] == '\n' || out[i] == '\r' || (pipeToo && out[i] == '|')) out[i] = ' ';
    return out;
}

std::string part(const std::string &s) {
    const std::string f = flatten(s, true);
    return f.find_first_not_of(' ') == std::string::npos ? std::string("unknown") : f;
}

std::vector<std::string> splitTabs(const std::string &s) {
    std::vector<std::string> out;
    std::string::size_type start = 0;
    for (;;) {
        const std::string::size_type tab = s.find('\t', start);
        out.push_back(s.substr(start, tab == std::string::npos ? std::string::npos : tab - start));
        if (tab == std::string::npos) break;
        start = tab + 1;
    }
    return out;
}

} // namespace

std::string hardwareProbeBuildKey(const std::string &device, const std::string &osVersion,
                                  const std::string &gstDroidVersion, const std::string &appVersion) {
    return part(device) + "|" + part(osVersion) + "|" + part(gstDroidVersion) + "|" + part(appVersion);
}

std::string releaseFileValue(const std::string &text, const std::string &key) {
    std::istringstream in(text);
    std::string line;
    while (std::getline(in, line)) {
        if (!line.empty() && line[line.size() - 1] == '\r') line.erase(line.size() - 1);
        if (line.compare(0, key.size() + 1, key + "=") != 0) continue;
        std::string v = line.substr(key.size() + 1);
        if (v.size() >= 2 && (v[0] == '"' || v[0] == '\'') && v[v.size() - 1] == v[0]) v = v.substr(1, v.size() - 2);
        return v;
    }
    return std::string();
}

std::string encodeProbeVerdict(const HardwareProbeVerdict &v) {
    const ProbeState s = v.record.state;
    if (s != ProbeState::Passed && s != ProbeState::Failed) return std::string();
    std::ostringstream out;
    out << "v1\t" << (s == ProbeState::Passed ? "passed" : "failed") << '\t'
        << (s == ProbeState::Passed && v.firstFrameMs >= 0 ? v.firstFrameMs : -1) << '\t'
        << flatten(v.record.buildKey, false) << '\t' << flatten(v.detail, false);
    return out.str();
}

bool decodeProbeVerdict(const std::string &text, HardwareProbeVerdict *verdict) {
    const std::vector<std::string> f = splitTabs(text);
    if (f.size() != 5 || f[0] != "v1" || f[3].empty()) return false;
    HardwareProbeVerdict v;
    if (f[1] == "passed") v.record.state = ProbeState::Passed;
    else if (f[1] == "failed") v.record.state = ProbeState::Failed;
    else return false;
    char *end = nullptr;
    const long long ms = std::strtoll(f[2].c_str(), &end, 10);
    if (f[2].empty() || !end || *end) return false;
    v.firstFrameMs = v.record.state == ProbeState::Passed && ms >= 0 ? ms : -1;
    v.record.buildKey = f[3];
    v.detail = f[4];
    *verdict = v;
    return true;
}

std::string probeUnavailableReason(bool droidvdecPresent, bool droideglsinkPresent) {
    if (!droidvdecPresent) return "droidvdec missing";
    if (!droideglsinkPresent) return "droideglsink missing";
    return std::string();
}

std::string probeResultLine(ProbeState state, const std::string &detail, std::int64_t firstFrameMs,
                            bool hardwareRequested, const std::string &unavailableReason) {
    if (!unavailableReason.empty()) return "unavailable: " + unavailableReason;
    switch (state) {
    case ProbeState::Passed: {
        if (firstFrameMs < 0) return "passed";
        std::ostringstream out;
        out << "passed (first frame " << firstFrameMs << " ms)";
        return out.str();
    }
    case ProbeState::Failed: return detail.empty() ? std::string("failed") : "failed: " + detail;
    case ProbeState::Running: return "running";
    case ProbeState::NotRun: break;
    }
    return hardwareRequested ? "not run" : "not run (hardware decode off)";
}

} } // namespace nordtile::player
