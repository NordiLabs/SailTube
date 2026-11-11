// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "GstCapabilityProbe.h"

#include <QElapsedTimer>
#include <QLibrary>
#include <QMutex>
#include <QMutexLocker>

namespace nordtile { namespace video { namespace platform {

namespace {
// Minimal mirror of the public, stable GStreamer 1.x C ABI used here.
typedef int gboolean;
typedef unsigned long GType;
struct GList { void *data; GList *next; GList *prev; };
struct GstStaticCaps { void *caps; const char *string; void *reserved[4]; };
struct GstStaticPadTemplate { const char *nameTemplate; int direction; int presence; GstStaticCaps staticCaps; };
const int padDirectionSink = 2;

typedef gboolean (*InitCheck)(int *, char ***, void **);
typedef void *(*FactoryFind)(const char *);
typedef const GList *(*StaticPadTemplates)(void *);
typedef void (*ObjectUnref)(void *);
typedef void *(*FeatureLoad)(void *);
typedef unsigned (*FeatureRank)(void *);
typedef void (*SetFeatureRank)(void *, unsigned);
typedef GType (*ElementType)(void *);
typedef gboolean (*ElementRegister)(void *, const char *, unsigned, GType);
typedef void (*Version)(unsigned *, unsigned *, unsigned *, unsigned *);

const char *const probedElements[] = {
    "droidvdec", "droidadec", "avdec_h264", "openh264dec", "avdec_h265", "libde265dec",
    "vp8dec", "vp9dec", "avdec_vp9", "dav1ddec", "av1dec", "avdec_av1",
    "avdec_aac", "faad", "fdkaacdec", "opusdec", "vorbisdec",
    "qtdemux", "matroskademux", "hlsdemux", "hlsdemux2", "dashdemux", "dashdemux2",
    "souphttpsrc", "playbin", "playbin3", "droideglsink"
};

bool has(const QStringList &present, const char *name) { return present.contains(QLatin1String(name)); }

// Kept after the probe so the "playbin" factory name can be pointed at either
// implementation later (selectPlaybinImplementation).
struct PlaybinSwitch {
    ElementRegister registerElement = nullptr;
    GType playbin = 0;
    GType playbin3 = 0;
    unsigned playbinRank = 0;
    bool usingPlaybin3 = false;
} playbinSwitch;
// Guards playbinSwitch: the probe may run on a worker thread (NFR-PERF-01) while the GUI
// thread selects an implementation.
QMutex playbinSwitchMutex;

GType loadedType(FactoryFind find, FeatureLoad load, ElementType elementType, ObjectUnref unref, const char *name) {
    GType type = 0;
    if (void *factory = find(name)) {
        void *loaded = load(factory);
        if (loaded) { type = elementType(loaded); unref(loaded); }
        unref(factory);
    }
    return type;
}
}

bool selectPlaybinImplementation(bool playbin3) {
    QMutexLocker lock(&playbinSwitchMutex);
    PlaybinSwitch &p = playbinSwitch;
    if (!p.registerElement || !p.playbin || !p.playbin3) return !playbin3;
    if (p.usingPlaybin3 == playbin3) return true;
    // gst_element_register replaces the registry feature of that name, so the
    // next gst_element_factory_make("playbin") (Qt's backend) builds this type.
    if (!p.registerElement(nullptr, "playbin", p.playbinRank, playbin3 ? p.playbin3 : p.playbin)) return false;
    p.usingPlaybin3 = playbin3;
    return true;
}

media::DecoderCapabilities capabilitiesFrom(const QStringList &present, const QString &droidVideoCaps,
                                            const QString &droidAudioCaps, bool adaptivePipeline) {
    media::DecoderCapabilities c;
    const bool droid = has(present, "droidvdec");
    const auto hw = [&](const char *mime) { return droid && droidVideoCaps.contains(QLatin1String(mime)); };
    c.h264.hardware = hw("video/x-h264");
    c.hevc.hardware = hw("video/x-h265");
    c.vp8.hardware = hw("video/x-vp8");
    c.vp9.hardware = hw("video/x-vp9");
    c.av1.hardware = hw("video/x-av1");
    c.h264.available = c.h264.hardware || has(present, "avdec_h264") || has(present, "openh264dec");
    c.hevc.available = c.hevc.hardware || has(present, "avdec_h265") || has(present, "libde265dec");
    c.vp8.available = c.vp8.hardware || has(present, "vp8dec");
    c.vp9.available = c.vp9.hardware || has(present, "vp9dec") || has(present, "avdec_vp9");
    c.av1.available = c.av1.hardware || has(present, "dav1ddec") || has(present, "av1dec") || has(present, "avdec_av1");
    // Software decoders separately (the selection policy needs to know whether
    // anything but droidvdec can decode a codec; PLY-21 software is the default).
    c.h264.software = has(present, "avdec_h264") || has(present, "openh264dec");
    c.hevc.software = has(present, "avdec_h265") || has(present, "libde265dec");
    c.vp8.software = has(present, "vp8dec");
    c.vp9.software = has(present, "vp9dec") || has(present, "avdec_vp9");
    c.av1.software = has(present, "dav1ddec") || has(present, "av1dec") || has(present, "avdec_av1");
    const bool droidAac = has(present, "droidadec") && droidAudioCaps.contains(QLatin1String("audio/mpeg"));
    c.aac = droidAac || has(present, "avdec_aac") || has(present, "faad") || has(present, "fdkaacdec");
    c.opus = has(present, "opusdec");
    c.vorbis = has(present, "vorbisdec");
    c.mp4Demux = has(present, "qtdemux");
    c.webmDemux = has(present, "matroskademux");
    c.hls = has(present, "hlsdemux") || has(present, "hlsdemux2");
    c.adaptive = adaptivePipeline && c.mp4Demux;
    return c;
}

QVariantMap GstProbeReport::toVariant() const {
    QVariantMap m;
    m.insert(QStringLiteral("loaded"), loaded);
    m.insert(QStringLiteral("version"), version);
    m.insert(QStringLiteral("elements"), presentElements);
    m.insert(QStringLiteral("sinkCaps"), decoderSinkCaps);
    m.insert(QStringLiteral("ranks"), ranks);
    m.insert(QStringLiteral("playbin3"), playbin3);
    m.insert(QStringLiteral("playbinSwitchable"), playbinSwitchable);
    m.insert(QStringLiteral("adaptiveNeedsPlaybin3"), adaptiveNeedsPlaybin3);
    m.insert(QStringLiteral("hardwareDecoderDemoted"), hardwareDecoderDemoted);
    m.insert(QStringLiteral("elapsedMs"), elapsedMs);
    const auto codec = [](const media::CodecSupport &s) {
        return s.hardware ? QStringLiteral("hardware") : s.available ? QStringLiteral("software") : QStringLiteral("none");
    };
    m.insert(QStringLiteral("h264"), codec(capabilities.h264));
    m.insert(QStringLiteral("hevc"), codec(capabilities.hevc));
    m.insert(QStringLiteral("vp8"), codec(capabilities.vp8));
    m.insert(QStringLiteral("vp9"), codec(capabilities.vp9));
    m.insert(QStringLiteral("av1"), codec(capabilities.av1));
    m.insert(QStringLiteral("aac"), capabilities.aac);
    m.insert(QStringLiteral("opus"), capabilities.opus);
    m.insert(QStringLiteral("hls"), capabilities.hls);
    m.insert(QStringLiteral("adaptive"), capabilities.adaptive);
    return m;
}

GstProbeReport probeGstreamer(AdaptivePipeline mode, bool hardwareDecoding) {
    GstProbeReport report;
    QElapsedTimer clock;
    clock.start();
    QLibrary library(QStringLiteral("gstreamer-1.0"), 0);
    library.setLoadHints(QLibrary::ResolveAllSymbolsHint);
    const auto initCheck = reinterpret_cast<InitCheck>(library.resolve("gst_init_check"));
    const auto find = reinterpret_cast<FactoryFind>(library.resolve("gst_element_factory_find"));
    const auto templates = reinterpret_cast<StaticPadTemplates>(library.resolve("gst_element_factory_get_static_pad_templates"));
    const auto unref = reinterpret_cast<ObjectUnref>(library.resolve("gst_object_unref"));
    const auto load = reinterpret_cast<FeatureLoad>(library.resolve("gst_plugin_feature_load"));
    const auto rank = reinterpret_cast<FeatureRank>(library.resolve("gst_plugin_feature_get_rank"));
    const auto setRank = reinterpret_cast<SetFeatureRank>(library.resolve("gst_plugin_feature_set_rank"));
    const auto elementType = reinterpret_cast<ElementType>(library.resolve("gst_element_factory_get_element_type"));
    const auto registerElement = reinterpret_cast<ElementRegister>(library.resolve("gst_element_register"));
    const auto version = reinterpret_cast<Version>(library.resolve("gst_version"));
    if (!initCheck || !find || !templates || !unref || !load || !rank || !setRank || !elementType || !registerElement || !version
            || !initCheck(nullptr, nullptr, nullptr)) {
        // Unknown platform (host build, missing library): assume the baseline
        // that every Sailfish device documents, H.264 + AAC in MP4.
        report.capabilities = capabilitiesFrom(QStringList() << QStringLiteral("avdec_h264")
            << QStringLiteral("avdec_aac") << QStringLiteral("qtdemux"), QString(), QString(), false);
        report.elapsedMs = clock.elapsed();
        return report;
    }
    // Never unloaded: Qt Multimedia uses the same library for the process lifetime.
    report.loaded = true;
    unsigned major = 0, minor = 0, micro = 0, nano = 0;
    version(&major, &minor, &micro, &nano);
    report.version = QStringLiteral("%1.%2.%3").arg(major).arg(minor).arg(micro);
    QString droidVideo, droidAudio;
    for (const char *name : probedElements) {
        void *factory = find(name);
        if (!factory) continue;
        report.presentElements << QString::fromLatin1(name);
        report.ranks.insert(QString::fromLatin1(name), rank(factory));
        if (QByteArray(name) == "droidvdec" || QByteArray(name) == "droidadec") {
            QString caps;
            for (const GList *t = templates(factory); t; t = t->next) {
                const auto *pad = static_cast<const GstStaticPadTemplate *>(t->data);
                if (pad && pad->direction == padDirectionSink && pad->staticCaps.string)
                    caps += QString::fromUtf8(pad->staticCaps.string).left(4096);
            }
            report.decoderSinkCaps.insert(QString::fromLatin1(name), caps.left(4096));
            (QByteArray(name) == "droidvdec" ? droidVideo : droidAudio) = caps;
            if (!hardwareDecoding && QByteArray(name) == "droidvdec") {
                setRank(factory, 0);
                report.hardwareDecoderDemoted = true;
            }
        }
        unref(factory);
    }
    report.playbin3 = report.presentElements.contains(QStringLiteral("playbin3"));
    const bool legacyDash = report.presentElements.contains(QStringLiteral("dashdemux"));
    const bool dash2 = report.presentElements.contains(QStringLiteral("dashdemux2"));
    // The new adaptive demuxers are written for playbin3/decodebin3; Qt 5.6
    // asks for "playbin". The session points that name at playbin3 only for
    // adaptive attempts (selectPlaybinImplementation) and keeps the stock
    // playbin for everything else, which Qt 5.6 fully understands.
    if (report.playbin3 && mode != AdaptivePipeline::Off) {
        QMutexLocker lock(&playbinSwitchMutex);
        PlaybinSwitch &p = playbinSwitch;
        p.registerElement = registerElement;
        p.playbin = loadedType(find, load, elementType, unref, "playbin");
        p.playbin3 = loadedType(find, load, elementType, unref, "playbin3");
        p.playbinRank = report.ranks.value(QStringLiteral("playbin")).toUInt();
        report.playbinSwitchable = p.playbin && p.playbin3;
    }
    report.adaptiveNeedsPlaybin3 = mode == AdaptivePipeline::Playbin3 || (dash2 && !legacyDash);
    report.alwaysPlaybin3 = mode == AdaptivePipeline::Playbin3 && report.playbinSwitchable;
    const bool adaptive = mode != AdaptivePipeline::Off
        && (report.adaptiveNeedsPlaybin3 ? (dash2 && report.playbinSwitchable) : legacyDash);
    // PLT-08 / PLY-21: the hardware flags stay even when droidvdec is demoted.
    // The player never autoplugs it (GstEngine demotes it at start-up) and only
    // creates it explicitly; the per-item plan decides (selectStreamsWithFallback:
    // software first, hardware only for an item no software decoder can play).
    report.capabilities = capabilitiesFrom(report.presentElements, droidVideo, droidAudio, adaptive);
    // PLT-08: droidadec is demoted too (no droid element is ever autoplugged)
    // and the player only creates droidvdec explicitly, so AAC counts only
    // with a software AAC decoder; without one Opus is chosen (SEL-04).
    report.capabilities.aac = report.presentElements.contains(QStringLiteral("avdec_aac"))
        || report.presentElements.contains(QStringLiteral("faad"))
        || report.presentElements.contains(QStringLiteral("fdkaacdec"));
    report.elapsedMs = clock.elapsed();
    return report;
}

}}}
