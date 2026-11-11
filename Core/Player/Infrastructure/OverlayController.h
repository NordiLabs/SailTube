// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#pragma once

// QML-facing driver of the overlay decisions (PLY-17): tap stacking with the
// double-tap window and one-flush seeks (TapGestures) and auto-hide of the
// controls (ControlsVisibility). Owns a single single-shot timer armed at the
// next deadline of either policy; no periodic timer (NFR-PERF-06). The watch
// page (ux component) binds its overlay to these properties and forwards taps.
// Qt Core only, no SailTube types.

#include "../Domain/TapGestures.h"
#include <QElapsedTimer>
#include <QObject>
#include <QTimer>

namespace nordtile { namespace player {

class OverlayController : public QObject {  // not final: qmlRegisterType derives from it
    Q_OBJECT
    Q_PROPERTY(bool controlsVisible READ controlsVisible NOTIFY controlsChanged)
    Q_PROPERTY(bool menuOpen READ menuOpen WRITE setMenuOpen NOTIFY controlsChanged)
    Q_PROPERTY(bool gestureActive READ gestureActive NOTIFY gestureChanged)
    // -1 left (backwards), +1 right (forwards).
    Q_PROPERTY(int gestureSide READ gestureSide NOTIFY gestureChanged)
    // Running total of the double-tap burst in seconds, signed (PLY-17 "show a running total").
    Q_PROPERTY(int gestureSeconds READ gestureSeconds NOTIFY gestureChanged)
    Q_PROPERTY(double gestureTargetMs READ gestureTargetMs NOTIFY gestureChanged)
    // 5, 10 or 15 (setting).
    Q_PROPERTY(int stepSeconds READ stepSeconds WRITE setStepSeconds NOTIFY stepSecondsChanged)
public:
    explicit OverlayController(QObject *parent = nullptr);

    bool controlsVisible() const { return controls_.visible(); }
    bool menuOpen() const { return controls_.menuOpen(); }
    void setMenuOpen(bool open);
    bool gestureActive() const { return taps_.gestureActive(); }
    int gestureSide() const { return taps_.displaySide() == TapSide::Left ? -1 : 1; }
    int gestureSeconds() const { return taps_.displayTotalSeconds(); }
    double gestureTargetMs() const { return static_cast<double>(taps_.targetMs()); }
    int stepSeconds() const { return taps_.stepSeconds(); }
    void setStepSeconds(int seconds);

    // A tap on the video area at x of width, with the player's current position
    // and duration (ms). Single taps toggle the controls after the double-tap
    // window; double taps seek through seekRequested (one per burst).
    Q_INVOKABLE void tap(double x, double width, double positionMs, double durationMs);
    // Any control was used: show and restart the auto-hide timer.
    Q_INVOKABLE void interact();
    Q_INVOKABLE void setPlaying(bool playing);
    Q_INVOKABLE void setEnded();
    Q_INVOKABLE void cancelGesture();

signals:
    void controlsChanged();
    void gestureChanged();
    void stepSecondsChanged();
    // Apply as ONE flush seek (NFR-PERF-03 "double-tap burst = 1 flush").
    void seekRequested(double positionMs);

private:
    qint64 now() const { return clock_.elapsed(); }
    void apply(const TapOutput &out);
    void rearm();
    void onTimer();

    TapGestures taps_;
    ControlsVisibility controls_;
    QElapsedTimer clock_;
    QTimer timer_;
};

}}
