// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#pragma once

// DL-03 / E6: joins a video-only and an audio-only download into one file with
// the device GStreamer muxers, without re-encoding. GStreamer is loaded at run
// time (QLibrary) through a minimal mirror of its public C ABI, so the package
// has no build or link dependency on it. Without GStreamer, or without the
// muxer for a container, remux() fails at once and the engine keeps the pair.
// Writes only `outputPath.part` (and, for MP4, `outputPath.faststart.part`)
// beside the output; nothing goes to /tmp (PLT-09).

#include "Remuxer.h"

#include <QHash>
#include <QMutex>
#include <QObject>
#include <QString>
#include <atomic>
#include <memory>

namespace nordtile { namespace video { namespace downloads {

namespace gstremux { struct Api; class Worker; }

class GstRemuxer : public QObject, public Remuxer {
    Q_OBJECT
public:
    explicit GstRemuxer(QObject *parent = nullptr);
    // `gstreamerLibrary` is a QLibrary name (default "gstreamer-1.0", soname
    // version 0); tests pass a bogus one to simulate a device without GStreamer.
    explicit GstRemuxer(const QString &gstreamerLibrary, QObject *parent = nullptr);
    ~GstRemuxer() override; // cancels running jobs; their completions are dropped

    // Probes once (gst_init + element factories), then returns the cached result.
    MuxerAvailability availability() override;

    // Each job runs on its own worker thread. The completion runs on the thread
    // that called remux() (it needs an event loop), never inside remux() itself.
    // A cancelled job completes with ok=false and error "cancelled".
    quint64 remux(const QString &videoPath, const QString &audioPath, const QString &outputPath,
                  MuxContainer container, Completion completion) override;
    void cancel(quint64 id) override;

    // Bus wait limit for inputs of `inputBytes` in total: 60 s + 10 s per 100 MiB.
    static qint64 timeoutMsFor(qint64 inputBytes);

private:
    friend class gstremux::Worker;
    void probeLocked();
    void failLater(const Completion &completion, const QString &error);

    QString m_library;
    QMutex m_mutex;
    bool m_probed = false;
    MuxerAvailability m_availability;
    QString m_mp4Muxer; // "mp4mux", or "qtmux" when only that exists
    std::shared_ptr<gstremux::Api> m_api;
    std::atomic<quint64> m_nextId;
    QHash<quint64, gstremux::Worker *> m_jobs; // guarded by m_mutex
};

}}}
