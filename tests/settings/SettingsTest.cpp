// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Settings invariants (NFR-TST-03 for SET-01, SET-02 and SET-03), each a named test:
//   defaults_table_matches_ui     UI literals (SettingsInventory.js) == shared table, both ways
//   fresh_install_defaults        a fresh AppSettings reads every table default, per variant
//   store_variant_hides_full_rows [FULL] rows are absent from a STORE build's rows
//   values_validated_and_typed    setValue rejects unknown keys and disallowed values; values
//                                 round-trip through the INI file with their table type
//   renamed_keys_migrate_once     keys earlier builds wrote move once to the engine names
//   engine_applies_without_restart preferences() follows a change immediately
// run.sh also rebuilds this test against a mutated table and expects it to fail (SET-03).
#include "Clients/Sailfish/Presentation/AppSettings.h"
#include "Core/Settings/Domain/SettingsDefaults.h"
#include <QCoreApplication>
#include <QFile>
#include <QJSEngine>
#include <QSettings>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <cstdio>
#include <functional>
#include <map>

using nordtile::video::sailfish::AppSettings;
namespace st = nordtile::video::settings;
static int failures = 0;
static const char *current = "";
#define CHECK(cond, ...) do { if (!(cond)) { ++failures; std::printf("FAIL %s: ", current); std::printf(__VA_ARGS__); std::printf("\n"); } } while (0)

static QString text(const QVariant &v) {
    if (v.type() == QVariant::Bool) return v.toBool() ? QStringLiteral("true") : QStringLiteral("false");
    if (v.type() == QVariant::Double && v.toDouble() == static_cast<int>(v.toDouble())) return QString::number(static_cast<int>(v.toDouble()));
    return v.toString();
}

static void run(const char *name, const std::function<void()> &body) {
    const int before = failures;
    current = name;
    body();
    std::printf("%s %s\n", failures == before ? "ok  " : "FAIL", name);
}

