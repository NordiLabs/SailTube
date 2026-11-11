// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "SettingsDefaults.h"
#include <cmath>
#include <cstdlib>
#include <sstream>

namespace nordtile { namespace video { namespace settings {

namespace {
const double none = 0;
#define CHOICE(key, def, options) { key, SettingType::Text, def, none, none, none, options, Variant::All }
#define INT_CHOICE(key, def, options) { key, SettingType::Int, def, none, none, none, options, Variant::All }
#define SWITCH(key, def) { key, SettingType::Bool, def, none, none, none, "", Variant::All }
#define NUMBER(key, def, lo, hi, step) { key, SettingType::Int, def, lo, hi, step, "", Variant::All }

// SET-01 inventory. Keep in step with qml/logic/SettingsInventory.js (tests/settings fails
// otherwise) and with the spec table; change a default here, never in the UI.
const std::vector<SettingSpec> table = {
    CHOICE("content/landingPage", "subscriptions", "subscriptions|recommended|search"),
    SWITCH("content/hideShorts", "true"),
    SWITCH("content/hideWatched", "false"),
    NUMBER("history/cap", "500", 100, 2000, 100),
    SWITCH("history/paused", "false"),
    CHOICE("playback/resume", "always", "always|ask|never"),
    SWITCH("playback/keepDisplayOn", "true"),
    SWITCH("playback/background", "true"),
    CHOICE("playback/verticalFullscreen", "auto", "auto|landscape"),
    CHOICE("playback/edgeGlow", "auto", "auto|hidden"),
    SWITCH("playback/autoplayRelated", "true"),
    SWITCH("playback/keepSessionOnLeave", "true"),
    // Live video in the active cover (user decision 2026-11-08; FULL only).
    { "playback/coverVideo", SettingType::Bool, "true", none, none, none, "", Variant::Full },
    SWITCH("playback/resumeAfterInterruption", "true"),
    INT_CHOICE("playback/doubleTapSeconds", "10", "5|10|15"),
    { "playback/speed", SettingType::Real, "1", 0.25, 2, 0.25, "", Variant::All },
    // SEL-05: the last audio-track language picked in the player ("" = original, no preference).
    { "playback/dubLanguage", SettingType::Text, "", none, none, none, "", Variant::All },
    CHOICE("comments/autoLoad", "near", "near|always|never"),
    // 0 = Automatic: the selection picks 720p (1080p with verified hardware decode), SEL-02.
    INT_CHOICE("playback/maxHeight", "0", "0|2160|1440|1080|720|480|360"),
    INT_CHOICE("playback/maxHeightMobile", "480", "0|2160|1440|1080|720|480|360"),
    SWITCH("playback/hardwareDecode", "false"),
    INT_CHOICE("playback/codecPreference", "0", "0|1|2"),
    SWITCH("playback/highFrameRate", "true"),
    INT_CHOICE("playback/adaptivePipeline", "1", "0|1|2"),
    SWITCH("audio/equalizer", "false"),
    NUMBER("audio/boost", "100", 100, 500, 10),
    SWITCH("audio/loudness", "false"),
    // Playback component: stable volume, a host-side audio choice from the same answer (not output).
    SWITCH("playback/stableVolume", "false"),
    CHOICE("captions/style", "", ""),
    // SB-02: master switch and one mode per category, 0 off, 1 skip, 2 show a button
    // (defaults from Core/Player/Domain/SponsorSkip.cpp; poi_highlight is never skipped).
    SWITCH("sponsorblock/enabled", "true"),
    INT_CHOICE("sponsorblock/sponsor", "1", "0|1|2"),
    INT_CHOICE("sponsorblock/selfpromo", "1", "0|1|2"),
    INT_CHOICE("sponsorblock/interaction", "1", "0|1|2"),
    INT_CHOICE("sponsorblock/intro", "0", "0|1|2"),
    INT_CHOICE("sponsorblock/outro", "0", "0|1|2"),
    INT_CHOICE("sponsorblock/preview", "0", "0|1|2"),
    INT_CHOICE("sponsorblock/music_offtopic", "0", "0|1|2"),
    INT_CHOICE("sponsorblock/filler", "0", "0|1|2"),
    INT_CHOICE("sponsorblock/poi_highlight", "0", "0|2"),
    SWITCH("integrations/ryd", "false"),
    CHOICE("downloads/folder", "", ""),
    INT_CHOICE("downloads/quality", "720", "1080|720|480|360|-1"),
    SWITCH("downloads/unmeteredOnly", "true"),
    { "providers/extractorChannel", SettingType::Text, "stable", none, none, none, "stable|nightly", Variant::Full },
    { "providers/poProvider", SettingType::Bool, "false", none, none, none, "", Variant::Full },
    CHOICE("providers/clientOverride", "auto", "auto"),
    SWITCH("diagnostics/timingLog", "false"),
    SWITCH("diagnostics/startLowRung", "false"),   // NFR-PERF-02 start-up A/B (playback component)
    { "providers/poTokenHeapMb", SettingType::Int, "512", 128, 2048, 64, "", Variant::Full },  // RES-14 heap ceiling
};
#undef CHOICE
#undef INT_CHOICE
#undef SWITCH
#undef NUMBER

std::vector<std::string> split(const std::string &s) {
    std::vector<std::string> out;
    std::stringstream in(s);
    std::string part;
    while (std::getline(in, part, '|')) out.push_back(part);
    return out;
}
}

const std::vector<SettingSpec> &settingsTable() { return table; }

const SettingSpec *findSetting(const std::string &key) {
    for (const SettingSpec &s : table) if (key == s.key) return &s;
    return nullptr;
}

std::vector<SettingSpec> settingsFor(Variant build) {
    std::vector<SettingSpec> out;
    for (const SettingSpec &s : table)
        if (s.variant == Variant::All || s.variant == build) out.push_back(s);
    return out;
}

namespace {
bool inRange(const SettingSpec &spec, const std::string &value) {
    char *end = nullptr;
    const double v = std::strtod(value.c_str(), &end);
    if (value.empty() || *end) return false;
    if (spec.maximum > spec.minimum && (v < spec.minimum || v > spec.maximum)) return false;
    if (spec.step > 0) {
        const double steps = (v - spec.minimum) / spec.step;
        return std::fabs(steps - std::round(steps)) < 1e-6;
    }
    return true;
}
}

bool allowedValue(const SettingSpec &spec, const std::string &value) {
    if (spec.type == SettingType::Bool) return value == "true" || value == "false";
    if (*spec.options) {
        for (const std::string &o : split(spec.options)) if (o == value) return true;
        return false;
    }
    if (spec.type == SettingType::Text) return true;
    return inRange(spec, value);
}

}}}
