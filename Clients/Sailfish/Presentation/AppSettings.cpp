// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "AppSettings.h"

#include "Core/Player/Domain/KeepOnPolicy.h"
#include "Core/Player/Domain/SpeedPolicy.h"
#include "Core/Player/Domain/TapGestures.h"
#ifndef SAILTUBE_STORE
#include "Core/Resolver/Domain/PoTokenPolicy.h"
#endif
#include "Core/Settings/Domain/SettingsDefaults.h"

#include <QStandardPaths>
#include <QStringList>

namespace nordtile { namespace video { namespace sailfish {

namespace {
QString settingsPath() {
    return QStandardPaths::writableLocation(QStandardPaths::AppConfigLocation) + QStringLiteral("/SailTube.conf");
}
const int allowedHeights[] = { 0, 2160, 1440, 1080, 720, 480, 360, 240, 144 };
QString sponsorKey(const QString &apiName) { return QStringLiteral("sponsorblock/") + apiName; }

settings::Variant buildVariant() {
#ifdef SAILTUBE_VARIANT_STORE
    return settings::Variant::Store;
#else
    return settings::Variant::Full;
#endif
}

QVariant typedValue(const settings::SettingSpec &spec, const QString &text) {
    switch (spec.type) {
    case settings::SettingType::Bool: return text == QLatin1String("true");
    case settings::SettingType::Int: return text.toInt();
    case settings::SettingType::Real: return text.toDouble();
    case settings::SettingType::Text: return text;
    }
    return text;
}

QString textOf(const QVariant &value) {
    if (value.type() == QVariant::Bool) return value.toBool() ? QStringLiteral("true") : QStringLiteral("false");
    return value.toString();
}
}

AppSettings::AppSettings(QObject *parent)
    : QObject(parent), settings_(settingsPath(), QSettings::IniFormat) {
    settings_.setFallbacksEnabled(false);
    // SEL-08: an older build stored the listen-only toggle; it is per session now.
    settings_.remove(QStringLiteral("playback/audioOnly"));
    migrateRenamedKeys();
    connect(this, &AppSettings::changed, this, [this]() { ++revision_; });
}

// Earlier ux builds wrote these keys; the engines read the new names. A stored value is an
// explicit user choice (defaults are never written), so it moves once; an invalid one is dropped.
void AppSettings::migrateRenamedKeys() {
    struct Rename { const char *from; const char *to; };
    static const Rename renames[] = {
        { "playback/doubleTapStep", "playback/doubleTapSeconds" },
        { "integrations/sponsorBlock", "sponsorblock/enabled" },
        { "playback/hardwareDecoding", "playback/hardwareDecode" },
    };
    for (const Rename &r : renames) {
        const QString from = QString::fromLatin1(r.from), to = QString::fromLatin1(r.to);
        if (!settings_.contains(from)) continue;
        const QVariant old = settings_.value(from);
        settings_.remove(from);
        if (settings_.contains(to)) continue;
        const settings::SettingSpec *spec = settings::findSetting(r.to);
        if (spec && settings::allowedValue(*spec, textOf(old).toStdString())) settings_.setValue(to, old);
    }
}

void AppSettings::store(const QString &key, const QVariant &value) {
    settings_.setValue(key, value);
    emit valueChanged(key);
    emit changed();
}

// SEL-02: 0 (the default) leaves the ceiling to the selection: 720p with
// software decode, 1080p with verified hardware decode, 480p on mobile data.
// A stored explicit choice is kept.
int AppSettings::maxHeight() const {
    const int value = settings_.value(QStringLiteral("playback/maxHeight"), 0).toInt();
    for (int allowed : allowedHeights) if (allowed == value) return value;
    return 0;
}

void AppSettings::setMaxHeight(int value) {
    bool ok = false;
    for (int allowed : allowedHeights) ok = ok || allowed == value;
    if (!ok || value == maxHeight()) return;
    store(QStringLiteral("playback/maxHeight"), value);
}

int AppSettings::adaptivePipelineValue() const {
    return qBound(0, settings_.value(QStringLiteral("playback/adaptivePipeline"), 1).toInt(), 2);
}

platform::AdaptivePipeline AppSettings::adaptivePipeline() const {
    switch (adaptivePipelineValue()) {
    case 0: return platform::AdaptivePipeline::Off;
    case 2: return platform::AdaptivePipeline::Playbin3;
    default: return platform::AdaptivePipeline::Automatic;
    }
}

void AppSettings::setAdaptivePipeline(int value) {
    value = qBound(0, value, 2);
    if (value == adaptivePipelineValue()) return;
    store(QStringLiteral("playback/adaptivePipeline"), value);
}

bool AppSettings::highFrameRate() const {
    return settings_.value(QStringLiteral("playback/highFrameRate"), true).toBool();
}

void AppSettings::setHighFrameRate(bool value) {
    if (value == highFrameRate()) return;
    store(QStringLiteral("playback/highFrameRate"), value);
}

int AppSettings::codecPreference() const {
    return qBound(0, settings_.value(QStringLiteral("playback/codecPreference"), 0).toInt(), 2);
}

void AppSettings::setCodecPreference(int value) {
    value = qBound(0, value, 2);
    if (value == codecPreference()) return;
    store(QStringLiteral("playback/codecPreference"), value);
}

void AppSettings::setAudioOnly(bool value) {
    if (value == audioOnly_) return;
    audioOnly_ = value;
    emit changed();
}

bool AppSettings::hardwareDecoding() const {
    // PLY-21: software decode is the default. A new key, so installs that had
    // core's old default (on) start in software too.
    return settings_.value(QStringLiteral("playback/hardwareDecode"), false).toBool();
}

void AppSettings::setHardwareDecoding(bool value) {
    if (value == hardwareDecoding()) return;
    store(QStringLiteral("playback/hardwareDecode"), value);
}

double AppSettings::playbackSpeed() const {
    return player::speed::snap(settings_.value(QStringLiteral("playback/speed"), player::speed::kNormal).toDouble());
}

void AppSettings::setPlaybackSpeed(double value) {
    value = player::speed::snap(value);
    if (value == playbackSpeed()) return;
    store(QStringLiteral("playback/speed"), value);
}

bool AppSettings::keepDisplayOn() const {
    return settings_.value(QStringLiteral("playback/keepDisplayOn"), player::kKeepDisplayOnDefault).toBool();
}

void AppSettings::setKeepDisplayOn(bool value) {
    if (value == keepDisplayOn()) return;
    store(QStringLiteral("playback/keepDisplayOn"), value);
}

int AppSettings::doubleTapSeconds() const {
    const int v = settings_.value(QStringLiteral("playback/doubleTapSeconds"), player::TapGestures::kDefaultStepSeconds).toInt();
    return v == 5 || v == 10 || v == 15 ? v : player::TapGestures::kDefaultStepSeconds;
}

void AppSettings::setDoubleTapSeconds(int value) {
    if ((value != 5 && value != 10 && value != 15) || value == doubleTapSeconds()) return;
    store(QStringLiteral("playback/doubleTapSeconds"), value);
}

bool AppSettings::autoplayRelated() const {
    return settings_.value(QStringLiteral("playback/autoplayRelated"), true).toBool();
}

void AppSettings::setAutoplayRelated(bool value) {
    if (value == autoplayRelated()) return;
    store(QStringLiteral("playback/autoplayRelated"), value);
}

bool AppSettings::keepSessionOnLeave() const {
    return settings_.value(QStringLiteral("playback/keepSessionOnLeave"), true).toBool();
}

void AppSettings::setKeepSessionOnLeave(bool value) {
    if (value == keepSessionOnLeave()) return;
    store(QStringLiteral("playback/keepSessionOnLeave"), value);
}

bool AppSettings::resumeAfterInterruption() const {
    return settings_.value(QStringLiteral("playback/resumeAfterInterruption"), true).toBool();
}

void AppSettings::setResumeAfterInterruption(bool value) {
    if (value == resumeAfterInterruption()) return;
    store(QStringLiteral("playback/resumeAfterInterruption"), value);
}

bool AppSettings::sponsorBlockEnabled() const {
    return settings_.value(QStringLiteral("sponsorblock/enabled"), player::kSponsorBlockEnabledDefault).toBool();
}

void AppSettings::setSponsorBlockEnabled(bool value) {
    if (value == sponsorBlockEnabled()) return;
    store(QStringLiteral("sponsorblock/enabled"), value);
}

QStringList AppSettings::sponsorCategories() const {
    QStringList names;
    for (const player::SponsorCategoryInfo &info : player::sponsorCategories())
        names << QString::fromLatin1(info.apiName);
    return names;
}

int AppSettings::sponsorSkipMode(const QString &apiName) const {
    player::SponsorCategory category;
    if (!player::sponsorCategoryFromApiName(apiName.toStdString(), &category)) return 0;
    const int fallback = static_cast<int>(player::defaultSkipMode(category));
    const int v = settings_.value(sponsorKey(apiName), fallback).toInt();
    if (v < 0 || v > 2) return fallback;
    // poi_highlight is never auto-skipped (SB-02): AutoSkip reads as ShowButton.
    if (player::sponsorCategoryInfo(category).highlight && v == 1) return 2;
    return v;
}

void AppSettings::setSponsorSkipMode(const QString &apiName, int mode) {
    player::SponsorCategory category;
    if (mode < 0 || mode > 2 || !player::sponsorCategoryFromApiName(apiName.toStdString(), &category)) return;
    if (mode == sponsorSkipMode(apiName)) return;
    store(sponsorKey(apiName), mode);
}

player::SponsorSettings AppSettings::sponsorSettings() const {
    player::SponsorSettings s;
    s.enabled = sponsorBlockEnabled();
    for (const player::SponsorCategoryInfo &info : player::sponsorCategories())
        s.setMode(info.category, static_cast<player::SkipMode>(sponsorSkipMode(QString::fromLatin1(info.apiName))));
    return s;
}

bool AppSettings::timingLog() const { return settings_.value(QStringLiteral("diagnostics/timingLog"), false).toBool(); }

void AppSettings::setTimingLog(bool value) {
    if (value == timingLog()) return;
    store(QStringLiteral("diagnostics/timingLog"), value);
}

// NFR-PERF-02 A/B: off on a clean install.
bool AppSettings::startLowRung() const {
    return settings_.value(QStringLiteral("diagnostics/startLowRung"), false).toBool();
}

void AppSettings::setStartLowRung(bool value) {
    if (value == startLowRung()) return;
    store(QStringLiteral("diagnostics/startLowRung"), value);
}

// Cover video: on on a clean install in FULL. STORE has no cover video (always off, not stored).
bool AppSettings::coverVideo() const {
#ifdef SAILTUBE_STORE
    return false;
#else
    return settings_.value(QStringLiteral("playback/coverVideo"), true).toBool();
#endif
}

void AppSettings::setCoverVideo(bool value) {
#ifdef SAILTUBE_STORE
    Q_UNUSED(value);
#else
    if (value == coverVideo()) return;
    store(QStringLiteral("playback/coverVideo"), value);
#endif
}

#ifndef SAILTUBE_STORE
// RES-14: off on a clean install; the user opts in.
bool AppSettings::poTokenProvider() const { return settings_.value(QStringLiteral("providers/poProvider"), false).toBool(); }

void AppSettings::setPoTokenProvider(bool value) {
    if (value == poTokenProvider()) return;
    store(QStringLiteral("providers/poProvider"), value);
}

int AppSettings::poTokenHeapMb() const {
    return resolver::potoken::clampHeapMb(
        settings_.value(QStringLiteral("providers/poTokenHeapMb"), resolver::potoken::defaultHeapMb).toInt());
}

void AppSettings::setPoTokenHeapMb(int value) {
    value = resolver::potoken::clampHeapMb(value);
    if (value == poTokenHeapMb()) return;
    store(QStringLiteral("providers/poTokenHeapMb"), value);
}
#endif

QVariant AppSettings::value(const QString &key, const QVariant &fallback) const {
    if (key == QLatin1String("playback/maxHeight")) return maxHeight();
    if (key == QLatin1String("playback/adaptivePipeline")) return adaptivePipelineValue();
    if (key == QLatin1String("playback/highFrameRate")) return highFrameRate();
    if (key == QLatin1String("playback/codecPreference")) return codecPreference();
    if (key == QLatin1String("playback/hardwareDecode")) return hardwareDecoding();
    if (key == QLatin1String("playback/speed")) return playbackSpeed();
    if (key == QLatin1String("playback/keepDisplayOn")) return keepDisplayOn();
    if (key == QLatin1String("playback/coverVideo")) return coverVideo();
    if (key == QLatin1String("playback/doubleTapSeconds")) return doubleTapSeconds();
    if (key == QLatin1String("playback/autoplayRelated")) return autoplayRelated();
    if (key == QLatin1String("playback/keepSessionOnLeave")) return keepSessionOnLeave();
    if (key == QLatin1String("playback/resumeAfterInterruption")) return resumeAfterInterruption();
    if (key == QLatin1String("sponsorblock/enabled")) return sponsorBlockEnabled();
    if (key == QLatin1String("diagnostics/timingLog")) return timingLog();
    if (key == QLatin1String("diagnostics/startLowRung")) return startLowRung();
#ifndef SAILTUBE_STORE
    if (key == QLatin1String("providers/poProvider")) return poTokenProvider();
    if (key == QLatin1String("providers/poTokenHeapMb")) return poTokenHeapMb();
#endif
    if (key.startsWith(QLatin1String("sponsorblock/")) && sponsorCategories().contains(key.mid(13)))
        return sponsorSkipMode(key.mid(13));
    const settings::SettingSpec *spec = settings::findSetting(key.toStdString());
    if (!spec) return fallback;     // not a setting: only the caller knows a default
    const QVariant fallbackDefault = typedValue(*spec, QString::fromUtf8(spec->defaultValue));
    if (!settings_.contains(key)) return fallbackDefault;
    // INI stores text; a value the table no longer allows reads as the default.
    const QString stored = textOf(settings_.value(key));
    if (!settings::allowedValue(*spec, stored.toStdString())) return fallbackDefault;
    return typedValue(*spec, stored);
}

void AppSettings::setValue(const QString &key, const QVariant &value) {
    if (key == QLatin1String("playback/maxHeight")) return setMaxHeight(value.toInt());
    if (key == QLatin1String("playback/adaptivePipeline")) return setAdaptivePipeline(value.toInt());
    if (key == QLatin1String("playback/highFrameRate")) return setHighFrameRate(value.toBool());
    if (key == QLatin1String("playback/codecPreference")) return setCodecPreference(value.toInt());
    if (key == QLatin1String("playback/hardwareDecode")) return setHardwareDecoding(value.toBool());
    if (key == QLatin1String("playback/speed")) return setPlaybackSpeed(value.toDouble());
    if (key == QLatin1String("playback/keepDisplayOn")) return setKeepDisplayOn(value.toBool());
    if (key == QLatin1String("playback/coverVideo")) return setCoverVideo(value.toBool());
    if (key == QLatin1String("playback/doubleTapSeconds")) return setDoubleTapSeconds(value.toInt());
    if (key == QLatin1String("playback/autoplayRelated")) return setAutoplayRelated(value.toBool());
    if (key == QLatin1String("playback/keepSessionOnLeave")) return setKeepSessionOnLeave(value.toBool());
    if (key == QLatin1String("playback/resumeAfterInterruption")) return setResumeAfterInterruption(value.toBool());
    if (key == QLatin1String("sponsorblock/enabled")) return setSponsorBlockEnabled(value.toBool());
    if (key == QLatin1String("diagnostics/timingLog")) return setTimingLog(value.toBool());
    if (key == QLatin1String("diagnostics/startLowRung")) return setStartLowRung(value.toBool());
#ifndef SAILTUBE_STORE
    if (key == QLatin1String("providers/poProvider")) return setPoTokenProvider(value.toBool());
    if (key == QLatin1String("providers/poTokenHeapMb")) return setPoTokenHeapMb(value.toInt());
#endif
    if (key.startsWith(QLatin1String("sponsorblock/")) && sponsorCategories().contains(key.mid(13)))
        return setSponsorSkipMode(key.mid(13), value.toInt());
    const settings::SettingSpec *spec = settings::findSetting(key.toStdString());
    if (!spec) return;              // SET-03: only keys in the shared table are settings
    const QString text = textOf(value);
    if (!settings::allowedValue(*spec, text.toStdString())) return;
    if (text == textOf(this->value(key))) return;
    store(key, text);
}

QVariantList AppSettings::schema() const {
    QVariantList out;
    for (const settings::SettingSpec &spec : settings::settingsFor(buildVariant())) {
        QVariantMap row;
        row.insert(QStringLiteral("key"), QString::fromUtf8(spec.key));
        row.insert(QStringLiteral("def"), typedValue(spec, QString::fromUtf8(spec.defaultValue)));
        if (*spec.options) {
            QVariantList options;
            for (const QString &o : QString::fromUtf8(spec.options).split(QLatin1Char('|')))
                options << typedValue(spec, o);
            row.insert(QStringLiteral("values"), options);
        }
        if (spec.maximum > spec.minimum) {
            row.insert(QStringLiteral("min"), spec.minimum);
            row.insert(QStringLiteral("max"), spec.maximum);
            row.insert(QStringLiteral("step"), spec.step);
        }
        row.insert(QStringLiteral("variant"), spec.variant == settings::Variant::Full ? QStringLiteral("FULL")
                                              : spec.variant == settings::Variant::Store ? QStringLiteral("STORE")
                                              : QString());
        out << row;
    }
    return out;
}

QString AppSettings::variant() const {
    return buildVariant() == settings::Variant::Store ? QStringLiteral("STORE") : QStringLiteral("FULL");
}

media::PlaybackPreferences AppSettings::preferences() const {
    media::PlaybackPreferences p;
    p.mode = audioOnly() ? media::PlaybackMode::AudioOnly : media::PlaybackMode::Video;
    p.maxHeight = maxHeight();
    // SEL-02: the ceiling on mobile data (0 = the selection's default, 480p).
    p.mobileMaxHeight = value(QStringLiteral("playback/maxHeightMobile"), 480).toInt();
    p.allowAdaptive = adaptivePipelineValue() != 0;
    p.allowHighFrameRate = highFrameRate();
    p.codec = codecPreference() == 1 ? media::CodecPreference::H264
        : codecPreference() == 2 ? media::CodecPreference::Vp9 : media::CodecPreference::Automatic;
    p.hardwareDecode = hardwareDecoding();
    return p;
}

}}}
