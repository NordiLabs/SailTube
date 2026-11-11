// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#pragma once
// The one table of settings defaults (SET-03), shared by the engine (AppSettings and everything
// that reads it), the UI (checked against qml/logic/SettingsInventory.js by tests/settings) and
// the tests. Qt-free C++11.
//
// Keys are "group/name" as stored in SailTube.conf. Defaults are written as text and parsed by
// type, so the table stays a plain constant array. `options` lists the allowed values of a
// choice, separated by '|'; numbers carry min/max/step.
#include <cstddef>
#include <string>
#include <vector>

namespace nordtile { namespace video { namespace settings {

enum class SettingType { Bool, Int, Real, Text };
enum class Variant { All, Full, Store };

struct SettingSpec {
    const char *key;
    SettingType type;
    const char *defaultValue;
    double minimum;
    double maximum;
    double step;
    const char *options;
    Variant variant;
};

// The whole table, in inventory order.
const std::vector<SettingSpec> &settingsTable();
// nullptr for an unknown key.
const SettingSpec *findSetting(const std::string &key);
// Rows a build of the given variant offers ([FULL] rows are absent on STORE).
std::vector<SettingSpec> settingsFor(Variant build);
// True when value (as text) is allowed for the row: in options, or in range and on the step.
bool allowedValue(const SettingSpec &spec, const std::string &value);

}}}
