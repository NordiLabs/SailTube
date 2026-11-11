// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "HardwareDecodeGate.h"
#include "HardwareProbeClip.h"

#include <QSettings>
#include <QUrl>

#include <sstream>

namespace nordtile { namespace player {

namespace {
const char *const kVerdictKey = "hardwareDecode/probe";
}

HardwareDecodeGate::Storage HardwareDecodeGate::iniStorage(const QString &path) {
    Storage s;
    s.load = [path]() {
        QSettings settings(path, QSettings::IniFormat);
        settings.setFallbacksEnabled(false);
        return settings.value(QLatin1String(kVerdictKey)).toString();
    };
    s.save = [path](const QString &line) {
        QSettings settings(path, QSettings::IniFormat);
        settings.setFallbacksEnabled(false);
        if (line.isEmpty()) settings.remove(QLatin1String(kVerdictKey));
        else settings.setValue(QLatin1String(kVerdictKey), line);
        settings.sync();
    };
    return s;
}

HardwareDecodeGate::HardwareDecodeGate(HardwareProbeRunner *runner, const Options &options, QObject *parent)
    : QObject(parent), runner_(runner), options_(options), policy_(options.buildKey) {
    clock_.start();
    deadline_.setSingleShot(true);
    deadline_.setTimerType(Qt::PreciseTimer);
    connect(&deadline_, &QTimer::timeout, this, [this]() {
        const DecodeEvent event = policy_.tick(clock_.elapsed());
        if (event == DecodeEvent::None && policy_.probeState() == ProbeState::Running) deadline_.start(5);  // early wake
        else finish(event, "no frame within 2 s");
    });
    policy_.setHardwareElementsPresent(runner_ && unavailable().empty());
    // A verdict of another build (OS, gst-droid or app update) is ignored: probe again.
    if (options_.storage.load) {
        HardwareProbeVerdict stored;
        if (decodeProbeVerdict(options_.storage.load().toStdString(), &stored) &&
            stored.record.buildKey == options_.buildKey) {
            policy_.restore(stored.record);
            verdict_ = stored;
        }
    }
}

HardwareDecodeGate::~HardwareDecodeGate() {
    ++run_;
    if (runner_) runner_->stop();
}

std::string HardwareDecodeGate::unavailable() const {
    if (!runner_) return "not in this build";
    return probeUnavailableReason(options_.droidvdecPresent, options_.droideglsinkPresent);
}

bool HardwareDecodeGate::verified() const {
    return policy_.probeState() == ProbeState::Passed && policy_.hardwareElementsPresent();
}

QString HardwareDecodeGate::resultLine() const {
    return QString::fromStdString(probeResultLine(policy_.probeState(), verdict_.detail, verdict_.firstFrameMs,
                                                  policy_.hardwareRequested(), unavailable()));
}

void HardwareDecodeGate::setHardwareRequested(bool requested) {
    if (requested == policy_.hardwareRequested()) return;
    policy_.setHardwareRequested(requested);
    if (!requested && policy_.probeState() == ProbeState::Running) {
        // Turned off mid-probe: no verdict; the next enable probes again.
        ++run_;
        deadline_.stop();
        runner_->stop();
        policy_.forgetProbe();
    }
    if (requested) schedule();
    emit changed();
}

void HardwareDecodeGate::retryProbe() {
    ++run_;
    deadline_.stop();
    if (runner_) runner_->stop();
    policy_.forgetProbe();
    verdict_ = HardwareProbeVerdict();
    if (options_.storage.save) options_.storage.save(QString());
    schedule();
    emit changed();
}

void HardwareDecodeGate::schedule() {
    if (scheduled_ || !policy_.needsProbe()) return;
    scheduled_ = true;
    // Never inside the settings change that asked for it.
    QTimer::singleShot(0, this, [this]() {
        scheduled_ = false;
        startProbe();
    });
}

QString HardwareDecodeGate::clipUri() {
    if (!options_.clipUri.isEmpty()) return options_.clipUri;
    const QString path = writeHardwareProbeClip(options_.clipDirectory);
    return path.isEmpty() ? QString() : QUrl::fromLocalFile(path).toString();
}

void HardwareDecodeGate::startProbe() {
    if (!policy_.needsProbe() || !runner_) return;
    const quint64 run = ++run_;
    const qint64 started = clock_.elapsed();
    policy_.onProbeStarted(started);
    const QString uri = clipUri();
    if (uri.isEmpty()) {
        finish(policy_.onProbeError(), "probe clip unavailable");
        return;
    }
    HardwareProbeRunner::Listener listener;
    listener.frame = [this, run, started]() {
        if (run != run_) return;
        const qint64 at = clock_.elapsed();
        verdict_.firstFrameMs = at - started;
        std::ostringstream late;
        late << "first frame after 2 s (" << (at - started) << " ms)";
        finish(policy_.onProbeFrame(at), late.str());
    };
    listener.error = [this, run](const std::string &why) {
        if (run != run_) return;
        finish(policy_.onProbeError(), why.empty() ? std::string("pipeline error") : why);
    };
    std::string why;
    if (!runner_->start(uri.toStdString(), listener, &why)) {
        finish(policy_.onProbeError(), why.empty() ? std::string("probe pipeline did not start") : why);
        return;
    }
    deadline_.start(static_cast<int>(DecodeModePolicy::kProbeFrameDeadlineMs) + 1);
    emit changed();
}

void HardwareDecodeGate::finish(DecodeEvent event, const std::string &detail) {
    if (event != DecodeEvent::ProbePassed && event != DecodeEvent::ProbeFailed) return;
    ++run_;  // later listener calls of this run are dropped
    deadline_.stop();
    if (runner_) runner_->stop();
    const bool passed = event == DecodeEvent::ProbePassed;
    const std::int64_t frameMs = verdict_.firstFrameMs;
    verdict_ = HardwareProbeVerdict();
    verdict_.record = policy_.record();
    verdict_.detail = passed ? std::string() : detail;
    verdict_.firstFrameMs = passed ? frameMs : -1;
    if (options_.storage.save) options_.storage.save(QString::fromStdString(encodeProbeVerdict(verdict_)));
    emit changed();
}

} } // namespace nordtile::player
