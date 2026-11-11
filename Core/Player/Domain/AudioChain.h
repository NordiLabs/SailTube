// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#pragma once

// PLY-22 audio-chain values: 10-band EQ, loudness normalisation, boost and
// limiter. Pure, Qt-free. The pipeline plan (PipelinePlan.h) places the
// elements; this file computes what the engine pushes into them, both at
// build time and live (every stage stays in the chain even when neutral, so
// a live change never needs a rebuild).
//
// Engine contract: after any settings or stream change, call
// computeAudioChainValues() and then audioChainProperties(); apply each
// ElementProperties block to the element with that name if the pipeline has
// it (skipped elements are simply absent). Values are locale-independent
// strings for gst_util_set_object_arg().

#include <array>
#include <string>
#include <vector>

namespace nordtile { namespace player {

// ---- 10-band EQ ------------------------------------------------------------

const int kEqBandCount = 10;
// PLY-22: centre frequencies of equalizer-10bands band0..band9 (fixed by the
// element; Hz).
const std::array<int, kEqBandCount> &eqBandFrequencies();

// PLY-22: UI range and engine clamp (the element itself accepts -24..+12 dB;
// we clamp to the UI range so a stored or scripted value cannot exceed it).
const double kEqMinDb = -12.0;
const double kEqMaxDb = 12.0;

typedef std::array<double, kEqBandCount> EqGains;  // dB per band

// PLY-22: clamp one band gain to [kEqMinDb, kEqMaxDb]; NaN -> 0 dB.
double clampEqGain(double db);
EqGains clampEqGains(const EqGains &gains);
EqGains flatEq();

// PLY-22 presets. `key` is a stable storage key; display strings belong to
// the UI. Order is the menu order.
//   flat     all bands 0 dB
//   bass     low-shelf lift (29-237 Hz)
//   treble   high-shelf lift (3.8-15 kHz)
//   vocal    presence lift (474 Hz-3.8 kHz), lows and highs trimmed
//   loudness "smile" curve (lows + highs up); approximates the equal-loudness
//            contour for quiet listening on the phone speaker or earbuds
//   spoken   speech clarity for talks / podcasts: rumble below ~120 Hz cut,
//            1.9-3.8 kHz intelligibility band lifted, sibilance band trimmed
struct EqPreset {
    std::string key;
    EqGains gainsDb;
};
const std::vector<EqPreset> &eqPresets();
// PLY-22: look up a preset by key; false (and `out` untouched) if unknown.
bool eqPresetGains(const std::string &key, EqGains &out);

// ---- Loudness normalisation, boost, limiter --------------------------------

// PLY-22: loudness floor (-20 dB) and boost range (100..500 %).
const double kLoudnessFloorDb = -20.0;
const int kBoostMinPercent = 100;
const int kBoostMaxPercent = 500;

// User settings that drive the chain (persisted by the settings layer).
struct AudioSettings {
    EqGains eqGainsDb = EqGains{{0, 0, 0, 0, 0, 0, 0, 0, 0, 0}};
    bool normalizationEnabled = false;  // PLY-22: optional, off by default
    // Upper cap of the normalisation gain, in percent of unity (100 = the
    // normaliser only attenuates, 200 = may raise quiet streams by up to
    // +6 dB). Clamped to the boost range. Default 100: like the web player,
    // only loud streams are turned down; raising is the user's choice.
    int normalizationMaxBoostPercent = 100;
    int boostPercent = 100;  // PLY-22: 100..500 %
};

// Per-stream loudness metadata (from playerConfig.audioConfig.loudnessDb, or
// perceptualLoudnessDb - loudnessTargetLkfs; the resolver computes it).
// Positive = the stream is louder than the target.
struct StreamLoudness {
    bool known = false;
    double loudnessDb = 0.0;
};

// PLY-22: linear normalisation gain. 1.0 when disabled or unknown; otherwise
// 10^(-dB/20), floored at 10^(-20/20) = 0.1 and capped at
// maxBoostPercent / 100 (itself clamped to 1.0..5.0). Non-finite -> 1.0.
double loudnessGain(bool enabled, const StreamLoudness &loudness, int maxBoostPercent);

// PLY-22: boost percent -> linear gain, clamped to 1.0..5.0. Never < 1.0.
double boostGain(int percent);

// Everything the engine pushes live into the chain.
struct AudioChainValues {
    EqGains eqGainsDb = EqGains{{0, 0, 0, 0, 0, 0, 0, 0, 0, 0}};  // clamped
    double loudnessGain = 1.0;  // "audio-loudness" volume
    double boostGain = 1.0;     // "audio-boost" volume
    // Limiter engaged whenever the chain can exceed unity: any positive EQ
    // band, boost > 1, or normalisation gain > 1. At unity it is bypassed,
    // so neutral settings stay bit-transparent (rglimiter compresses above
    // -6 dBFS when enabled).
    bool limiterEnabled = false;
};

// PLY-22: derive the live values from settings + stream metadata.
AudioChainValues computeAudioChainValues(const AudioSettings &settings, const StreamLoudness &loudness);

// One element's property assignments, in order.
struct PropertySetting {
    std::string name;
    std::string value;   // gst_util_set_object_arg() syntax
    bool optional = false;  // engine skips it if the element lacks the property
};
struct ElementProperties {
    std::string element;  // element name in the pipeline (PipelinePlan.h names)
    std::vector<PropertySetting> properties;
};

// PLY-22: property values per chain element for `values`. `limiterFactory` is
// the factory the plan chose for "audio-limiter" ("rglimiter" or
// "audiodynamic"; empty = no limiter, no block emitted).
std::vector<ElementProperties> audioChainProperties(const AudioChainValues &values,
                                                    const std::string &limiterFactory);

// Locale-independent number formatting used for every property string ("."
// decimal separator even after Qt called setlocale(LC_ALL, "")).
std::string formatDouble(double value);

} } // namespace nordtile::player
