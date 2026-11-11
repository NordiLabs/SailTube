// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#pragma once

// PLY-20: the GStreamer probe pipeline behind HardwareDecodeGate (FULL build only; it links
// GStreamer, which the STORE validator rejects, E9). The header has no GStreamer types.
//
// Pipeline (buildProbePlan(ProbeKind::HardwareDecode)):
//   uridecodebin caps="video/x-h264, stream-format=avc, alignment=au; video/x-vp9" uri=<clip>
//     ! droidvdec ! droideglsink sync=false
// The first buffer that reaches the sink's pad is the first decoded frame (droidvdec produced
// it). Bus errors and an end of stream without a frame are failures. Listener calls are posted
// to the runner's thread (the thread that owns this object).

#include "HardwareDecodeGate.h"

#include <QObject>
#include <QString>

#include <string>

namespace nordtile { namespace player {

class GstHardwareProbeRunner final : public QObject, public HardwareProbeRunner {
public:
    explicit GstHardwareProbeRunner(QObject *parent = nullptr);
    ~GstHardwareProbeRunner() override;

    bool start(const std::string &clipUri, const Listener &listener, std::string *why) override;
    void stop() override;

    // PLY-20 per-build fallback input: the factory is in the registry. Waits for GStreamer
    // (GstEngine::waitInitialized, NFR-PERF-01); false when it cannot be initialised.
    static bool elementPresent(const char *factory);
    // PLY-20 build key of this device: /etc/hw-release NAME (or MER_HA_DEVICE), /etc/os-release
    // VERSION_ID, the gst-droid plugin version and `appVersion` (hardwareProbeBuildKey).
    // Waits for GStreamer like elementPresent(); start() does too.
    static std::string currentBuildKey(const QString &appVersion);

protected:
    bool event(QEvent *event) override;

private:
    struct Run;
    Run *run_ = nullptr;
    quint64 generation_ = 0;
    Listener listener_;
};

} } // namespace nordtile::player
