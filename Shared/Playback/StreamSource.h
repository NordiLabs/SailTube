// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#pragma once

// Interface between a player and the instant-playback component that resolves
// and fetches YouTube streams (see docs/SHARED_PLAYBACK.md on the integration
// branch). Qt Core only: no QML, multimedia, GStreamer or application types,
// so the same header can be pinned by SailTune and SailTube.
//
// Producer: SailTune's streaming component (fast /player path, probe, cache,
// breaker, yt-dlp fallback, bounded Range fetcher). Until that lands,
// SailTube's playback component implements it with an adapter over its yt-dlp
// runner. Players code only against this header.
//
// Contract for every asynchronous call:
// - call on the owning thread; the completion runs there later, exactly once
//   (cancellation included), never from inside the call;
// - no completion after the implementation is destroyed;
// - URLs and headers are ephemeral: never persist or log them;
// - error values carry fixed classes and labels, never provider text.

#include <QByteArray>
#include <QList>
#include <QPair>
#include <QString>
#include <QStringList>
#include <QUrl>
#include <QVariantMap>
#include <functional>
#include <utility>
#include <vector>

namespace nordtile { namespace playback {

enum class StreamPurpose { Video, AudioOnly };

// Network class the quality ceiling was chosen for (SEL-02; part of the RES-10 key).
enum class NetworkClass { Unknown, Unmetered, Metered };

// Everything that changes the answer; implementations key their cache on all of it.
// Every field except `speculative` is in streamCacheKey() and outputAffectingFields() below
// (RES-10); a new field must be added to both.
struct StreamRequest {
    QString videoId;                     // 11 chars of [A-Za-z0-9_-]
    StreamPurpose purpose = StreamPurpose::Video;
    int maxHeight = 1080;                // 0 = no ceiling
    bool highFrameRate = false;          // >30 fps rungs only when explicitly wanted
    QStringList videoCodecs;             // preference order, e.g. {"avc1","vp09"}; empty = implementation default
    QStringList audioCodecs;             // e.g. {"opus","mp4a"}
    QString dubLanguage;                 // BCP-47; empty = original track
    bool hardwareDecode = false;
    bool speculative = false;            // prefetch: never starts helpers, may be dropped
    // Appended (playback component, RES-10 key fields):
    bool signedIn = false;               // never adds cookies to the primary resolve (RES-20); private is final only signed out (RES-09)
    bool poProvider = false;             // PO-token provider active (RES-08 step 4, RES-14)
    NetworkClass network = NetworkClass::Unknown; // network class of the ceiling (SEL-02)
    QString clientOverride;              // diagnostics-only client choice; empty = automatic (RES-10)
};

// RES-06 verdict of a format's URL. NoVerdict (network failure) is accepted but unverified: a
// refusal on the first playback request escalates at once.
enum class ProbeState { NotProbed, Accepted, NoVerdict };

struct StreamFormat {
    QString formatId;                    // provider id (itag), diagnostics and resume only
    QString container;                   // mp4, webm, m4a, ...
    QString videoCodec;                  // RFC 6381 string or "none"
    QString audioCodec;                  // RFC 6381 string or "none"
    int width = 0, height = 0;
    double fps = 0;
    double bitrateKbps = 0;
    int audioSampleRate = 0;
    qint64 contentLength = -1;           // bytes, -1 unknown
    QString language;
    bool originalAudio = false;
    bool dynamicRangeCompressed = false;
    QUrl url;
    QList<QPair<QByteArray, QByteArray>> headers;   // request headers the source must send
    // Appended (playback component):
    int languagePreference = -1;         // higher is preferred; the original/default track highest (SEL-04, SEL-05)
    bool descriptive = false;            // audio-description track, ranked last (SEL-04)
    bool premium = false;                // Premium rung: explicit menu choice only (SEL-02, SEL-03)
    bool hdr = false;                    // high dynamic range rung (SEL-02 tie-break)
    bool isHls = false;                  // HLS manifest: never proxied or probed (RES-17, RES-19)
    qint64 expiresAtUnixMs = 0;          // this URL's expire, 0 unknown (RES-10, RES-12)
    ProbeState probe = ProbeState::NotProbed; // RES-06

