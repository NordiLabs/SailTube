// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#pragma once

// Binding of SharedStreamSource to SailTune's pinned component (Shared/SailTune/Core/SharedPlayback,
// Shared/SharedPlayback.pri). Only the component's public headers are named here.
//
// App wiring, off the UI thread (SharedStreamSourceThreaded.h; behaviour in SharedStreamSource.h):
//   YtDlpStreamResolver ytdlp(&service);                  // GUI thread (its runner), extractor fallback
//   QPointer<YtDlpStreamResolver> guard(&ytdlp);
//   const auto gui = player::threaded::Channel::here();
//   player::ThreadedStreamSource::Options options;        // monotonicMs + timing hooks
//   player::ThreadedStreamSource source([gui, guard]() {  // the factory runs on the worker thread
//       player::SailTuneBackend::Config c;
//       c.fallbackChannel = gui;
//       c.fallback = [guard]() { return static_cast<playback::StreamResolver *>(guard.data()); };
//       return new player::SailTuneBackend(c);
//   }, options);
//   source.warmUp();                                      // once the first screen is up
//   source.resolve(request, completion) -> StreamSet; source.fetcher()->open(format, opened) -> local URL
//   source.reportRefused(videoId, format)                 // a byte-0 refusal: the only strike
//   source.reportFault(videoId.toStdString(), player::SourceFault::Stalled)   // or Connection, Abandoned:
//                                                         // never a strike, the fast path stays
//   source.invalidate(videoId)                            // a failure nobody explained (strike rules)
// Destroy `source` before `ytdlp`.
//
// The pinned MediaProxy (5611210) fetches the first 256 KiB and the last 16 KiB of every stream at
// open(): Config::proxy.prebufferBytes / tailBytes set to 0 switch that off for the application.

#include "SharedStreamSource.h"
#include "SharedStreamSourceThreaded.h"
#include "../../../Shared/SailTune/Core/SharedPlayback/Domain/StreamTypes.h"
#include "../../../Shared/SailTune/Core/SharedPlayback/Infrastructure/FastStreamResolver.h"
#include "../../../Shared/SailTune/Core/SharedPlayback/Infrastructure/MediaProxy.h"
#include <QElapsedTimer>
#include <QTimer>

namespace nordtile { namespace player {

struct SailTuneSharedPlayback {
    typedef music::sharedplayback::infrastructure::FastStreamResolver Resolver;
    typedef music::sharedplayback::infrastructure::MediaProxy Proxy;
    typedef music::sharedplayback::domain::StreamRequest Request;
    typedef music::sharedplayback::domain::StreamFormat Format;
    typedef music::sharedplayback::domain::StreamSet Set;
    typedef music::sharedplayback::domain::ResolveOutcome Outcome;
    typedef music::sharedplayback::domain::FailureKind FailureKind;
    typedef music::sharedplayback::domain::ResolvePath Path;
};

typedef SharedStreamSource<SailTuneSharedPlayback> SailTuneStreamSource;

// FastStreamResolver options for use with SailTuneStreamSource: the defaults, the component's
// own probe on (it follows up to 3 googlevideo redirects since fd49627; the adapter adds no probe
// of its own, it only asks the component to probe a rung it delivers instead of the component's
// top one). Pinned at 5611210: visitor and player timeouts are 8 s and 10 s.
inline music::sharedplayback::infrastructure::FastStreamResolver::Options componentOptions() {
    return music::sharedplayback::infrastructure::FastStreamResolver::Options();
}

// The worker-thread side for ThreadedStreamSource: the component, the proxy, the adapter and a
// queued hop to the extractor fallback on its own thread. Created and deleted on the worker thread.
class SailTuneBackend final : public StreamSourceBackend {
public:
    struct Config {
        music::sharedplayback::infrastructure::FastStreamResolver::Options fast = componentOptions();
        music::sharedplayback::infrastructure::MediaProxy::Options proxy;
        SharedStreamSourceOptions source;
        std::shared_ptr<threaded::Channel> fallbackChannel;      // the fallback's thread
        std::function<playback::StreamResolver *()> fallback;    // looked up there (e.g. a QPointer)
        int warmUpTimeoutMs = 9000;                              // above the visitor request's 8 s
    };

    explicit SailTuneBackend(const Config &config)
        : config_(config), fast_(config.fast), proxy_(config.proxy),
          fallback_(config.fallbackChannel && config.fallback ? new threaded::QueuedResolver(config.fallbackChannel, config.fallback) : nullptr),
          source_(&fast_, &proxy_, fallback_.get(), config.source) {}

    playback::StreamResolver *resolver() override { return &source_; }
    playback::StreamFetcher *fetcher() override { return source_.fetcher(); }
    SailTuneStreamSource &source() { return source_; }

    // The component has no warm-up completion (docs/SHARED_REQUESTS.md S-19): its visitor id is
    // polled every 20 ms until it is there or warmUpTimeoutMs passed.
    void warmUp(std::function<void(bool ok, qint64 visitorMs)> done) override {
        if (fast_.hasVisitor()) {
            QTimer::singleShot(0, &source_, [done]() { done(true, 0); });
            return;
        }
        source_.warmUp();
        std::shared_ptr<QElapsedTimer> timer = std::make_shared<QElapsedTimer>();
        timer->start();
        poll(timer, done);
    }

private:
    void poll(const std::shared_ptr<QElapsedTimer> &timer, const std::function<void(bool, qint64)> &done) {
        QTimer::singleShot(20, &source_, [this, timer, done]() {
            if (fast_.hasVisitor()) { done(true, timer->elapsed()); return; }
            if (timer->elapsed() >= config_.warmUpTimeoutMs) { done(false, timer->elapsed()); return; }
            poll(timer, done);
        });
    }

    Config config_;
    music::sharedplayback::infrastructure::FastStreamResolver fast_;
    music::sharedplayback::infrastructure::MediaProxy proxy_;
    std::unique_ptr<threaded::QueuedResolver> fallback_;
    SailTuneStreamSource source_; // declared last: destroyed first
};

}}
