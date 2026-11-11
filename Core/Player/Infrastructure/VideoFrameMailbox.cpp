// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "VideoFrameMailbox.h"

#include <QMutexLocker>
#include <cmath>

namespace nordtile { namespace player {

QSize VideoFrame::displaySize() const {
    if (!valid()) return QSize();
    const double par = pixelAspect > 0 ? pixelAspect : 1.0;
    return QSize(static_cast<int>(std::lround(width * par)), height);
}

QSize HardwareVideoGeometry::displaySize() const {
    if (!valid()) return QSize();
    const double par = pixelAspect > 0 ? pixelAspect : 1.0;
    return QSize(static_cast<int>(std::lround(width * par)), height);
}

void VideoFrameMailbox::setHardwareSource(const std::shared_ptr<HardwareFrameSource> &source) {
    std::shared_ptr<HardwareFrameSource> previous;
    {
        QMutexLocker lock(&mutex_);
        if (hardware_ == source) return;
        previous = hardware_;  // released outside the lock
        hardware_ = source;
    }
    notify();
}

std::shared_ptr<HardwareFrameSource> VideoFrameMailbox::hardwareSource() const {
    QMutexLocker lock(&mutex_);
    return hardware_;
}

void VideoFrameMailbox::post(const VideoFrame &frame) {
    VideoFrame previous;
    {
        QMutexLocker lock(&mutex_);
        previous = frame_; // released outside the lock
        frame_ = frame;
        frame_.serial = ++serial_;
    }
    notify();
}

VideoFrame VideoFrameMailbox::latest() const {
    QMutexLocker lock(&mutex_);
    return frame_;
}

void VideoFrameMailbox::clear() {
    VideoFrame previous;
    {
        QMutexLocker lock(&mutex_);
        previous = frame_;
        frame_ = VideoFrame();
        ++serial_;
    }
    notify();
}

quint64 VideoFrameMailbox::serial() const {
    QMutexLocker lock(&mutex_);
    return serial_;
}

int VideoFrameMailbox::addListener(const Listener &listener) {
    QMutexLocker lock(&listenerMutex_);
    const int id = nextListener_++;
    listeners_.push_back(std::make_pair(id, listener));
    return id;
}

void VideoFrameMailbox::removeListener(int id) {
    QMutexLocker lock(&listenerMutex_);
    for (auto it = listeners_.begin(); it != listeners_.end(); ++it)
        if (it->first == id) { listeners_.erase(it); return; }
}

void VideoFrameMailbox::notify() {
    // Listeners only post events; they must not call back into the mailbox.
    QMutexLocker lock(&listenerMutex_);
    for (const auto &l : listeners_) if (l.second) l.second();
}

}}
