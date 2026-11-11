// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#pragma once

// DL-03: join a downloaded video-only track and audio-only track into one file
// without re-encoding and without bundling ffmpeg. The production implementation
// (GstRemuxer) drives the device GStreamer muxers through the public C ABI loaded
// at run time; availability is the host side of experiment E6.

#include <QObject>
#include <QString>
#include <functional>

namespace nordtile { namespace video { namespace downloads {

enum class MuxContainer { Mp4, WebM, Matroska };

struct MuxerAvailability {
    bool loaded = false;       // GStreamer could be initialised
    bool mp4 = false;          // mp4mux or qtmux, with qtdemux
    bool webm = false;         // webmmux, with matroskademux
    bool matroska = false;     // matroskamux, with matroskademux
    QString detail;            // element names found, for diagnostics
};

class Remuxer {
public:
    using Completion = std::function<void(bool ok, const QString &error)>;
    virtual ~Remuxer() {}
    virtual MuxerAvailability availability() = 0;
    // Asynchronous; the completion runs on the caller's (QObject) thread. The
    // output is written to `outputPath + ".part"` and renamed on success.
    // One of the inputs may be empty: a one-track remux, used to rewrite a
    // fragmented (DASH) MP4/M4A as a flat file (moov + one mdat).
    virtual quint64 remux(const QString &videoPath, const QString &audioPath, const QString &outputPath,
                          MuxContainer container, Completion completion) = 0;
    virtual void cancel(quint64 id) = 0;
};

}}}
