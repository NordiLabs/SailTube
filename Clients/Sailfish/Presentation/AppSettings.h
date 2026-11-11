// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#pragma once

#include "Core/Media/Domain/StreamSelection.h"
#include "Core/Platform/Infrastructure/GstCapabilityProbe.h"
#include "Core/Player/Domain/SponsorSkip.h"
#include <QObject>
#include <QSettings>
#include <QVariantList>

namespace nordtile { namespace video { namespace sailfish {

// Private preferences in AppConfigLocation/SailTube.conf. No QSettings fallbacks.
// Playback settings carry the SET-01 / requirement defaults; the ux component
// builds their UI.
class AppSettings final : public QObject {
    Q_OBJECT
    // SEL-02 unmetered ceiling; 0 (default) = the selection's per-decode-mode default.
    Q_PROPERTY(int maxHeight READ maxHeight WRITE setMaxHeight NOTIFY changed)
    Q_PROPERTY(int adaptivePipeline READ adaptivePipelineValue WRITE setAdaptivePipeline NOTIFY changed)
    Q_PROPERTY(bool highFrameRate READ highFrameRate WRITE setHighFrameRate NOTIFY changed)
    Q_PROPERTY(int codecPreference READ codecPreference WRITE setCodecPreference NOTIFY changed)
    // SEL-08: per session (process lifetime), never stored: off again after a restart.
    Q_PROPERTY(bool audioOnly READ audioOnly WRITE setAudioOnly NOTIFY changed)
    // PLY-21: off on a clean install; applies per item from the next video.
    Q_PROPERTY(bool hardwareDecoding READ hardwareDecoding WRITE setHardwareDecoding NOTIFY changed)
    Q_PROPERTY(bool restartRequired READ restartRequired NOTIFY changed)
    // PLY-07: remembered speed (0.25 .. 2 in 0.25 steps), default 1.
    Q_PROPERTY(double playbackSpeed READ playbackSpeed WRITE setPlaybackSpeed NOTIFY changed)
    // PLY-12: keep the display on while a video plays in the foreground, default on.
    Q_PROPERTY(bool keepDisplayOn READ keepDisplayOn WRITE setKeepDisplayOn NOTIFY changed)
    // PLY-17: double-tap seek step, 5 / 10 / 15 s, default 10.
    Q_PROPERTY(int doubleTapSeconds READ doubleTapSeconds WRITE setDoubleTapSeconds NOTIFY changed)
    // PLY-25: related-video autoplay with a 5 s countdown, default on.
    Q_PROPERTY(bool autoplayRelated READ autoplayRelated WRITE setAutoplayRelated NOTIFY changed)
    // PLY-10: keep the session when leaving the watch page, default on.
    Q_PROPERTY(bool keepSessionOnLeave READ keepSessionOnLeave WRITE setKeepSessionOnLeave NOTIFY changed)
    // PLY-23: resume after a call or alarm, default on.
    Q_PROPERTY(bool resumeAfterInterruption READ resumeAfterInterruption WRITE setResumeAfterInterruption NOTIFY changed)
    // SB-02 master switch, default on.
    Q_PROPERTY(bool sponsorBlockEnabled READ sponsorBlockEnabled WRITE setSponsorBlockEnabled NOTIFY changed)
    // SB-02: category API names in UI order (sponsor, selfpromo, ..., poi_highlight).
    Q_PROPERTY(QStringList sponsorCategories READ sponsorCategories CONSTANT)
    // DIA-03 timing log (sailtube-timing: lines), default off.
    Q_PROPERTY(bool timingLog READ timingLog WRITE setTimingLog NOTIFY changed)
    // NFR-PERF-02 start-up A/B, default off: the first plan of a video uses the best dual rung
    // <= 480p of the normal pick's codec family, then one same-set switch up.
    Q_PROPERTY(bool startLowRung READ startLowRung WRITE setStartLowRung NOTIFY changed)
    // Cover video: the playing video live in the active cover; default on in FULL. STORE: always
    // false, writes ignored.
    Q_PROPERTY(bool coverVideo READ coverVideo WRITE setCoverVideo NOTIFY changed)
#ifndef SAILTUBE_STORE
    // RES-14 [FULL]: the optional PO-token helper, off by default; its V8 heap ceiling in MiB
    // (128..2048, default 512). STORE has neither.
    Q_PROPERTY(bool poTokenProvider READ poTokenProvider WRITE setPoTokenProvider NOTIFY changed)
    Q_PROPERTY(int poTokenHeapMb READ poTokenHeapMb WRITE setPoTokenHeapMb NOTIFY changed)
#endif
    // Bumped on every change; QML bindings over value() read it to re-evaluate.
    Q_PROPERTY(int revision READ revision NOTIFY changed)
    // SET-03: the shared defaults table rows this build offers ([FULL] rows absent on STORE):
    // key, def, and values or min/max/step, variant.
    Q_PROPERTY(QVariantList schema READ schema CONSTANT)
    // "FULL" or "STORE" (SAILTUBE_VARIANT_STORE at build time).
    Q_PROPERTY(QString variant READ variant CONSTANT)
public:
    explicit AppSettings(QObject *parent = nullptr);