int main(int argc, char **argv) {
    QTemporaryDir config;
    qputenv("XDG_CONFIG_HOME", config.path().toLocal8Bit());
    QCoreApplication app(argc, argv);
    app.setOrganizationName(QStringLiteral("org.nordilabs.sailtube"));
    app.setApplicationName(QStringLiteral("SailTube"));

    QFile f(QStringLiteral(SAILTUBE_ROOT "/Clients/Sailfish/qml/logic/SettingsInventory.js"));
    if (!f.open(QIODevice::ReadOnly)) { std::printf("FAIL inventory missing\n"); return 1; }
    QJSEngine engine;
    QJSValue r = engine.evaluate(QString::fromUtf8(f.readAll()).replace(QStringLiteral(".pragma library"), QString()),
                                 QStringLiteral("SettingsInventory.js"));
    if (r.isError()) { std::printf("FAIL inventory: %s\n", qPrintable(r.toString())); return 1; }
    QJSValue entries = engine.globalObject().property(QStringLiteral("ENTRIES"));
    std::map<QString, QJSValue> ui;
    for (int i = 0, n = entries.property(QStringLiteral("length")).toInt(); i < n; ++i) {
        QJSValue e = entries.property(static_cast<quint32>(i));
        ui[e.property(QStringLiteral("key")).toString()] = e;
    }

    run("defaults_table_matches_ui", [&]() {
        for (const st::SettingSpec &spec : st::settingsTable()) {
            const QString key = QString::fromUtf8(spec.key);
            auto it = ui.find(key);
            CHECK(it != ui.end(), "%s is in the table but not in SettingsInventory.js", spec.key);
            if (it == ui.end()) continue;
            const QString uiDefault = text(it->second.property(QStringLiteral("def")).toVariant());
            CHECK(uiDefault == QString::fromUtf8(spec.defaultValue), "%s: UI literal %s, table %s",
                  spec.key, qPrintable(uiDefault), spec.defaultValue);
            const QString uiVariant = it->second.property(QStringLiteral("variant")).toString();
            const bool fullOnly = spec.variant == st::Variant::Full;
            CHECK((uiVariant == QStringLiteral("FULL")) == fullOnly, "%s: variant differs", spec.key);
            const QJSValue values = it->second.property(QStringLiteral("values"));
            if (*spec.options && values.isArray()) {
                QStringList uiValues;
                for (int k = 0, n = values.property(QStringLiteral("length")).toInt(); k < n; ++k)
                    uiValues << text(values.property(static_cast<quint32>(k)).toVariant());
                CHECK(uiValues.join(QLatin1Char('|')) == QString::fromUtf8(spec.options), "%s: options %s vs %s",
                      spec.key, qPrintable(uiValues.join(QLatin1Char('|'))), spec.options);
            }
        }
        for (const auto &kv : ui)
            CHECK(st::findSetting(kv.first.toStdString()) != nullptr, "%s is in the UI but not in the table", qPrintable(kv.first));
    });

    run("fresh_install_defaults", [&]() {
        AppSettings s;
        for (const st::SettingSpec &spec : st::settingsTable()) {
            const QString got = text(s.value(QString::fromUtf8(spec.key)));
            CHECK(got == QString::fromUtf8(spec.defaultValue), "%s reads %s, table %s", spec.key, qPrintable(got), spec.defaultValue);
            CHECK(st::allowedValue(spec, spec.defaultValue), "%s default is not an allowed value", spec.key);
        }
        CHECK(s.schema().size() == static_cast<int>(st::settingsFor(st::Variant::Full).size()), "schema has every FULL row");
        CHECK(s.variant() == QStringLiteral("FULL"), "this build is FULL");
    });

    run("store_variant_hides_full_rows", [&]() {
        const auto store = st::settingsFor(st::Variant::Store);
        const auto full = st::settingsFor(st::Variant::Full);
        int fullOnly = 0;
        for (const st::SettingSpec &s : st::settingsTable()) if (s.variant == st::Variant::Full) ++fullOnly;
        // The spec's two [FULL] rows, the PO-token heap ceiling and the cover video (off on STORE).
        CHECK(fullOnly == 4, "four [FULL] rows (extractor channel, PO provider, PO heap ceiling, cover video)");
        CHECK(st::findSetting("playback/coverVideo") && st::findSetting("playback/coverVideo")->variant == st::Variant::Full,
              "cover video is FULL only");
        CHECK(store.size() + fullOnly == full.size(), "STORE lacks exactly the [FULL] rows");
        for (const st::SettingSpec &s : store) CHECK(s.variant != st::Variant::Full, "%s on STORE", s.key);
    });

    run("values_validated_and_typed", [&]() {
        {
            AppSettings s;
            const int rev = s.revision();
            s.setValue(QStringLiteral("content/hideShorts"), false);
            s.setValue(QStringLiteral("history/cap"), 800);
            s.setValue(QStringLiteral("history/cap"), 850);                 // off the 100 step
            s.setValue(QStringLiteral("playback/edgeGlow"), QStringLiteral("hidden"));
            s.setValue(QStringLiteral("playback/edgeGlow"), QStringLiteral("sometimes"));   // not an option
            s.setValue(QStringLiteral("playback/maxHeight"), 480);
            s.setValue(QStringLiteral("playback/maxHeight"), 999);
            s.setValue(QStringLiteral("not/aSetting"), 1);
            CHECK(s.revision() == rev + 4, "four accepted changes, got %d", s.revision() - rev);
            CHECK(!s.value(QStringLiteral("not/aSetting")).isValid(), "unknown key not stored");
            CHECK(s.value(QStringLiteral("not/aSetting"), 7).toInt() == 7, "unknown key returns the caller's fallback");
        }
        AppSettings s;   // read back from the file
        CHECK(s.value(QStringLiteral("content/hideShorts")).type() == QVariant::Bool, "bool type");
        CHECK(s.value(QStringLiteral("content/hideShorts")).toBool() == false, "bool value");
        CHECK(s.value(QStringLiteral("history/cap")).type() == QVariant::Int, "int type");
        CHECK(s.value(QStringLiteral("history/cap")).toInt() == 800, "int value");
        CHECK(s.value(QStringLiteral("playback/edgeGlow")).toString() == QStringLiteral("hidden"), "text value");
        CHECK(s.maxHeight() == 480, "typed key persisted");
    });

    run("renamed_keys_migrate_once", [&]() {
        {
            QSettings raw(QStandardPaths::writableLocation(QStandardPaths::AppConfigLocation) + QStringLiteral("/SailTube.conf"),
                          QSettings::IniFormat);
            raw.clear();
            raw.setValue(QStringLiteral("playback/doubleTapStep"), 15);
            raw.setValue(QStringLiteral("integrations/sponsorBlock"), false);
            raw.setValue(QStringLiteral("playback/hardwareDecoding"), QStringLiteral("true"));
            raw.sync();
        }
        AppSettings s;
        CHECK(s.doubleTapSeconds() == 15, "double-tap step moved to playback/doubleTapSeconds");
        CHECK(!s.sponsorBlockEnabled(), "SponsorBlock switch moved to sponsorblock/enabled");
        CHECK(s.hardwareDecoding(), "hardware decoding choice moved to playback/hardwareDecode");
        CHECK(!s.value(QStringLiteral("playback/doubleTapStep")).isValid(), "old key is gone");
        {
            QSettings raw(QStandardPaths::writableLocation(QStandardPaths::AppConfigLocation) + QStringLiteral("/SailTube.conf"),
                          QSettings::IniFormat);
            raw.setValue(QStringLiteral("playback/doubleTapStep"), 30);   // not allowed any more
            raw.sync();
        }
        AppSettings again;
        CHECK(again.doubleTapSeconds() == 15, "a stale old value never overwrites the new key");
        QSettings(QStandardPaths::writableLocation(QStandardPaths::AppConfigLocation) + QStringLiteral("/SailTube.conf"),
                  QSettings::IniFormat).clear();
    });

    run("engine_applies_without_restart", [&]() {
        AppSettings s;
        s.setValue(QStringLiteral("playback/maxHeight"), 360);
        s.setValue(QStringLiteral("playback/highFrameRate"), false);
        s.setValue(QStringLiteral("playback/codecPreference"), 2);
        s.setAudioOnly(true);   // per session (SEL-08), not a stored setting
        s.setValue(QStringLiteral("sponsorblock/intro"), 2);
        const auto p = s.preferences();
        CHECK(p.maxHeight == 360, "max height applied");
        CHECK(!p.allowHighFrameRate, "frame rate applied");
        CHECK(p.codec == nordtile::video::media::CodecPreference::Vp9, "codec applied");
        CHECK(p.mode == nordtile::video::media::PlaybackMode::AudioOnly, "audio only applied");
        CHECK(s.sponsorSkipMode(QStringLiteral("intro")) == 2, "SponsorBlock category mode applied");
    });

    std::printf("settings: %s\n", failures ? "FAILED" : "all invariants hold");
    return failures ? 1 : 0;
}