    bool hasVideo() const { return !videoCodec.isEmpty() && videoCodec != QLatin1String("none"); }
    bool hasAudio() const { return !audioCodec.isEmpty() && audioCodec != QLatin1String("none"); }
};

enum class ResolvePath { Cache, FastPath, Extractor, ExtractorWithJavaScript, Unknown };

// RES-19: live state of the answer.
enum class LiveState { NotLive, Live, Upcoming, PostLive, WasLive };

// Resolve phases in milliseconds, -1 = not measured (DIA-02 timelines).
struct ResolvePhases {
    qint64 queueMs = -1;                 // waiting for a worker / runner slot
    qint64 visitorMs = -1;               // visitor id mint (fast path, RES-05)
    qint64 requestMs = -1;               // the player request or extractor run
    qint64 probeMs = -1;                 // RES-06 probe join
};

// Scrub-preview storyboard: sprite sheets that cover the video in order. Each sheet holds
// columns x rows cells of width x height pixels, left to right then top to bottom; the last sheet
// may hold fewer. Sheets are plain image addresses on the provider's image host.
struct StoryboardSheet {
    QUrl url;
    qint64 durationMs = 0;
};
struct StreamStoryboard {
    int width = 0, height = 0;           // one cell, pixels
    int columns = 0, rows = 0;           // cells per sheet
    QList<StoryboardSheet> sheets;
    bool isEmpty() const { return sheets.isEmpty(); }
};

struct StreamSet {
    QString videoId;
    QList<StreamFormat> video;           // video-only, best first under the request
    QList<StreamFormat> audio;           // audio-only, best first under the request
    QList<StreamFormat> muxed;           // progressive fallback(s), best first
    QUrl hlsManifest;                    // live streams
    bool live = false;
    qint64 expiresAtUnixMs = 0;
    bool hasLoudness = false;
    double loudnessDb = 0;
    ResolvePath path = ResolvePath::Unknown;
    qint64 resolveMs = -1;
    // Appended (playback component):
    LiveState liveState = LiveState::NotLive; // `live` above is liveState == Live
    qint64 scheduledStartUnixMs = 0;     // upcoming start, 0 unknown (RES-19)
    ResolvePhases phases;                // DIA-02
    StreamStoryboard storyboard;         // scrub preview; empty when the answer carries none
};

enum class StreamErrorClass {
    None, Cancelled, Network, Timeout, BotCheck, SignInRequired, AgeRestricted,
    Upcoming, Unavailable, NoPlayableStream, RuntimeUnavailable, Failed,
    // Appended (playback component, RES-09):
    Removed, GeoBlocked, Private, TryLater, StillProcessing, Terminated, InvalidId,
    Busy, InvalidResponse,
    // Appended (playback component, RES-08/09): the answer had no usable URLs (SABR-only formats, a
    // failed n / signature challenge); a fuller extraction may still succeed. Not final.
    DeliveryChanged
};

struct StreamError {
    StreamErrorClass kind = StreamErrorClass::None;
    bool final = false;                  // no retry or widening (RES-09)
    QString label;                       // fixed diagnostics label, never provider text
    bool ok() const { return kind == StreamErrorClass::None; }
};

// RES-09: final classes get no retry, widening or fallback; "try again later" never is; private
// is final only when signed out.
inline bool isFinal(StreamErrorClass kind, bool signedIn) {
    switch (kind) {
    case StreamErrorClass::Removed: case StreamErrorClass::Terminated: case StreamErrorClass::Upcoming:
    case StreamErrorClass::GeoBlocked: case StreamErrorClass::InvalidId: case StreamErrorClass::Unavailable:
        return true;
    case StreamErrorClass::Private:
        return !signedIn;
    default:
        return false;
    }
}

// Fixed diagnostics label of a class (StreamError::label).
inline const char *errorLabel(StreamErrorClass kind) {
    switch (kind) {
    case StreamErrorClass::None: return "none";
    case StreamErrorClass::Cancelled: return "cancelled";
    case StreamErrorClass::Network: return "network";
    case StreamErrorClass::Timeout: return "timeout";
    case StreamErrorClass::BotCheck: return "bot_check";
    case StreamErrorClass::SignInRequired: return "sign_in_required";
    case StreamErrorClass::AgeRestricted: return "age_restricted";
    case StreamErrorClass::Upcoming: return "upcoming";
    case StreamErrorClass::Unavailable: return "unavailable";
    case StreamErrorClass::NoPlayableStream: return "no_playable_stream";
    case StreamErrorClass::RuntimeUnavailable: return "runtime_unavailable";
    case StreamErrorClass::Failed: return "failed";
    case StreamErrorClass::Removed: return "removed";
    case StreamErrorClass::GeoBlocked: return "geo_blocked";
    case StreamErrorClass::Private: return "private";
    case StreamErrorClass::TryLater: return "try_later";
    case StreamErrorClass::StillProcessing: return "still_processing";
    case StreamErrorClass::Terminated: return "terminated";
    case StreamErrorClass::InvalidId: return "invalid_id";
    case StreamErrorClass::Busy: return "busy";
    case StreamErrorClass::InvalidResponse: return "invalid_response";
    case StreamErrorClass::DeliveryChanged: return "delivery_changed";
    }
    return "failed";
}

// RES-09: short user-facing reason, never provider text or desktop-only advice. English source
// strings; the UI translates by class.
inline const char *userReason(StreamErrorClass kind) {
    switch (kind) {
    case StreamErrorClass::None: return "";
    case StreamErrorClass::Cancelled: return "Cancelled.";
    case StreamErrorClass::Network: return "The network is unavailable.";
    case StreamErrorClass::Timeout: return "Loading the video took too long.";
    case StreamErrorClass::BotCheck: return "YouTube is checking for automated access. Try again shortly.";
    case StreamErrorClass::SignInRequired: return "This video needs a signed-in account.";
    case StreamErrorClass::AgeRestricted: return "This video is age-restricted.";
    case StreamErrorClass::Upcoming: return "This video has not started yet.";
    case StreamErrorClass::Unavailable: return "This video is unavailable.";
    case StreamErrorClass::NoPlayableStream: return "No playable stream was offered for this video.";
    case StreamErrorClass::RuntimeUnavailable: return "The video resolver is not available.";
    case StreamErrorClass::Failed: return "The video could not be loaded.";
    case StreamErrorClass::Removed: return "This video has been removed.";
    case StreamErrorClass::GeoBlocked: return "This video is not available in your country.";
    case StreamErrorClass::Private: return "This video is private.";
    case StreamErrorClass::TryLater: return "YouTube asked to try again later.";
    case StreamErrorClass::StillProcessing: return "This stream has just ended and is still being processed.";
    case StreamErrorClass::Terminated: return "The account that posted this video has been terminated.";
    case StreamErrorClass::InvalidId: return "This is not a valid video link.";
    case StreamErrorClass::Busy: return "Too many videos are loading. Try again.";
    case StreamErrorClass::InvalidResponse: return "YouTube sent an unexpected answer.";
    case StreamErrorClass::DeliveryChanged: return "YouTube changed how this video is delivered.";
    }
    return "The video could not be loaded.";
}

// A StreamError of the class with final and label filled in (RES-09).
inline StreamError streamError(StreamErrorClass kind, bool signedIn) {
    StreamError e;
    e.kind = kind;
    e.final = isFinal(kind, signedIn);
    e.label = QLatin1String(errorLabel(kind));
    return e;
}

// RES-10 cache key: the video id plus every output-affecting field of the request (all fields
// but `speculative`). Strings are length-prefixed, so no two distinct requests share a key.
inline QByteArray streamCacheKey(const StreamRequest &r) {
    QByteArray k("r1|");
    const auto text = [&k](const QString &s) {
        const QByteArray u = s.toUtf8();
        k += QByteArray::number(u.size());
        k += ':';
        k += u;
        k += '|';
    };
    const auto number = [&k](qint64 v) {
        k += QByteArray::number(v);
        k += '|';
    };
    const auto list = [&](const QStringList &l) {
        number(l.size());
        for (const QString &s : l) text(s);
    };
    text(r.videoId);
    number(static_cast<int>(r.purpose));
    number(r.maxHeight);
    number(r.highFrameRate ? 1 : 0);
    list(r.videoCodecs);
    list(r.audioCodecs);
    text(r.dubLanguage);
    number(r.hardwareDecode ? 1 : 0);
    number(r.signedIn ? 1 : 0);
    number(r.poProvider ? 1 : 0);
    number(static_cast<int>(r.network));
    text(r.clientOverride);
    return k;
}

// RES-10 settings registry of StreamRequest: one entry per output-affecting field, the setting it
// derives from, and a mutation that changes only that field (the invariant test fails when a
// field is missing from streamCacheKey()).
struct StreamRequestField {
    const char *name;
    const char *setting;
    void (*mutate)(StreamRequest &);
};
const int streamRequestFieldCount = 11; // bump together with StreamRequest and the list below

inline const std::vector<StreamRequestField> &outputAffectingFields() {
    static const std::vector<StreamRequestField> fields = {
        {"purpose", "playback.listenOnly", [](StreamRequest &r) {
            r.purpose = r.purpose == StreamPurpose::Video ? StreamPurpose::AudioOnly : StreamPurpose::Video; }},
        {"maxHeight", "quality.ceiling", [](StreamRequest &r) { r.maxHeight = r.maxHeight == 480 ? 720 : 480; }},
        {"highFrameRate", "quality.highFrameRate", [](StreamRequest &r) { r.highFrameRate = !r.highFrameRate; }},
        {"videoCodecs", "playback.codecPolicy", [](StreamRequest &r) { r.videoCodecs << QStringLiteral("av01"); }},
        {"audioCodecs", "playback.codecPolicy", [](StreamRequest &r) { r.audioCodecs << QStringLiteral("vorbis"); }},
        {"dubLanguage", "playback.dubLanguage", [](StreamRequest &r) { r.dubLanguage += QLatin1Char('x'); }},
        {"hardwareDecode", "playback.hardwareDecode", [](StreamRequest &r) { r.hardwareDecode = !r.hardwareDecode; }},
        {"signedIn", "account.signedIn", [](StreamRequest &r) { r.signedIn = !r.signedIn; }},
        {"poProvider", "resolver.poProvider", [](StreamRequest &r) { r.poProvider = !r.poProvider; }},
        {"network", "quality.networkClass", [](StreamRequest &r) {
            r.network = r.network == NetworkClass::Metered ? NetworkClass::Unmetered : NetworkClass::Metered; }},
        {"clientOverride", "resolver.clientOverride", [](StreamRequest &r) { r.clientOverride += QLatin1Char('x'); }},
    };
    return fields;
}

class StreamResolver {
public:
    using Completion = std::function<void(const StreamSet &, const StreamError &)>;
    virtual ~StreamResolver() = default;
    virtual quint64 resolve(const StreamRequest &, Completion) = 0;
    virtual void prefetch(const StreamRequest &) = 0;
    virtual void cancel(quint64 operation) = 0;
    // A source was refused at byte 0 or expired: drop cached answers for the video.
    virtual void invalidate(const QString &videoId) = 0;
    // Fixed keys and numbers only (path counters, median timings, breaker state).
    virtual QVariantMap diagnostics() const = 0;
    // Appended (playback component): this format of the video was refused at byte 0 (RES-07, RES-12).
    // An implementation with a breaker counts a strike when the URL came from its fast path and
    // still had a minute or more to live. Default: invalidate(videoId).
    virtual void reportRefused(const QString &videoId, const StreamFormat &) { invalidate(videoId); }
    // Appended (playback component, RES-09): asks why a video that another path called final cannot be
    // played. At most one extraction pass: no widening, retry or fallback. Same completion and
    // cancel() contract as resolve(). Default: resolve().
    virtual quint64 nameFinal(const StreamRequest &request, Completion completion) {
        return resolve(request, std::move(completion));
    }
};

class StreamFetcher {
public:
    // localUrl is what the pipeline opens (loopback URL with a session token, or
    // an in-process source URI); empty on error.
    using Opened = std::function<void(const QUrl &localUrl, const StreamError &)>;
    virtual ~StreamFetcher() = default;
    virtual quint64 open(const StreamFormat &, Opened) = 0;
    virtual void close(quint64 stream) = 0;
    virtual QVariantMap diagnostics() const = 0;
};

}}
