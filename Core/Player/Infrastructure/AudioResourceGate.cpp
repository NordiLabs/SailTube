// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "AudioResourceGate.h"

#ifndef NORDTILE_NO_AUDIORESOURCE
#include <audioresource-qt.h>
#endif

namespace nordtile { namespace player {

AudioResourceGate::AudioResourceGate(QObject *parent) : QObject(parent) {
#ifndef NORDTILE_NO_AUDIORESOURCE
    resource_ = new AudioResourceQt::AudioResource(this, AudioResourceQt::AudioResource::MediaType);
    connect(resource_, &AudioResourceQt::AudioResource::acquiredChanged, this, &AudioResourceGate::sync);
#endif
}

AudioResourceGate::~AudioResourceGate() {
#ifndef NORDTILE_NO_AUDIORESOURCE
    if (resource_ && acquired_) resource_->release();
#endif
}

void AudioResourceGate::request() {
    wanted_ = true;
#ifndef NORDTILE_NO_AUDIORESOURCE
    if (resource_->isAcquired()) { sync(); if (acquired_) emit granted(); return; }
    resource_->acquire();
#else
    const bool changed = !acquired_;
    acquired_ = true;
    if (changed) emit acquiredChanged();
    emit granted();
#endif
}

void AudioResourceGate::release() {
    wanted_ = false;
#ifndef NORDTILE_NO_AUDIORESOURCE
    resource_->release();
#else
    if (acquired_) { acquired_ = false; emit acquiredChanged(); }
#endif
}

void AudioResourceGate::sync() {
#ifndef NORDTILE_NO_AUDIORESOURCE
    const bool now = resource_->isAcquired();
    if (now == acquired_) return;
    acquired_ = now;
    emit acquiredChanged();
    if (now) { if (wanted_) emit granted(); }
    else if (wanted_) emit lost(); // taken away by the policy (call, alarm)
#endif
}

}}
