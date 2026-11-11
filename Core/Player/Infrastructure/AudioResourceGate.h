// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#pragma once

// Sailfish audio resource policy (PLY-23, PLT-12) through libaudioresource-qt:
// the player asks for the media audio resource before PLAYING, and the policy
// takes it away for calls and alarms. Decisions (pause on loss, resume rules)
// live in Domain/AudioResourcePolicy; this class only reports grant and loss.
//
// Built without libaudioresource (host tests, NORDTILE_NO_AUDIORESOURCE) every
// request is granted at once, so playback never depends on the policy daemon.

#include <QObject>

namespace AudioResourceQt { class AudioResource; }

namespace nordtile { namespace player {

class AudioResourceGate final : public QObject {
    Q_OBJECT
    Q_PROPERTY(bool acquired READ acquired NOTIFY acquiredChanged)
public:
    explicit AudioResourceGate(QObject *parent = nullptr);
    ~AudioResourceGate() override;

    bool acquired() const { return acquired_; }
    // Ask for the resource. granted() follows (possibly synchronously when
    // already held); a refusal is reported as lost().
    void request();
    // Give it back (pause by the user for long, stop, teardown).
    void release();

signals:
    void acquiredChanged();
    void granted();
    void lost();

private:
    void sync();
    AudioResourceQt::AudioResource *resource_ = nullptr;
    bool acquired_ = false;
    bool wanted_ = false;
};

}}
