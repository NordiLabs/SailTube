// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#pragma once

// Startup probe of the device's GStreamer registry (the backend of Qt
// Multimedia on Sailfish OS). Loaded with dlopen so the package has no build or
// link dependency on GStreamer; when it cannot be loaded, conservative
// assumptions are used and reported as such. See docs/PLAYBACK.md.

#include "../../Media/Domain/StreamSelection.h"
#include <QString>
#include <QStringList>
#include <QVariantMap>

namespace nordtile { namespace video { namespace platform {

enum class AdaptivePipeline {
    Off,       // never build DASH manifests
    Automatic, // legacy dashdemux, or dashdemux2 with playbin3 for adaptive attempts only
    Playbin3   // playbin3 for every attempt (diagnostics)
};

struct GstProbeReport {
    bool loaded = false;      // libgstreamer-1.0 found and initialised
    QString version;
    QStringList presentElements;
    QVariantMap decoderSinkCaps; // element name -> sink caps (bounded)
    QVariantMap ranks;           // element name -> rank
    bool playbin3 = false;
    bool playbinSwitchable = false;     // "playbin" can be pointed at playbin3 and back
    bool adaptiveNeedsPlaybin3 = false; // only dashdemux2 is installed
    bool alwaysPlaybin3 = false;        // diagnostics mode
    bool hardwareDecoderDemoted = false; // droidvdec rank set to none (PLT-08)
    qint64 elapsedMs = 0;
    media::DecoderCapabilities capabilities;

    QVariantMap toVariant() const;
};

// Call once before the first QMediaPlayer exists: aliasing and rank changes
// affect every pipeline created afterwards in this process.
// hardwareDecoding false demotes droidvdec so autoplugging never picks it; the
// capabilities keep its hardware flags either way (the plan decides per item).
// Thread-safety (NFR-PERF-01): may run on any thread, e.g. the GStreamer
// start-up worker (GstEngine::startInitialize's afterInit job). It touches no
// GUI object and no Qt object outside its own QLibrary; it returns a value
// type. gst_init_check is thread-safe in GStreamer, the library handle is
// process-wide, and the playbin switch state it records is guarded by a
// mutex shared with selectPlaybinImplementation(). Hand the report to the GUI
// thread with a queued signal or event (happens-before); do not read it while
// the probe runs.
GstProbeReport probeGstreamer(AdaptivePipeline mode, bool hardwareDecoding);

// Points the factory name "playbin" (what Qt 5.6 creates) at playbin3 or back at
// the stock playbin. Affects pipelines created afterwards; returns false when the
// requested implementation is unavailable. Requires a successful probe.
// Any thread (serialised with probeGstreamer()).
bool selectPlaybinImplementation(bool playbin3);

// Pure part, unit-tested: maps element presence and droid caps to capabilities.
media::DecoderCapabilities capabilitiesFrom(const QStringList &present, const QString &droidVideoCaps,
                                            const QString &droidAudioCaps, bool adaptivePipeline);

}}}
