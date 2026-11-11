// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "OverlayController.h"

#include <algorithm>

namespace nordtile { namespace player {

OverlayController::OverlayController(QObject *parent) : QObject(parent) {
    clock_.start();
    timer_.setSingleShot(true);
    connect(&timer_, &QTimer::timeout, this, &OverlayController::onTimer);
}

void OverlayController::setMenuOpen(bool open) {
    if (open == controls_.menuOpen()) return;
    controls_.setMenuOpen(now(), open);
    emit controlsChanged();
    rearm();
}

void OverlayController::setStepSeconds(int seconds) {
    const int before = taps_.stepSeconds();
    taps_.setStepSeconds(seconds);
    if (taps_.stepSeconds() != before) emit stepSecondsChanged();
}

void OverlayController::tap(double x, double width, double positionMs, double durationMs) {
    const bool wasActive = taps_.gestureActive();
    const TapOutput out = taps_.tap(now(), tapSideFor(x, width), static_cast<qint64>(positionMs),
                                    static_cast<qint64>(durationMs));
    apply(out);
    if (taps_.gestureActive() || wasActive) emit gestureChanged();
    rearm();
}

void OverlayController::interact() {
    const bool before = controls_.visible();
    controls_.interact(now());
    if (controls_.visible() != before) emit controlsChanged();
    rearm();
}

void OverlayController::setPlaying(bool playing) {
    const bool before = controls_.visible();
    controls_.setPlaying(now(), playing);
    if (controls_.visible() != before) emit controlsChanged();
    rearm();
}

void OverlayController::setEnded() {
    const bool before = controls_.visible();
    controls_.setEnded();
    if (controls_.visible() != before) emit controlsChanged();
    rearm();
}

void OverlayController::cancelGesture() {
    const bool was = taps_.gestureActive();
    taps_.cancel();
    if (was) emit gestureChanged();
    rearm();
}

void OverlayController::apply(const TapOutput &out) {
    if (out.toggleControls) {
        controls_.toggle(now());
        emit controlsChanged();
    }
    if (out.seek) emit seekRequested(static_cast<double>(out.seekTargetMs));
    if (out.gestureEnded) emit gestureChanged();
}

void OverlayController::onTimer() {
    const qint64 t = now();
    const bool wasActive = taps_.gestureActive();
    apply(taps_.advance(t));
    if (wasActive != taps_.gestureActive()) emit gestureChanged();
    const bool before = controls_.visible();
    if (controls_.advance(t) || controls_.visible() != before) emit controlsChanged();
    rearm();
}

void OverlayController::rearm() {
    qint64 next = -1;
    const qint64 a = taps_.nextDeadlineMs(), b = controls_.nextDeadlineMs();
    if (a >= 0) next = a;
    if (b >= 0) next = next < 0 ? b : std::min(next, b);
    if (next < 0) { timer_.stop(); return; }
    timer_.start(static_cast<int>(std::max<qint64>(0, next - now())));
}

}}