    int maxHeight() const;
    void setMaxHeight(int);
    int adaptivePipelineValue() const;
    platform::AdaptivePipeline adaptivePipeline() const;
    void setAdaptivePipeline(int);
    bool highFrameRate() const;
    void setHighFrameRate(bool);
    int codecPreference() const;
    void setCodecPreference(int);
    bool audioOnly() const { return audioOnly_; }
    void setAudioOnly(bool);
    bool hardwareDecoding() const;
    void setHardwareDecoding(bool);
    // The player builds a pipeline per item now, so no setting needs a restart.
    bool restartRequired() const { return false; }

    double playbackSpeed() const;
    void setPlaybackSpeed(double);
    bool keepDisplayOn() const;
    void setKeepDisplayOn(bool);
    int doubleTapSeconds() const;
    void setDoubleTapSeconds(int);
    bool autoplayRelated() const;
    void setAutoplayRelated(bool);
    bool keepSessionOnLeave() const;
    void setKeepSessionOnLeave(bool);
    bool resumeAfterInterruption() const;
    void setResumeAfterInterruption(bool);
    bool sponsorBlockEnabled() const;
    void setSponsorBlockEnabled(bool);
    QStringList sponsorCategories() const;
    // SB-02: 0 off, 1 auto-skip, 2 show button; defaults from SponsorSkip.h.
    Q_INVOKABLE int sponsorSkipMode(const QString &apiName) const;
    Q_INVOKABLE void setSponsorSkipMode(const QString &apiName, int mode);
    player::SponsorSettings sponsorSettings() const;
    bool timingLog() const;
    void setTimingLog(bool);
    bool startLowRung() const;
    void setStartLowRung(bool);
    bool coverVideo() const;
    void setCoverVideo(bool);
    // DIA-03: the settings file (AppConfigLocation/SailTube.conf), for the start-up log line.
    QString settingsFilePath() const { return settings_.fileName(); }
#ifndef SAILTUBE_STORE
    bool poTokenProvider() const;
    void setPoTokenProvider(bool);
    int poTokenHeapMb() const;
    void setPoTokenHeapMb(int);
#endif

    media::PlaybackPreferences preferences() const;

    // Generic access (SET-01/03): keys and defaults come from Core/Settings/Domain/
    // SettingsDefaults. Keys with a typed accessor above go through it; other keys are
    // validated against the table. A key outside the table returns `fallback` and is not stored.
    Q_INVOKABLE QVariant value(const QString &key, const QVariant &fallback = QVariant()) const;
    Q_INVOKABLE void setValue(const QString &key, const QVariant &value);
    int revision() const { return revision_; }
    QVariantList schema() const;
    QString variant() const;

signals:
    void changed();
    void valueChanged(const QString &key);

private:
    void store(const QString &key, const QVariant &value);
    void migrateRenamedKeys();

    QSettings settings_;
    bool audioOnly_ = false;
    int revision_ = 0;
};

}}}
