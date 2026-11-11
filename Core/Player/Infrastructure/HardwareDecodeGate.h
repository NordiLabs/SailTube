// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#pragma once

// PLY-20: the per-device hardware-decode probe, run once per build when the user enables
// hardware decode, and its persisted verdict (DecoderCapabilities::hardwareDecodeVerified).
// Qt Core only, no GStreamer: the probe pipeline itself is a HardwareProbeRunner
// (GstHardwareProbeRunner in HardwareProbeRunner.h, FULL build only), so the gate also
// compiles in the STORE build, where it has no runner and never verifies.
//
//   setHardwareRequested(true)  -> needsProbe() (requested, droidvdec + droideglsink present,
//                                  no verdict stored for this build) -> next event-loop turn:
//                                  runner->start(bundled 2 s H.264 clip) -> first decoded frame
//                                  within 2 s = Passed; an error, the end of the clip without a
//                                  frame or no frame by 2 s = Failed (DecodeModePolicy).
//   verdict                      -> storage (one line, HardwareProbe.h) keyed by the build, so an
//                                  OS / gst-droid / app update probes again; changed() fires.
//
// The 3 s per-video watchdog and the per-build fallback (an element missing) live in the
// player (VideoPlayer, PipelinePlan); this gate only decides whether hardware may be chosen.

#include "Core/Player/Domain/HardwareProbe.h"
#include "Core/Player/Domain/PipelinePlan.h"

#include <QElapsedTimer>
#include <QObject>
#include <QString>
#include <QTimer>

#include <functional>
#include <string>

namespace nordtile { namespace player {

// The probe pipeline (PLY-20): buildProbePlan(ProbeKind::HardwareDecode) on a local clip.
class HardwareProbeRunner {
public:
    struct Listener {
        std::function<void()> frame;                     // first decoded frame reached the sink
        std::function<void(const std::string &)> error;  // pipeline error, or the clip ended without a frame
    };
    virtual ~HardwareProbeRunner() {}
    // Starts the pipeline on `clipUri` (file://). The listener is called later, on the thread that
    // called start(), never from inside start(). False (with *why) when it cannot start at all.
    virtual bool start(const std::string &clipUri, const Listener &listener, std::string *why) = 0;
    // Tears the pipeline down; no listener call after it returns. Safe when not running.
    virtual void stop() = 0;
};

class HardwareDecodeGate final : public QObject {
    Q_OBJECT
public:
    struct Storage {
        std::function<QString()> load;                 // the stored verdict line, "" none
        std::function<void(const QString &)> save;
    };
    struct Options {
        std::string buildKey;                // hardwareProbeBuildKey(...), see currentHardwareBuildKey()
        bool droidvdecPresent = false;       // per-build fallback: both elements in the registry
        bool droideglsinkPresent = false;
        QString clipUri;                     // probe clip; "" = the bundled clip written to clipDirectory
        QString clipDirectory;               // flash-backed (PLT-09), e.g. <cache>/probe
        Storage storage;                     // empty = nothing persisted (tests)
    };

    // PLY-20: a private ini file (fallbacks off) holding the verdict line.
    static Storage iniStorage(const QString &path);

    // runner may be null (STORE build): the gate then never probes and never verifies.
    HardwareDecodeGate(HardwareProbeRunner *runner, const Options &options, QObject *parent = nullptr);
    ~HardwareDecodeGate() override;

    // PLY-20/21: the hardware-decode setting. The first enable on a build without a stored verdict
    // starts the probe (on the next event-loop turn); turning it off cancels a running probe.
    void setHardwareRequested(bool requested);
    // Settings "probe again": forget the stored verdict; probes now when requested.
    void retryProbe();

    // PLY-20 -> SEL-02 / selection: DecoderCapabilities::hardwareDecodeVerified.
    bool verified() const;
    ProbeState probeState() const { return policy_.probeState(); }
    const HardwareProbeVerdict &verdict() const { return verdict_; }
    // DIA-02 "hardware-decode probe result" (probeResultLine).
    QString resultLine() const;
    const std::string &buildKey() const { return options_.buildKey; }

signals:
    void changed();  // verdict, running state or request changed

private:
    void schedule();
    void startProbe();
    void finish(DecodeEvent event, const std::string &detail);
    QString clipUri();
    std::string unavailable() const;

    HardwareProbeRunner *runner_;
    Options options_;
    DecodeModePolicy policy_;
    HardwareProbeVerdict verdict_;
    QElapsedTimer clock_;
    QTimer deadline_;   // DecodeModePolicy::kProbeFrameDeadlineMs
    quint64 run_ = 0;   // drops listener calls of an older run
    bool scheduled_ = false;
};

} } // namespace nordtile::player
