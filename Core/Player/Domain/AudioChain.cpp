// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "AudioChain.h"
#include "PipelinePlan.h"

#include <cmath>
#include <locale>
#include <sstream>

namespace nordtile { namespace player {

const std::array<int, kEqBandCount> &eqBandFrequencies() {
    static const std::array<int, kEqBandCount> hz = {{29, 59, 119, 237, 474, 947, 1889, 3770, 7523, 15011}};
    return hz;
}

double clampEqGain(double db) {
    if (std::isnan(db)) return 0.0;
    if (db < kEqMinDb) return kEqMinDb;
    if (db > kEqMaxDb) return kEqMaxDb;
    return db;
}

EqGains clampEqGains(const EqGains &gains) {
    EqGains out;
    for (int i = 0; i < kEqBandCount; ++i) out[i] = clampEqGain(gains[i]);
    return out;
}

EqGains flatEq() {
    EqGains g;
    g.fill(0.0);
    return g;
}

namespace {
EqPreset preset(const char *key, double b0, double b1, double b2, double b3, double b4, double b5, double b6,
                double b7, double b8, double b9) {
    EqPreset p;
    p.key = key;
    p.gainsDb = EqGains{{b0, b1, b2, b3, b4, b5, b6, b7, b8, b9}};
    return p;
}
}

const std::vector<EqPreset> &eqPresets() {
    //                                   29   59  119  237  474  947  1.9k 3.8k 7.5k  15k
    static const std::vector<EqPreset> presets = {
        preset("flat",                     0,   0,   0,   0,   0,   0,   0,   0,   0,   0),
        preset("bass",                     6,   6,   5,   3,   1,   0,   0,   0,   0,   0),
        preset("treble",                   0,   0,   0,   0,   0,   1,   2,   4,   5,   6),
        preset("vocal",                   -2,  -2,  -1,   0,   2,   3,   4,   3,   1,   0),
        preset("loudness",                 5,   4,   2,   0,  -1,  -1,   0,   2,   4,   4),
        preset("spoken",                  -6,  -5,  -3,   0,   1,   2,   4,   4,   1,  -1),
    };
    return presets;
}

bool eqPresetGains(const std::string &key, EqGains &out) {
    const std::vector<EqPreset> &all = eqPresets();
    for (size_t i = 0; i < all.size(); ++i) {
        if (all[i].key == key) { out = all[i].gainsDb; return true; }
    }
    return false;
}

namespace {
int clampPercent(int percent) {
    if (percent < kBoostMinPercent) return kBoostMinPercent;
    if (percent > kBoostMaxPercent) return kBoostMaxPercent;
    return percent;
}
}

double boostGain(int percent) { return clampPercent(percent) / 100.0; }

double loudnessGain(bool enabled, const StreamLoudness &loudness, int maxBoostPercent) {
    if (!enabled || !loudness.known || !std::isfinite(loudness.loudnessDb)) return 1.0;
    double gain = std::pow(10.0, -loudness.loudnessDb / 20.0);
    const double floor = std::pow(10.0, kLoudnessFloorDb / 20.0);
    const double cap = boostGain(maxBoostPercent);
    if (gain < floor) gain = floor;
    if (gain > cap) gain = cap;
    return gain;
}

AudioChainValues computeAudioChainValues(const AudioSettings &settings, const StreamLoudness &loudness) {
    AudioChainValues v;
    v.eqGainsDb = clampEqGains(settings.eqGainsDb);
    v.loudnessGain = loudnessGain(settings.normalizationEnabled, loudness, settings.normalizationMaxBoostPercent);
    v.boostGain = boostGain(settings.boostPercent);
    bool positiveEq = false;
    for (int i = 0; i < kEqBandCount; ++i) positiveEq = positiveEq || v.eqGainsDb[i] > 0.0;
    v.limiterEnabled = positiveEq || v.boostGain > 1.0 || v.loudnessGain > 1.0;
    return v;
}

std::string formatDouble(double value) {
    std::ostringstream s;
    s.imbue(std::locale::classic());
    s.precision(9);
    s << value;
    return s.str();
}

namespace {
PropertySetting prop(const std::string &name, const std::string &value) {
    PropertySetting p;
    p.name = name;
    p.value = value;
    return p;
}
}

std::vector<ElementProperties> audioChainProperties(const AudioChainValues &values,
                                                    const std::string &limiterFactory) {
    std::vector<ElementProperties> out;

    ElementProperties eq;
    eq.element = elementnames::kAudioEq;
    for (int i = 0; i < kEqBandCount; ++i) {
        std::ostringstream name;
        name << "band" << i;
        eq.properties.push_back(prop(name.str(), formatDouble(clampEqGain(values.eqGainsDb[i]))));
    }
    out.push_back(eq);

    ElementProperties loud;
    loud.element = elementnames::kAudioLoudness;
    loud.properties.push_back(prop("volume", formatDouble(values.loudnessGain)));
    out.push_back(loud);

    ElementProperties boost;
    boost.element = elementnames::kAudioBoost;
    boost.properties.push_back(prop("volume", formatDouble(values.boostGain < 1.0 ? 1.0 : values.boostGain)));
    out.push_back(boost);

    if (limiterFactory == "rglimiter") {
        ElementProperties lim;
        lim.element = elementnames::kAudioLimiter;
        lim.properties.push_back(prop("enabled", values.limiterEnabled ? "true" : "false"));
        out.push_back(lim);
    } else if (limiterFactory == "audiodynamic") {
        // Fallback limiter: a hard-knee compressor with ratio 0 holds every
        // sample at or below the threshold. Disengaged = threshold 1.0, which
        // only clamps what the sink would clip anyway.
        ElementProperties lim;
        lim.element = elementnames::kAudioLimiter;
        lim.properties.push_back(prop("mode", "compressor"));
        lim.properties.push_back(prop("characteristics", "hard-knee"));
        lim.properties.push_back(prop("ratio", "0"));
        lim.properties.push_back(prop("threshold", values.limiterEnabled ? "0.977" : "1"));
        out.push_back(lim);
    }
    return out;
}

} } // namespace nordtile::player
