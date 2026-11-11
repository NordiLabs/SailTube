// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "PlaybackSession.h"
#include "AppSettings.h"
#include "ErrorText.h"
#include "PlaybackCaptions.h"
#include "PlaybackTiming.h"
#include "Core/Player/Domain/MediaControlsPolicy.h"
#include "Core/Player/Infrastructure/AudioResourceGate.h"
#include "Core/Player/Infrastructure/HardwareDecodeGate.h"
#include "Core/Player/Infrastructure/SharedStreamSourceThreaded.h"
#include "Core/Player/Infrastructure/VideoPlayer.h"

#include <QDate>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QLocale>
#include <QMetaMethod>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QRegularExpression>
#include <QUrl>

#include <algorithm>
#include <cmath>

namespace nordtile { namespace video { namespace sailfish {

namespace {
QString s(const std::string &v) { return QString::fromStdString(v); }
QString s_qs(const std::string &v) { return QString::fromStdString(v); }  // inside Port, `s` is the session
std::string u(const QString &v) { return v.toUtf8().toStdString(); }

// Container family ("mp4", "m4a", "webm") of a shared format: its container (or MIME subtype),
// else implied by the codecs.
std::string containerFor(const playback::StreamFormat &f) {
    QString c = f.container.trimmed().toLower();
    const int slash = c.indexOf(QLatin1Char('/'));
    if (slash >= 0) c = c.mid(slash + 1);
    const int semicolon = c.indexOf(QLatin1Char(';'));
    if (semicolon >= 0) c = c.left(semicolon).trimmed();
    if (!c.isEmpty()) return u(c);
    const media::VideoCodec v = media::parseVideoCodec(u(f.videoCodec));
    const media::AudioCodec a = media::parseAudioCodec(u(f.audioCodec));
    if (v == media::VideoCodec::Vp9 || v == media::VideoCodec::Vp8 || a == media::AudioCodec::Opus || a == media::AudioCodec::Vorbis)
        return "webm";
    return f.hasVideo() ? "mp4" : "m4a";
}

media::LiveStatus liveStatusOf(playback::LiveState s) {
    switch (s) {
    case playback::LiveState::Live: return media::LiveStatus::IsLive;
    case playback::LiveState::Upcoming: return media::LiveStatus::Upcoming;
    case playback::LiveState::PostLive: return media::LiveStatus::PostLive;
    case playback::LiveState::WasLive: return media::LiveStatus::WasLive;
    case playback::LiveState::NotLive: break;
    }
    return media::LiveStatus::NotLive;
}

int branchSlot(player::Branch b) {
    return b == player::Branch::Video ? 0 : b == player::Branch::Audio ? 1 : 2;
}

bool loopback(const QUrl &url) {
    const QString host = url.host();
    return host == QLatin1String("127.0.0.1") || host == QLatin1String("localhost") || host == QLatin1String("::1");
}

const char *modeName(player::PlayMode m) {
    switch (m) {
    case player::PlayMode::Dual: return "dual";
    case player::PlayMode::Muxed: return "muxed";
    case player::PlayMode::Hls: return "hls";
    case player::PlayMode::Local: return "local";
    case player::PlayMode::AudioOnly: return "audio";
    }
    return "other";
}

const char *containerKey(const player::VideoPlayer *p) {
    const player::PipelinePlan &plan = p->plan();
    if (plan.mode == player::PlayMode::Hls) return "hls";
    if (plan.mode != player::PlayMode::Dual) return "muxed";
    for (std::size_t i = 0; i < plan.branches.size(); ++i)
        if (plan.branches[i].branch == player::Branch::Video)
            return plan.branches[i].container == player::Container::IsoBmff ? "mp4" : "webm";
    return "webm";
}

bool validArtworkId(const QString &id) {
    if (id.isEmpty() || id.size() > 64) return false;
    for (const QChar c : id)
        if (!(c.isLetterOrNumber() && c.unicode() < 128) && c != QLatin1Char('-') && c != QLatin1Char('_')) return false;
    return true;
}
}

// ---- Shared stream source <-> selection mapping ---------------------------------------------

media::MediaFormat mediaFormatFor(const playback::StreamFormat &f) {
    media::MediaFormat m;
    m.id = u(f.formatId);
    m.container = containerFor(f);
    m.url = f.url.toEncoded().toStdString();
    m.protocol = f.isHls ? media::Protocol::Hls
                         : f.url.scheme() == QLatin1String("https") ? media::Protocol::Https : media::Protocol::Other;
    for (const QPair<QByteArray, QByteArray> &h : f.headers) m.headers[h.first.toStdString()] = h.second.toStdString();
    m.videoCodecTag = f.hasVideo() ? u(f.videoCodec) : std::string("none");
    m.audioCodecTag = f.hasAudio() ? u(f.audioCodec) : std::string("none");
    m.videoCodec = media::parseVideoCodec(m.videoCodecTag);
    m.audioCodec = media::parseAudioCodec(m.audioCodecTag);
    m.width = f.width;
    m.height = f.height;
    m.fps = f.fps;
    m.totalKbps = f.bitrateKbps;
    if (!f.hasVideo()) m.audioKbps = f.bitrateKbps;
    m.audioSampleRate = f.audioSampleRate;
    m.fileSize = f.contentLength > 0 ? f.contentLength : 0;
    m.language = u(f.language);
    // SEL-04 language classes (AudioTrackPolicy.h): original +10, descriptive -10.
    m.languagePreference = f.descriptive ? -10 : (f.originalAudio && f.languagePreference < 10) ? 10 : f.languagePreference;
    m.dynamicRangeCompressed = f.dynamicRangeCompressed;
    m.highDynamicRange = f.hdr;
    m.premium = f.premium;
    return m;
}

media::SelectionInput selectionInputFor(const playback::StreamSet &set) {
    media::SelectionInput in;
    for (const QList<playback::StreamFormat> *list : {&set.video, &set.audio, &set.muxed})
        for (const playback::StreamFormat &f : *list) in.formats.push_back(mediaFormatFor(f));
    in.liveStatus = liveStatusOf(set.liveState);
    if (set.live && in.liveStatus == media::LiveStatus::NotLive) in.liveStatus = media::LiveStatus::IsLive;
    if (set.hlsManifest.isValid() && !set.hlsManifest.isEmpty()) in.manifestUrl = set.hlsManifest.toEncoded().toStdString();
    in.scheduledStartUnix = set.scheduledStartUnixMs / 1000;
    return in;
}

playback::StreamRequest streamRequestFor(const ResolveRequest &r, int networkClass, bool poProvider) {
    playback::StreamRequest q;
    q.videoId = s(r.videoId);
    q.purpose = r.audioOnly ? playback::StreamPurpose::AudioOnly : playback::StreamPurpose::Video;
    q.maxHeight = r.maxHeight;
    q.highFrameRate = r.highFrameRate;
    for (const std::string &c : r.videoCodecs) q.videoCodecs << s(c);
    for (const std::string &c : r.audioCodecs) q.audioCodecs << s(c);
    q.dubLanguage = s(r.dubLanguage);
    q.hardwareDecode = r.hardwareDecode;
    q.speculative = r.speculative;
    q.signedIn = false;  // RES-20: never cookies on the primary resolve
    q.poProvider = poProvider;  // RES-14: only while the helper is ready
    q.network = networkClass == 0 ? playback::NetworkClass::Unmetered
              : networkClass == 1 ? playback::NetworkClass::Metered : playback::NetworkClass::Unknown;
    return q;
}

std::vector<player::SponsorSegment> sponsorSegmentsFor(const QVariantList &segments, double unitMs) {
    std::vector<player::SponsorSegment> list;
    for (const QVariant &v : segments) {
        const QVariantMap m = v.toMap();
        player::SponsorCategory category;
        if (!player::sponsorCategoryFromApiName(u(m.value(QStringLiteral("category")).toString()), &category)) continue;
        // SB-02 decides skips and highlights only; mute and whole-video labels are not skips.
        const QString action = m.value(QStringLiteral("actionType")).toString();
        if (!action.isEmpty() && action != QLatin1String("skip") && action != QLatin1String("poi")) continue;
        const std::int64_t start = static_cast<std::int64_t>(m.value(QStringLiteral("start")).toDouble() * unitMs);
        std::int64_t end = static_cast<std::int64_t>(m.value(QStringLiteral("end")).toDouble() * unitMs);
        if (player::sponsorCategoryInfo(category).highlight) end = start;
        if (start < 0 || end < start) continue;
        list.push_back(player::SponsorSegment(start, end, category));
    }
    return list;
}

Storyboard storyboardFor(const QVariantMap &m) {
    Storyboard b;
    b.width = m.value(QStringLiteral("width")).toInt();
    b.height = m.value(QStringLiteral("height")).toInt();
    b.columns = m.value(QStringLiteral("columns")).toInt();
    b.rows = m.value(QStringLiteral("rows")).toInt();
    for (const QVariant &v : m.value(QStringLiteral("fragments")).toList()) {
        const QVariantMap f = v.toMap();
        StoryboardFragment fragment;
        fragment.url = u(f.value(QStringLiteral("url")).toString());
        fragment.durationMs = static_cast<std::int64_t>(f.value(QStringLiteral("durationMs")).toDouble());
        // Only https images (the overlay loads them as they are).
        if (QUrl(s(fragment.url)).scheme() != QLatin1String("https") || fragment.durationMs <= 0)
            return Storyboard();
        b.fragments.push_back(fragment);
    }
    if (!storyboardCell(b, 0, nullptr)) return Storyboard();
    return b;
}

Storyboard storyboardFor(const playback::StreamStoryboard &board) {
    Storyboard b;
    b.width = board.width;
    b.height = board.height;
    b.columns = board.columns;
    b.rows = board.rows;
    for (const playback::StoryboardSheet &sheet : board.sheets) {
        if (sheet.url.scheme() != QLatin1String("https") || sheet.durationMs <= 0) return Storyboard();
        StoryboardFragment fragment;
        fragment.url = sheet.url.toEncoded().toStdString();
        fragment.durationMs = sheet.durationMs;
        b.fragments.push_back(fragment);
    }
    if (!storyboardCell(b, 0, nullptr)) return Storyboard();
    return b;
}

QString audioTrackLabel(const media::AudioTrackOption &o) {
    QString name;
    if (o.language.empty()) {
        //: Label of the untagged or default audio track in the audio-track menu
        name = PlaybackSession::tr("Default");
    } else {
        const QString tag = s(o.language);
        const QLocale locale(QString(tag).replace(QLatin1Char('-'), QLatin1Char('_')));
        name = locale.language() != QLocale::C ? locale.nativeLanguageName() : QString();
        if (name.isEmpty()) name = tag;
        else name[0] = name[0].toUpper();
    }
    // A translator comment between an unbraced `if` and its statement is dropped by lupdate (Qt 5.6):
    // keep the braces.
    if (o.original) {
        //: Audio track menu: %1 is the language name of the original audio track
        return PlaybackSession::tr("%1 (original)").arg(name);
    }
    if (o.descriptive) {
        //: Audio track menu: %1 is the language name of a descriptive-audio track
        return PlaybackSession::tr("%1 (audio description)").arg(name);
    }
    return name;
}

ResolveAnswer::Error resolveErrorKind(const playback::StreamError &e) {
    using C = playback::StreamErrorClass;
    if (e.ok()) return ResolveAnswer::None;
    // A bare "unavailable" (one client's answer, not a final reason): the one fresh re-resolve may
    // still find the video (RES-09). The final Unavailable keeps final set.
    if (e.kind == C::Unavailable && !e.final) return ResolveAnswer::Retryable;
    if (e.final || playback::isFinal(e.kind, false)) return ResolveAnswer::Final;
    // The resolver runtime's temporary-file budget: asking again by itself meets the same budget
    // (two more extractor runs for nothing); the user's Retry stays.
    if (e.kind == C::Failed && e.label == QLatin1String("storage_limit")) return ResolveAnswer::Final;
    switch (e.kind) {
    // The resolver already ran its whole ladder (RES-08); asking again cannot change these.
    case C::Cancelled: case C::RuntimeUnavailable: case C::SignInRequired: case C::AgeRestricted:
    case C::StillProcessing: case C::Private:
        return ResolveAnswer::Final;
    default:
        return ResolveAnswer::Retryable;  // network, timeout, bot check, try later, busy, delivery changed, ...
    }
}

QString streamErrorText(playback::StreamErrorClass kind) {
    using C = playback::StreamErrorClass;
    using resolver::ErrorCode;
    switch (kind) {
    case C::None: case C::Cancelled: return QString();
    case C::Network: return errorText(ErrorCode::Network);
    case C::Timeout: return errorText(ErrorCode::Timeout);
    case C::BotCheck: return errorText(ErrorCode::BotCheck);
    case C::AgeRestricted: return errorText(ErrorCode::AgeRestricted);
    case C::Private: return errorText(ErrorCode::PrivateVideo);
    case C::Unavailable: return errorText(ErrorCode::VideoUnavailable);
    case C::Upcoming: return errorText(ErrorCode::Upcoming);
    case C::NoPlayableStream: return errorText(ErrorCode::NoPlayableStream);
    case C::RuntimeUnavailable: return errorText(ErrorCode::RuntimeUnavailable);
    case C::Busy: return errorText(ErrorCode::Busy);
    case C::InvalidResponse: return errorText(ErrorCode::InvalidResponse);
    //: RES-09: the extraction failed (not a device limit); a later yt-dlp update may fix it
    case C::Failed: return QCoreApplication::translate("SailTubeErrors", "The video could not be extracted. Try again; if it keeps failing, SailTube's resolver may need an update.");
    case C::DeliveryChanged:
        //: RES-09: the answer had no usable stream addresses (SABR-only formats, a failed player challenge)
        return QCoreApplication::translate("SailTubeErrors", "YouTube changed how this video is delivered. Try again later; SailTube's resolver may need an update.");
    case C::SignInRequired: return QCoreApplication::translate("SailTubeErrors", "This video needs a signed-in account.");
    case C::Removed: return QCoreApplication::translate("SailTubeErrors", "This video has been removed.");
    case C::GeoBlocked: return QCoreApplication::translate("SailTubeErrors", "This video is not available in your country.");
    //: RES-09: YouTube is rate limiting (HTTP 429, "try again later")
    case C::TryLater: return QCoreApplication::translate("SailTubeErrors", "YouTube is limiting requests right now. Try again in a minute.");
    case C::StillProcessing:
        return QCoreApplication::translate("SailTubeErrors", "This stream has just ended and is still being processed.");
    case C::Terminated:
        return QCoreApplication::translate("SailTubeErrors", "The account that posted this video has been terminated.");
    case C::InvalidId: return QCoreApplication::translate("SailTubeErrors", "This is not a valid video link.");
    }
    return errorText(ErrorCode::Failed);
}

bool refusalAtByteZero(const QString &reason, bool network, bool sawFirstByte, int httpStatus) {
    if (!network || sawFirstByte) return false;
    // The HTTP source's own status (its error details, VideoPlayer::failed) decides when known.
    if (httpStatus > 0) return httpStatus >= 400;
    // Fallback on the text: a "(403)"-style status, or the status phrase GStreamer's HTTP source
    // uses as the error message. Connection failures, DNS errors and timeouts carry neither and
    // are network deaths, not strikes (RES-07).
    static const QRegularExpression status(QStringLiteral("\\(([1-5][0-9][0-9])\\)"));
    const QRegularExpressionMatch m = status.match(reason);
    if (m.hasMatch()) return m.captured(1).toInt() >= 400;
    static const char *const phrases[] = {"forbidden", "not authorized", "unauthorized", "not found", "gone",
                                          "too many requests", "service unavailable"};
    const QString lower = reason.toLower();
    for (const char *p : phrases)
        if (lower.contains(QLatin1String(p))) return true;
    return false;
}

// ---- Session ----------------------------------------------------------------------------------

PlaybackSession::PlaybackSession(playback::StreamResolver *resolver, playback::StreamFetcher *fetcher, AppSettings *settings,
                                 const media::DecoderCapabilities &capabilities, bool keepAliveAvailable, QObject *parent)
    : QObject(parent), resolver_(resolver), fetcher_(fetcher), settings_(settings), capabilities_(capabilities), port_(this),
      controller_(&port_) {
    clock_.start();
    speculativeResolve_ = qgetenv("SAILTUBE_SPECULATIVE_RESOLVE") != "0";  // E12 A/B
    player_ = new player::VideoPlayer(this);
    gate_ = new player::AudioResourceGate(this);
    controller_.setCapabilities(capabilities_);
    controller_.setKeepAliveAvailable(keepAliveAvailable);
#ifdef SAILTUBE_STORE
    controller_.setCoverVideoSetting(false);  // cover video: none in STORE
#else
    controller_.setCoverVideoSetting(true);   // cover video: FULL default on until main.cpp says otherwise
#endif
    applySettings();
    if (settings_) connect(settings_.data(), &AppSettings::changed, this, [this]() { applySettings(); sync(); });
    deadline_.setSingleShot(true);
    likelyHold_.setSingleShot(true);
    connect(&likelyHold_, &QTimer::timeout, this, [this]() { dropLikely("expired"); });
    connect(&deadline_, &QTimer::timeout, this, [this]() {
        controller_.setWallClock(now(), QDateTime::currentMSecsSinceEpoch());  // RES-19 across suspend
        controller_.tick(now());
        sync();
    });

    // PLY-23: grants and losses arrive queued, never inside a controller call.
    connect(gate_, &player::AudioResourceGate::granted, this, [this]() { controller_.onAudioGranted(); sync(); },
            Qt::QueuedConnection);
    connect(gate_, &player::AudioResourceGate::lost, this, [this]() { controller_.onAudioLost(); sync(); },
            Qt::QueuedConnection);

    // Player events: state is read when the slot runs; one-shot events are
    // tagged with the load they belong to and dropped when stale.
    using player::VideoPlayer;
    connect(player_, &VideoPlayer::stateChanged, this, [this]() { playerStateChanged(); }, Qt::QueuedConnection);
    connect(player_, &VideoPlayer::playingChanged, this, [this]() {
        controller_.onPlayerPlaying(player_->playing());
        sync();
    }, Qt::QueuedConnection);
    connect(player_, &VideoPlayer::positionChanged, this, [this]() {
        controller_.onPosition(now(), static_cast<std::int64_t>(player_->position()));
        emit positionChanged();
        updateCaptionText();  // SEL-06: the cue follows the position at the poll cadence
        sync();
    }, Qt::QueuedConnection);
    connect(player_, &VideoPlayer::bufferedPositionChanged, this, &PlaybackSession::bufferedPositionChanged,
            Qt::QueuedConnection);
    connect(player_, &VideoPlayer::durationChanged, this, [this]() {
        controller_.onDuration(static_cast<std::int64_t>(player_->duration()));
        emit playerChanged();
    }, Qt::QueuedConnection);
    connect(player_, &VideoPlayer::seekableChanged, this, [this]() {
        controller_.onSeekable(player_->seekable());
        sync();
    }, Qt::QueuedConnection);
    connect(player_, &VideoPlayer::bufferingChanged, this, &PlaybackSession::playerChanged, Qt::QueuedConnection);
    connect(player_, &VideoPlayer::decodeModeChanged, this, &PlaybackSession::streamChanged, Qt::QueuedConnection);
    connect(player_, &VideoPlayer::repaintRequested, this, &PlaybackSession::repaintRequested);
    // videoAspect: the decoded picture's aspect replaces the plan's once the size is known.
    connect(player_, &VideoPlayer::videoSizeChanged, this, [this]() {
        decodedAspect_ = decodedAspect();
        decodedGeneration_ = loadGeneration_;
        updateAspect();
    });
    connect(player_, &VideoPlayer::firstFrame, this, [this]() {
        const quint64 g = loadGeneration_;
        // videoAspect: the first frame's caps carry the pixel aspect.
        const double aspect = decodedAspect();
        if (aspect > 0) {
            decodedAspect_ = aspect;
            decodedGeneration_ = g;
            updateAspect();
        }
        deferred([this, g]() {
            if (g != loadGeneration_) return;
            controller_.onFirstFrame(now());
        });
    });
    connect(player_, &VideoPlayer::endOfStream, this, [this]() {
        const quint64 g = loadGeneration_;
        deferred([this, g]() {
            if (g != loadGeneration_) return;
            // Shorts: a loop item restarts at 0 in the same pipeline; it never "ends".
            const bool loop = controller_.loop();
            controller_.onEnded(now());
            if (loop) timingMark("loop-restart", run());
            else emit ended();
        });
    });
    connect(player_, &VideoPlayer::failed, this, [this](const QString &reason, bool network, int branch, int httpStatus) {
        const quint64 g = loadGeneration_;
        branchFailed(branch, network, reason, httpStatus);  // RES-07 / RES-12: byte-0 refusals are reported
        const std::string r = u(reason);
        deferred([this, g, r, network]() {
            if (g == loadGeneration_) controller_.onPlayerFailed(now(), r, network);
        });
    });
    connect(player_, &VideoPlayer::decodeFallback, this, [this](const QString &reason) {
        const std::string r = u(reason);
        deferred([this, r]() { controller_.onDecodeFallback(r); });
    });
    connect(player_, &VideoPlayer::frozenChanged, this, [this]() {
        if (player_->frozen())
            timingMark("video-decode-stopped", QStringLiteral("mode=%1").arg(QLatin1String(modeName(player_->plan().mode))));
    });
    connect(player_, &VideoPlayer::firstByte, this, [this](const QString &branch, qint64) {
        const player::Branch b = branch == QLatin1String("video") ? player::Branch::Video
                               : branch == QLatin1String("audio") ? player::Branch::Audio : player::Branch::Muxed;
        firstBytes_ |= 1 << branchSlot(b);
        timingMark("first-byte", QStringLiteral("%1 branch=%2").arg(run()).arg(branch));
    });
    connect(player_, &VideoPlayer::timing, this, [this](const QString &marker, qint64 ms) {
        if (marker == QLatin1String("preroll")) timingMark("preroll-done", run());
        else if (marker == QLatin1String("seek-done")) {
            // The stall watchdog's own seek in place is not a user seek: no seek-done for it (the
            // NFR-PERF-03 pairs stay clean; its outcome is stall-recovered or the next stall-recover).
            if (!(controller_.stall().recovering() && controller_.stall().step() == player::StallStep::Seek))
                timingMark("seek-done", QStringLiteral("seek=%1 landed=%2").arg(seekCount_).arg(qint64(player_->position())));
        }
        // NFR-PERF-02 start-up split (engine markers carry their own keys): demux-ready,
        // start-seek, first-decoded per branch; teardown-done reports ms since the stop.
        else if (marker.startsWith(QLatin1String("demux-ready ")) || marker.startsWith(QLatin1String("start-seek "))
                 || marker.startsWith(QLatin1String("first-decoded "))) {
            const int space = marker.indexOf(QLatin1Char(' '));
            timingMark(marker.left(space).toLatin1().constData(),
                       QStringLiteral("%1 %2").arg(run()).arg(marker.mid(space + 1)));
        } else if (marker == QLatin1String("teardown-done")) {
            timingMark("teardown-done", QStringLiteral("%1 ms=%2").arg(run()).arg(ms));
        }
    });
    player_->setPositionConsumer(true);  // the page, the cover and MPRIS show the position
}

PlaybackSession::~PlaybackSession() {
    if (captionSource_) captionSource_->setChangedCallback(std::function<void()>());
    captionSource_ = nullptr;
    ++resolveToken_;
    if (spec_.op && spec_.op != resolveOp_ && resolver_) resolver_->cancel(spec_.op);
    spec_ = Speculation();
    if (likely_.op && likely_.op != resolveOp_ && resolver_) resolver_->cancel(likely_.op);
    closeStreams(&likely_.held);
    likely_ = Likely();
    closeStreams(&adopted_);
    if (resolveOp_ && resolver_) resolver_->cancel(resolveOp_);
    resolveOp_ = 0;
    if (cancelMetadata_) cancelMetadata_();
    cancelMetadata_ = nullptr;
    closeStreams(&streams_);
    closeStreams(&parked_);
}

// Runs f on the next event-loop turn (never inside the emitter or a controller call).
void PlaybackSession::deferred(const std::function<void()> &f) {
    QPointer<PlaybackSession> guard(this);
    QTimer::singleShot(0, this, [guard, f]() {
        if (!guard) return;
        f();
        guard->sync();
    });
}

// A BCP-47 style tag ("", "en", "pt-BR", "zh-Hant-TW"): letters, digits and '-', at most 35.
static bool languageTagLooksValid(const QString &tag) {
    if (tag.size() > 35) return false;
    for (const QChar c : tag)
        if (!(c.isLetterOrNumber() && c.unicode() < 128) && c != QLatin1Char('-')) return false;
    return true;
}

void PlaybackSession::applySettings() {
    if (!settings_) return;
    controller_.setRememberedRate(settings_->playbackSpeed());
    controller_.setKeepDisplayOnSetting(settings_->keepDisplayOn());
    controller_.setRelatedAutoplay(settings_->autoplayRelated());
    controller_.setKeepSessionOnLeave(settings_->keepSessionOnLeave());
    controller_.setResumeAfterInterruption(settings_->resumeAfterInterruption());
    controller_.setSponsorSettings(settings_->sponsorSettings());
    controller_.setStartLowRung(settings_->startLowRung());
    setTimingEnabled(settings_->timingLog());
    if (settings_->audioOnly() != controller_.audioOnly()) controller_.setAudioOnly(now(), settings_->audioOnly());
    if (hwGate_) hwGate_->setHardwareRequested(settings_->hardwareDecoding());  // PLY-20: first enable probes
    // SEL-05 remembered audio language (no settings row, like the speed) and the Stable volume
    // switch (SEL-04). A stored value the controller already has is a no-op.
    const QString dub = settings_->value(QStringLiteral("playback/dubLanguage")).toString();
    if (languageTagLooksValid(dub) && u(dub) != controller_.rememberedDub()) controller_.setRememberedDub(u(dub));
    const bool stable = settings_->value(QStringLiteral("playback/stableVolume")).toBool();
    if (stable != controller_.stableVolume()) controller_.setStableVolume(now(), stable);
    // The "Background playback" switch: off pauses when the app leaves the foreground.
    controller_.setBackgroundPlayback(settings_->value(QStringLiteral("playback/background"), true).toBool());
    // PLY-22: volume boost and loudness normalisation. The audio chain takes them live (no
    // rebuild); every later load starts with them (startPlayer).
    player::AudioSettings audio = audioSettings_;
    audio.boostPercent = settings_->value(QStringLiteral("audio/boost"), player::kBoostMinPercent).toInt();
    audio.normalizationEnabled = settings_->value(QStringLiteral("audio/loudness"), false).toBool();
    if (audio.boostPercent != audioSettings_.boostPercent || audio.normalizationEnabled != audioSettings_.normalizationEnabled) {
        audioSettings_ = audio;
        if (player_) player_->setAudioSettings(audioSettings_, loudnessFor(controller_.videoId()));
    }
}

// The loudness of a video's resolve answer; unknown for another video, a download or an answer
// without it (the normaliser then leaves the level alone).
player::StreamLoudness PlaybackSession::loudnessFor(const std::string &videoId) const {
    return !videoId.empty() && videoId == loudnessVideoId_ ? loudness_ : player::StreamLoudness();
}

void PlaybackSession::setHardwareDecodeGate(player::HardwareDecodeGate *gate) {
    if (hwGate_) disconnect(hwGate_.data(), nullptr, this, nullptr);
    hwGate_ = gate;
    if (!gate) return;
    connect(gate, &player::HardwareDecodeGate::changed, this, [this]() { applyHardwareVerdict(); });
    if (settings_) gate->setHardwareRequested(settings_->hardwareDecoding());
    applyHardwareVerdict();
}

void PlaybackSession::setCapabilities(const media::DecoderCapabilities &capabilities) {
    const bool verified = capabilities_.hardwareDecodeVerified;
    capabilities_ = capabilities;
    capabilities_.hardwareDecodeVerified = verified;
    controller_.setCapabilities(capabilities_);
    emit streamChanged();  // diagnostics
}

// PLY-20 -> SEL-02 and the decode mode of the next selection (hardware only when verified).
void PlaybackSession::applyHardwareVerdict() {
    const bool verified = hwGate_ && hwGate_->verified();
    if (verified != capabilities_.hardwareDecodeVerified) {
        capabilities_.hardwareDecodeVerified = verified;
        controller_.setCapabilities(capabilities_);
    }
    emit streamChanged();  // diagnostics
}

// Selection inputs that apply to the next video: preferences and network class (SEL-02).
void PlaybackSession::prepareSelection() {
    if (!settings_) return;
    controller_.setPreferences(selectionPreferences(&networkClass_));
}

// The preferences and network class prepareSelection() would apply now (SEL-02).
media::PlaybackPreferences PlaybackSession::selectionPreferences(int *networkClass) const {
    if (!settings_) {
        *networkClass = networkClass_;
        return controller_.preferences();
    }
    media::PlaybackPreferences p = settings_->preferences();
    if (controller_.dubRemembered()) p.audioLanguage = controller_.rememberedDub();  // SEL-05, this session
    *networkClass = networkProbe_ ? networkProbe_() : -1;
    p.network = *networkClass == 1 ? media::NetworkClass::Mobile : media::NetworkClass::Unmetered;
    return p;
}

QString PlaybackSession::net() const {
    return networkClass_ == 0 ? QStringLiteral("net=wifi") : networkClass_ == 1 ? QStringLiteral("net=4g")
                                                                                 : QStringLiteral("net=other");
}

// Emits the notify signals of everything the controller changed, and re-arms
// the one deadline timer.
void PlaybackSession::sync() {
    if (syncing_) return;
    syncing_ = true;
    QString pendingLocal;
    for (int guard = 0; guard < 4; ++guard) {
        const unsigned c = controller_.takeChanges();
        if (!c) break;
        if (c & PlaybackController::DetailsChange) {
            emit detailsChanged();
            fetchArtwork();
            if (captionSource_ && captionTrackNotified_ != videoId()) captionsUpdated();  // another video's tracks
        }
        if (c & PlaybackController::StreamChange) emit streamChanged();
        if (c & PlaybackController::StateChange) emit stateChanged();
        if (c & PlaybackController::PlayerChange) { emit playerChanged(); emit positionChanged(); }
        if (c & PlaybackController::ControlsChange) emit controlsChanged();
        if (c & PlaybackController::UpNextChange) emit upNextChanged();
        if (c & PlaybackController::SponsorChange) emit sponsorChanged();
        if (c & PlaybackController::StreamSwitchChange) {  // PLY-26 stream instead: the file is gone
            localCaptions_.clear();
            localSegments_ = false;
            localSidecarsPending_ = false;
            ++switchCount_;
            timingMark("switch-start", QStringLiteral("switch=%1 kind=stream %2").arg(switchCount_).arg(net()));
        }
        if (c & PlaybackController::LowRungSwitchChange) {  // NFR-PERF-02 low-rung A/B
            ++switchCount_;
            timingMark("switch-start", QStringLiteral("switch=%1 kind=lowrung %2").arg(switchCount_).arg(net()));
        }
        if (c & PlaybackController::AudioSwitchChange) {  // SEL-05/07 dub or stable-volume switch
            ++switchCount_;
            timingMark("switch-start", QStringLiteral("switch=%1 kind=dub %2").arg(switchCount_).arg(net()));
        }
        if (c & PlaybackController::SleepTimerChange) {
            sleepSecondsNotified_ = sleepTimerSecondsLeft();
            emit sleepTimerChanged();
        }
        if (c & PlaybackController::SegmentsChange) emit sponsorSegmentsChanged();
        if (c & PlaybackController::StallChange) {  // stall watchdog: its markers, in order
            for (const PlaybackController::StallEvent &e : controller_.takeStallEvents()) {
                switch (e.kind) {
                case PlaybackController::StallEvent::Detected:
                    timingMark("stall-detected", QStringLiteral("%1 reason=%2 position=%3 idle=%4")
                                                     .arg(run(), s(e.reason)).arg(e.positionMs).arg(e.ms));
                    break;
                case PlaybackController::StallEvent::Step:
                    timingMark("stall-recover", QStringLiteral("%1 step=%2").arg(run()).arg(e.step));
                    break;
                case PlaybackController::StallEvent::Recovered:
                    timingMark("stall-recovered", QStringLiteral("%1 ms=%2 step=%3").arg(run()).arg(e.ms).arg(e.step));
                    break;
                case PlaybackController::StallEvent::Failed:
                    timingMark("stall-failed", QStringLiteral("%1 reason=%2 step=%3").arg(run(), s(e.reason)).arg(e.step));
                    break;
                }
            }
        }
        if (c & PlaybackController::SwitchResumedChange)
            timingMark("switch-resumed", QStringLiteral("switch=%1").arg(switchCount_));
        if (c & PlaybackController::ReplacePageChange) emit replacePageRequested(s(controller_.replacePageVideoId()));
        if (c & PlaybackController::LocalOpenChange) pendingLocal = s(controller_.pendingLocalOpen());
    }
    syncing_ = false;
    // PLY-26: a queue / autoplay item with a download plays from the device, through the same
    // path as open() (outside the sync loop: openLocalRecord syncs again).
    if (!pendingLocal.isEmpty()) {
        QVariantMap record;
        if (localCopy(pendingLocal, QVariantMap(), &record)) {
            if (!record.contains(QStringLiteral("videoId"))) record.insert(QStringLiteral("videoId"), pendingLocal);
            hints_.clear();
            openLocalRecord(record, -1, PlaybackController::kLoopUnset);
        } else {
            open(pendingLocal);  // the file vanished since the check: stream it
        }
    }
    const int length = controller_.queueLength(), index = controller_.queueIndex();
    if (length != queueLength_ || index != queueIndex_) {
        queueLength_ = length;
        queueIndex_ = index;
        emit queueChanged();
    }
    if (controller_.loop() != loopNotified_) {
        loopNotified_ = controller_.loop();
        emit loopChanged();
    }
    updateCover();
    updateAspect();
    rearm();
    if (!likelyPendingId_.empty() && !controller_.starting() && !likely_.attached) runPendingLikely();
}

// videoAspect: the decoded picture of this load when known, else the chosen plan's format.
void PlaybackSession::updateAspect() {
    double aspect = 0.0;
    if (controller_.hasVideo() && !controller_.audioOnly())
        aspect = decodedGeneration_ == loadGeneration_ && decodedAspect_ > 0 ? decodedAspect_
                                                                             : controller_.plannedVideoAspect();
    if (std::fabs(aspect - videoAspect_) < 1e-9) return;
    videoAspect_ = aspect;
    emit videoAspectChanged();
}

// The player's current picture aspect, pixel aspect included (0 unknown).
double PlaybackSession::decodedAspect() const {
    player::VideoFrameMailbox *box = player_->frameMailbox();
    QSize size;
    if (const std::shared_ptr<player::HardwareFrameSource> hw = box->hardwareSource()) size = hw->geometry().displaySize();
    else size = box->latest().displaySize();
    if (!size.isValid() || size.isEmpty()) size = QSize(player_->videoWidth(), player_->videoHeight());
    return size.width() > 0 && size.height() > 0 ? static_cast<double>(size.width()) / size.height() : 0.0;
}

// Cover video: the item's flag, FreezePolicy's cover input and the cover-only decode time.
void PlaybackSession::updateCover() {
    const bool keeps = controller_.coverKeepsDecoding();
    if (keeps != coverKeepsDecoding_) {
        coverKeepsDecoding_ = keeps;
        player_->setCoverVisible(keeps);  // lifts / restores the PLY-09 freeze
    }
    // Markers cover-video-on / cover-video-off (one token: the marker grammar's <event>).
    if (coverMeter_.update(controller_.coverDecoding(), now()))
        coverMeter_.on ? timingMark("cover-video-on", run())
                       : timingMark("cover-video-off",
                                    QStringLiteral("%1 reason=%2").arg(run()).arg(QLatin1String(controller_.coverOffReason())));
    const bool active = controller_.coverVideoActive();
    if (active != coverVideoActive_) {
        coverVideoActive_ = active;
        emit coverVideoActiveChanged();
    }
}

void PlaybackSession::rearm() {
    const std::int64_t deadline = controller_.nextDeadlineMs();
    // The 1 s up-next tick keeps the countdown label current (no timer above 1 Hz).
    std::int64_t next = deadline;
    if (controller_.autoplay().countdownActive()) {
        const std::int64_t second = now() + 1000;
        if (next < 0 || second < next) next = second;
    }
    // Sleep timer: the countdown label ticks once a second only while it is bound and on screen;
    // otherwise the controller's single deadline is the only wake-up (NFR-PERF-06).
    if (controller_.sleepTimerActive() && sleepTimerShown()) {
        const int left = sleepTimerSecondsLeft();
        if (left != sleepSecondsNotified_) {
            sleepSecondsNotified_ = left;
            emit sleepTimerChanged();
        }
        const std::int64_t deadline = controller_.sleepDeadlineMs();
        if (deadline >= 0) {  // the next whole-second change (the end-of-video count follows the position)
            const std::int64_t rest = (deadline - now()) % 1000;
            const std::int64_t tickAt = now() + (rest > 0 ? rest : 1000);
            if (next < 0 || tickAt < next) next = tickAt;
        }
    }
    if (next < 0) { deadline_.stop(); return; }
    std::int64_t wait = next - now();
    // A far deadline (RES-19 scheduled start) is re-checked every minute against the wall clock.
    if (wait > 60000) wait = 60000;
    deadline_.start(static_cast<int>(wait < 0 ? 0 : wait));
    if (controller_.autoplay().countdownActive()) emit upNextChanged();
}

void PlaybackSession::playerStateChanged() {
    using player::VideoPlayer;
    const VideoPlayer::State st = player_->state();
    if (!startedReported_ && (st == VideoPlayer::Paused || st == VideoPlayer::Playing)) {
        startedReported_ = true;
        controller_.onPlayerStarted(now());
        requestMetadata();  // after the first preroll, never on the start path
    }
    emit playerChanged();
    sync();
}

// ---- Port ---------------------------------------------------------------------------------

// The resolve in flight is cancelled; a speculation it had joined goes with it.
void PlaybackSession::cancelResolveOp() {
    if (spec_.attached && spec_.op == resolveOp_) spec_ = Speculation();
    if (likely_.attached && likely_.op == resolveOp_) likely_ = Likely();
    if (resolveOp_ && resolver_) resolver_->cancel(resolveOp_);
    resolveOp_ = 0;
}

void PlaybackSession::Port::resolve(const ResolveRequest &r) {
    PlaybackSession *self = s;
    self->cancelResolveOp();
    const quint64 token = ++self->resolveToken_;
    self->lastStartMs_ = self->now();
    timingMark("resolve-start", self->run());
    const std::string videoId = r.videoId;
    const unsigned generation = r.generation;
    if (!self->resolver_) {
        self->deferred([self, token, videoId, generation]() {
            if (token != self->resolveToken_) return;
            self->onStreams(videoId, generation, playback::StreamSet(),
                            playback::streamError(playback::StreamErrorClass::RuntimeUnavailable, false));
        });
        return;
    }
    const playback::StreamRequest request = streamRequestFor(r, self->networkClass_, self->poProviderReady());
    // Shorts: a resolve of another video (an open, the queue, autoplay) ends the preload of a
    // different id; a re-resolve of the same video (recovery, switch) does not.
    if (videoId != self->lastResolveVideoId_ && self->likely_.token && self->likely_.videoId != videoId)
        self->dropLikely("open");
    self->lastResolveVideoId_ = videoId;
    // A whole-set failure, a retry or a recovery: drop the video's cached answers first (RES-10).
    if (r.fresh) {
        if (self->likely_.token && self->likely_.videoId == videoId) self->dropLikely("fresh");
        // RES-07: a resolver with strike rules is told why (a stall, a lost connection and a
        // start that ran out of time are not strikes); any other just drops the answers.
        if (player::SourceFaultSink *sink = dynamic_cast<player::SourceFaultSink *>(self->resolver_))
            sink->reportFault(r.videoId, r.fault);
        else
            self->resolver_->invalidate(request.videoId);
    }
    // E12: the press that led to this open already asked the same question.
    else if (self->takeSpeculation(request, videoId, generation, token)) return;
    // Shorts: likelyNext preloaded this video.
    else if (self->takeLikely(request, videoId, generation, token)) return;
    QPointer<PlaybackSession> guard(self);
    // The completion is asynchronous and on this thread (StreamSource.h contract).
    self->resolveOp_ = self->resolver_->resolve(request,
        [guard, token, videoId, generation](const playback::StreamSet &set, const playback::StreamError &error) {
        if (!guard || token != guard->resolveToken_) return;
        guard->resolveOp_ = 0;
        guard->onStreams(videoId, generation, set, error);
        guard->sync();
    });
}

void PlaybackSession::Port::cancelResolve() {
    ++s->resolveToken_;
    s->cancelResolveOp();
}

void PlaybackSession::Port::prefetch(const ResolveRequest &r) {
    if (s->resolver_) s->resolver_->prefetch(streamRequestFor(r, s->networkClass_, s->poProviderReady()));
}

void PlaybackSession::onStreams(const std::string &videoId, unsigned generation, const playback::StreamSet &set,
                                const playback::StreamError &error, bool fromSpeculation) {
    ResolveAnswer a;
    a.error = resolveErrorKind(error);
    a.errorCode = static_cast<int>(error.kind);
    // RES-19: an Upcoming class or an upcoming set, with the start when the source knows it.
    a.upcoming = error.kind == playback::StreamErrorClass::Upcoming ||
                 (error.ok() && set.liveState == playback::LiveState::Upcoming);
    a.scheduledStartUnixMs = set.scheduledStartUnixMs > 0 ? set.scheduledStartUnixMs : 0;
    // RES-09: rate limiting: the one re-resolve waits (PlaybackController::kRetryLaterMs).
    a.retryLater = a.error == ResolveAnswer::Retryable && error.kind == playback::StreamErrorClass::TryLater;
    // Never another video's streams.
    if (a.error == ResolveAnswer::None && !set.videoId.isEmpty() && set.videoId != s(videoId)) {
        a.error = ResolveAnswer::Retryable;
        a.errorCode = static_cast<int>(playback::StreamErrorClass::InvalidResponse);
    }
    if (a.error == ResolveAnswer::None) {
        if (videoId != formatsVideoId_) {
            formats_.clear();
            formatsVideoId_ = videoId;
        }
        for (const QList<playback::StreamFormat> *list : {&set.video, &set.audio, &set.muxed})
            for (const playback::StreamFormat &f : *list) formats_[f.url.toEncoded().toStdString()] = f;
        if (set.hlsManifest.isValid() && !set.hlsManifest.isEmpty()) {
            playback::StreamFormat manifest;
            manifest.url = set.hlsManifest;
            manifest.isHls = true;
            formats_[set.hlsManifest.toEncoded().toStdString()] = manifest;
        }
        path_ = set.path;
        pathFromSpeculation_ = fromSpeculation;
        // PLY-22: the stream's loudness for the normaliser (kept when a later answer has none).
        if (set.hasLoudness || loudnessVideoId_ != videoId) {
            loudnessVideoId_ = videoId;
            loudness_.known = set.hasLoudness;
            loudness_.loudnessDb = set.loudnessDb;
        }
        // Scrub preview: the answer's storyboard (the yt-dlp path carries one). A later answer for
        // the same video without one (a fast-path re-resolve) keeps what is known.
        if (!set.storyboard.isEmpty() || storyboardVideoId_ != videoId) {
            storyboard_ = storyboardFor(set.storyboard);
            storyboardVideoId_ = videoId;
        }
        a.details = detailsFor(set);
        a.details.summary.videoId = videoId;
        a.expiresAtUnixMs = set.expiresAtUnixMs;
        // E12: an answer asked for on touch-down is a prefetched one (perf_summary ttff.prefetch).
        const QLatin1String path(fromSpeculation ? "prefetch" : player::timingPathLabel(set.path));
        timingMark("resolve-done", QStringLiteral("%1 path=%2").arg(run()).arg(path));
        // RES-06: the component's probe is the resolve's last phase (before the tap when speculative).
        if (set.phases.probeMs >= 0 && !fromSpeculation) timingMark("probe-done", run());
    }
    controller_.onResolved(now(), QDateTime::currentMSecsSinceEpoch(), videoId, generation, a);
}

// The set's formats and live state; the summary from the open hints (ARCHITECTURE.md §8).
catalog::VideoDetails PlaybackSession::detailsFor(const playback::StreamSet &set) const {
    catalog::VideoDetails d;
    const media::SelectionInput in = selectionInputFor(set);
    d.formats = in.formats;
    d.manifestUrl = in.manifestUrl;
    d.summary.liveStatus = in.liveStatus;
    d.summary.title = u(hint("title").toString());
    d.summary.channel = u(hint("channel").toString());
    d.summary.channelId = u(hint("channelId").toString());
    d.summary.thumbnailUrl = u(hint("thumbnail").toString());
    const double duration = hint("durationSeconds", -1).toDouble();
    d.summary.durationSeconds = duration > 0 ? static_cast<std::int64_t>(duration) : -1;
    const double views = hint("viewCount", -1).toDouble();
    d.summary.viewCount = views >= 0 ? static_cast<std::int64_t>(views) : -1;
    return d;
}

void PlaybackSession::Port::load(const PlaybackLoad &l) {
    s->lastStartMs_ = s->now();
    ++s->loadGeneration_;
    s->startedReported_ = false;
    s->firstBytes_ = 0;
    s->openBranches(l);
}

// RES-16..19: every branch of the candidate through the fetcher, all opens issued before any
// answer; the player loads once all are open. HLS goes to the player as it is (RES-19); local
// files need no fetcher (PLY-26); a URL that is not in a resolved set never reaches the player
// (RES-22).
void PlaybackSession::openBranches(const PlaybackLoad &l) {
    parkStreams();
    pending_ = PendingLoad();
    pending_.active = true;
    pending_.load = l;
    const quint64 g = loadGeneration_;
    const player::SourceSpec *specs[3] = {&l.video, &l.audio, &l.muxed};
    bool unknown = false;
    for (int slot = 0; slot < 3; ++slot) {
        const player::SourceSpec &spec = *specs[slot];
        if (!spec.present() || spec.uri.compare(0, 7, "file://") == 0) continue;
        const std::map<std::string, playback::StreamFormat>::const_iterator known = formats_.find(spec.uri);
        if (known == formats_.end()) { unknown = true; break; }
        Stream st;
        st.remote = spec.uri;
        st.format = known->second;
        bool reused = false;  // SEL-07: the previous load's stream of the same format is kept
        // Stall recovery (freshStreams): never; the job that stopped delivering is closed below
        // and the format opens through a new one.
        for (std::vector<Stream>::iterator it = parked_.begin(); !l.freshStreams && it != parked_.end(); ++it)
            if (it->remote == spec.uri && it->ready) { st = *it; parked_.erase(it); reused = true; break; }
        // Shorts: likelyNext's held job of this format (open or still opening) is adopted.
        for (std::vector<Stream>::iterator it = adopted_.begin(); !reused && it != adopted_.end(); ++it)
            if (it->remote == spec.uri && (it->ready || it->handle)) { st = *it; adopted_.erase(it); reused = true; break; }
        if (!reused && (st.format.isHls || !fetcher_)) {
            st.local = st.format.url;
            st.ready = true;
        }
        streams_.push_back(st);
        pending_.branches[slot] = static_cast<int>(streams_.size()) - 1;
        if (!st.ready) ++pending_.waiting;
    }
    closeParked();  // the previous load's other stream jobs
    dropAdopted();  // a preload's jobs this plan does not use
    if (unknown) {
        pending_.active = false;
        closeStreams(&streams_);
        deferred([this, g]() {
            if (g == loadGeneration_) controller_.onPlayerFailed(now(), "stream not in the resolved set", false);
        });
        return;
    }
    QPointer<PlaybackSession> guard(this);
    for (std::size_t i = 0; i < streams_.size(); ++i) {
        if (streams_[i].ready || streams_[i].handle) continue;  // adopted while opening: its answer is routed here
        const int index = static_cast<int>(i);
        streams_[i].handle = fetcher_->open(streams_[i].format, [guard, g, index](const QUrl &local, const playback::StreamError &e) {
            if (guard) guard->branchOpened(g, index, local, e);
        });
    }
    if (pending_.waiting == 0) startPlayer(pending_.load);
}

void PlaybackSession::branchOpened(quint64 g, int index, const QUrl &local, const playback::StreamError &error) {
    if (g != loadGeneration_ || !pending_.active || index < 0 || index >= static_cast<int>(streams_.size())) return;
    Stream &st = streams_[static_cast<std::size_t>(index)];
    if (!error.ok() || local.isEmpty()) {
        // Refused at open (the source's probe already counted it, RES-06/07) or not servable:
        // this candidate failed, the controller tries the next one.
        pending_.active = false;
        closeStreams(&streams_);
        const bool network = error.kind == playback::StreamErrorClass::Network || error.kind == playback::StreamErrorClass::Timeout;
        controller_.onPlayerFailed(now(), "stream open failed: " + u(error.label), network);
        sync();
        return;
    }
    st.local = local;
    st.ready = true;
    if (--pending_.waiting > 0) return;
    startPlayer(pending_.load);
    sync();
}

void PlaybackSession::startPlayer(const PlaybackLoad &l) {
    pending_.active = false;
    player::PlayerSources src;
    src.mode = l.mode;
    src.video = l.video;
    src.audio = l.audio;
    src.muxed = l.muxed;
    player::SourceSpec *specs[3] = {&src.video, &src.audio, &src.muxed};
    for (int slot = 0; slot < 3; ++slot) {
        const int index = pending_.branches[slot];
        if (index < 0 || index >= static_cast<int>(streams_.size())) continue;
        const Stream &st = streams_[static_cast<std::size_t>(index)];
        specs[slot]->uri = st.local.toEncoded().toStdString();
        if (loopback(st.local)) {  // the fetcher sends the format's headers itself
            specs[slot]->userAgent.clear();
            specs[slot]->extraHeaders.clear();
        }
    }
    src.videoId = l.videoId;
    src.audioSettings = audioSettings_;          // PLY-22: boost, loudness normalisation
    src.loudness = loudnessFor(l.videoId);
    src.startPositionMs = l.startPositionMs;
    src.rate = l.rate;
    src.decode = l.decode;
    src.metadataDurationMs = l.metadataDurationMs;
    src.liveHls = l.liveHls;
    src.autoplay = false;  // PLY-23: PLAYING only after the audio resource is granted
    // SEL-07 / PLY-26: a quality, dub or stream-instead switch of the same video keeps the last
    // picture until the new build's first frame (no black frame; the cover stays active); so does
    // a stall-recovery reload. A switch to listen-only has no video and clears it.
    src.keepFrame = controller_.keepsPicture() && l.mode != player::PlayMode::AudioOnly &&
                    !l.videoId.empty() && l.videoId == lastLoadedVideoId_;
    lastLoadedVideoId_ = l.videoId;
    src.mediaRole = player::kMediaRole;  // PLT-12
    src.audioSink = audioSink_;
    src.videoSink = videoSink_;
    player_->load(src);
    if (pending_.play) player_->play();
    pending_.play = false;
}

void PlaybackSession::parkStreams() {
    for (std::size_t i = 0; i < streams_.size(); ++i) parked_.push_back(streams_[i]);
    streams_.clear();
}

void PlaybackSession::closeParked() { closeStreams(&parked_); }

void PlaybackSession::closeStreams(std::vector<Stream> *streams) {
    for (std::size_t i = 0; i < streams->size(); ++i)
        if ((*streams)[i].handle && fetcher_) fetcher_->close((*streams)[i].handle);
    streams->clear();
}

// RES-07 / RES-12: a branch refused at byte 0 is reported to the resolver (strike + cache drop)
// before the controller moves on; its stream is never reused.
void PlaybackSession::branchFailed(int branch, bool network, const QString &reason, int httpStatus) {
    if (branch < 0 || branch > 2) return;
    int slot = branchSlot(static_cast<player::Branch>(branch));
    if (pending_.branches[slot] < 0) slot = slot == 1 ? 2 : slot == 2 ? 1 : slot;  // audio-only plays muxed and back
    const int index = pending_.branches[slot];
    if (index < 0 || index >= static_cast<int>(streams_.size())) return;
    const bool sawFirstByte = (firstBytes_ & (1 << branchSlot(static_cast<player::Branch>(branch)))) != 0;
    if (!refusalAtByteZero(reason, network, sawFirstByte, httpStatus)) return;
    Stream &st = streams_[static_cast<std::size_t>(index)];
    if (st.format.isHls) return;  // RES-19: a manifest is not a refused media URL
    if (resolver_) resolver_->reportRefused(s(controller_.videoId()), st.format);
    ++refusedReports_;
    if (st.handle && fetcher_) fetcher_->close(st.handle);
    st.handle = 0;
    st.ready = false;
}

// Display metadata for a video opened without a row, after its first preroll (RES-02: on the
// metadata source's own lane).
void PlaybackSession::requestMetadata() {
    const QString id = videoId();
    if (!metadata_ || controller_.local() || id.isEmpty() || id == metadataVideoId_) return;
    if (!title().isEmpty() && !channel().isEmpty()) return;  // the row said what this is
    if (cancelMetadata_) cancelMetadata_();
    metadataVideoId_ = id;
    const quint64 token = ++metadataToken_;
    QPointer<PlaybackSession> guard(this);
    const std::string vid = u(id);
    std::function<void()> cancel = metadata_(id, [guard, token, vid](int code, const catalog::VideoDetails &d) {
        if (!guard || token != guard->metadataToken_) return;
        PlaybackSession *self = guard.data();
        self->deferred([self, token, vid, code, d]() {
            if (token != self->metadataToken_) return;
            self->cancelMetadata_ = nullptr;
            if (code == 0) self->controller_.setMetadata(vid, d);
        });
    });
    if (token == metadataToken_) cancelMetadata_ = cancel;
}

void PlaybackSession::Port::play() {
    if (s->pending_.active) s->pending_.play = true;
    else s->player_->play();
}

void PlaybackSession::Port::pause() {
    if (s->pending_.active) s->pending_.play = false;
    else s->player_->pause();
}

void PlaybackSession::Port::stopPlayer() {
    PlaybackSession *self = s;
    ++self->loadGeneration_;
    self->startedReported_ = false;
    self->pending_.active = false;
    self->parkStreams();  // a load that follows keeps what it can (SEL-07); the rest closes next turn
    // SEL-07 / PLY-26: during a same-video switch (or a stall-recovery reload) the picture stays
    // until the new first frame.
    self->player_->stop(self->controller_.keepsPicture());
    QPointer<PlaybackSession> guard(self);
    QTimer::singleShot(0, self, [guard]() { if (guard) guard->closeParked(); });
}

void PlaybackSession::Port::seek(std::int64_t ms) {
    if (s->pending_.active) s->pending_.load.startPositionMs = ms;
    else s->player_->seek(static_cast<double>(ms));
}

void PlaybackSession::Port::postSeek(std::int64_t ms) {
    // SB-02 / PLT-05: never seek from inside the position handler.
    PlaybackSession *self = s;
    const quint64 g = self->loadGeneration_;
    QTimer::singleShot(0, self, [self, g, ms]() {
        if (g != self->loadGeneration_) return;
        if (self->pending_.active) self->pending_.load.startPositionMs = ms;
        else self->player_->seek(static_cast<double>(ms));
    });
}

void PlaybackSession::Port::setRate(double rate) {
    if (s->pending_.active) s->pending_.load.rate = rate;
    else s->player_->setRate(rate);
}
void PlaybackSession::Port::acquireAudio() { s->gate_->request(); }
void PlaybackSession::Port::releaseAudio() { s->gate_->release(); }

void PlaybackSession::Port::savePosition(const std::string &videoId, std::int64_t positionMs) {
    emit s->positionSaveRequested(s_qs(videoId), static_cast<double>(positionMs));
}

void PlaybackSession::Port::rememberRate(double rate) {
    if (s->settings_) s->settings_->setPlaybackSpeed(rate);
}

// Stall watchdog: the pipeline's own answer, asked when the controller's deadline is due (no
// timer here). False when no load exists or the player already reported its failure.
bool PlaybackSession::Port::progress(PlayerProgress *out) {
    PlaybackSession *self = s;
    if (self->pending_.active) {  // the load's branches are still opening: nothing plays yet
        out->positionMs = self->pending_.load.startPositionMs;
        return true;
    }
    const player::VideoPlayer::State state = self->player_->state();
    if (state == player::VideoPlayer::Idle || state == player::VideoPlayer::Failed) return false;
    qint64 position = 0, buffered = 0;
    if (!self->player_->queryProgress(&position, &buffered)) return false;
    out->positionMs = position;
    out->bufferedMs = buffered;
    out->buffering = self->player_->buffering();
    out->bufferingPercent = self->player_->bufferingPercent();
    out->seeking = self->player_->seeking();
    out->ended = self->player_->ended() || state == player::VideoPlayer::Ended;
    out->videoFrozen = self->player_->frozen();
    return true;
}

// ---- Read-out ---------------------------------------------------------------------------

QObject *PlaybackSession::mediaObject() const { return player_; }
QString PlaybackSession::videoId() const { return s(controller_.videoId()); }

// Hints (ARCHITECTURE.md §8) describe the video they were given with only.
QVariant PlaybackSession::hint(const char *key, const QVariant &fallback) const {
    if (hintsVideoId_ != videoId()) return fallback;
    return hints_.value(QLatin1String(key), fallback);
}

QString PlaybackSession::title() const {
    const catalog::VideoDetails &d = controller_.details();
    return detailsLoaded() && !d.summary.title.empty() ? s(d.summary.title) : hint("title").toString();
}
QString PlaybackSession::channel() const {
    const catalog::VideoDetails &d = controller_.details();
    return detailsLoaded() && !d.summary.channel.empty() ? s(d.summary.channel) : hint("channel").toString();
}
QString PlaybackSession::channelId() const {
    const catalog::VideoDetails &d = controller_.details();
    return detailsLoaded() && !d.summary.channelId.empty() ? s(d.summary.channelId) : hint("channelId").toString();
}
QString PlaybackSession::description() const { return s(controller_.details().description); }
QString PlaybackSession::thumbnail() const {
    const QString h = hint("thumbnail").toString();
    if (!h.isEmpty()) return h;
    if (detailsLoaded() && !controller_.details().summary.thumbnailUrl.empty()) return s(controller_.details().summary.thumbnailUrl);
    // A video opened without a row (link, queue, autoplay): its public still.
    const std::string id = controller_.videoId();
    if (!controller_.local() && catalog::validVideoId(id))
        return QStringLiteral("https://i.ytimg.com/vi/%1/hqdefault.jpg").arg(s(id));
    return QString();
}
double PlaybackSession::viewCount() const {
    const double known = detailsLoaded() && !controller_.local() ? static_cast<double>(controller_.details().summary.viewCount) : -1.0;
    return known >= 0 ? known : hint("viewCount", -1).toDouble();
}
double PlaybackSession::likeCount() const {
    return controller_.local() ? -1.0 : static_cast<double>(controller_.details().likeCount);
}
QString PlaybackSession::uploadDate() const {
    const QDate date = QDate::fromString(s(controller_.details().uploadDate), QStringLiteral("yyyyMMdd"));
    return date.isValid() ? QLocale().toString(date, QLocale::ShortFormat) : QString();
}
bool PlaybackSession::live() const { return controller_.live() || hint("live").toBool(); }

QVariantList PlaybackSession::chapters() const {
    QVariantList list;
    for (const catalog::Chapter &c : controller_.details().chapters) {
        QVariantMap m;
        m.insert(QStringLiteral("title"), s(c.title));
        m.insert(QStringLiteral("start"), c.startSeconds * 1000.0);
        m.insert(QStringLiteral("end"), c.endSeconds * 1000.0);
        list.append(m);
    }
    return list;
}

QVariantList PlaybackSession::qualities() const {
    QVariantList list;
    const std::vector<media::QualityOption> &options = controller_.qualityOptions();
    for (std::size_t i = 0; i < options.size(); ++i) {
        const media::QualityOption &q = options[i];
        QVariantMap m;
        m.insert(QStringLiteral("height"), q.height);
        m.insert(QStringLiteral("highFrameRate"), q.highFrameRate);
        m.insert(QStringLiteral("premium"), q.premium);
        m.insert(QStringLiteral("label"), s(q.label));
        m.insert(QStringLiteral("codec"), QString::fromLatin1(media::videoCodecName(q.codec)));
        m.insert(QStringLiteral("formatId"), s(q.formatId));
        m.insert(QStringLiteral("playing"), static_cast<int>(i) == controller_.playingQualityIndex());
        list.append(m);
    }
    return list;
}

QString PlaybackSession::streamLabel() const {
    if (controller_.local()) {
        //: Stream label of a downloaded video played from the device
        return tr("Downloaded file");
    }
    const media::StreamCandidate *c = controller_.candidate();
    return c ? s(c->label) : QString();
}

QVariantMap PlaybackSession::diagnostics() const {
    QVariantMap m;
    const media::StreamCandidate *c = controller_.candidate();
    m.insert(QStringLiteral("decodeMode"), player_->decodeMode() == int(player::DecodeMode::Hardware)
                                               ? QStringLiteral("hardware") : QStringLiteral("software"));
    m.insert(QStringLiteral("hardwareDecodeFallback"), controller_.hardwareDecodeFallback());
    m.insert(QStringLiteral("decodeFallbackReason"), s(controller_.decodeFallbackReason()));
    // PLY-20 / DIA-02: the hardware-decode probe result and whether selection may use hardware.
    m.insert(QStringLiteral("hardwareDecodeProbe"), hwGate_ ? hwGate_->resultLine() : QStringLiteral("not run"));
    m.insert(QStringLiteral("hardwareDecodeVerified"), capabilities_.hardwareDecodeVerified);
    m.insert(QStringLiteral("playMode"), QLatin1String(modeName(player_->plan().mode)));
    m.insert(QStringLiteral("candidate"), c ? s(c->label) : QString());
    m.insert(QStringLiteral("attempt"), controller_.attempt());
    m.insert(QStringLiteral("chainLength"), static_cast<int>(controller_.chain().size()));
    m.insert(QStringLiteral("fullProfile"), controller_.fullProfile());
    m.insert(QStringLiteral("local"), controller_.local());
    // DL-07: informational; captions or segments of this download are wanted but not saved yet.
    m.insert(QStringLiteral("sidecarsPending"), controller_.local() && localSidecarsPending_);
    m.insert(QStringLiteral("localSegments"), controller_.local() && localSegments_);
    m.insert(QStringLiteral("lowRung"), controller_.lowRungActive());  // NFR-PERF-02 A/B
    m.insert(QStringLiteral("recoveryTries"), controller_.recovery().triesUsed());
    m.insert(QStringLiteral("failureReason"), s(controller_.failureReason()));
    m.insert(QStringLiteral("owner"), controller_.arbiter().owner());
    m.insert(QStringLiteral("parked"), controller_.arbiter().parked());
    m.insert(QStringLiteral("audioResource"), static_cast<int>(controller_.audioResource().state()));
    // MPR-01: zero after Stop (the resolve in flight plus every open or opening stream).
    m.insert(QStringLiteral("streamJobs"), (resolveOp_ ? 1 : 0) + static_cast<int>(streams_.size() + parked_.size()) +
                                               (likely_.op && !likely_.attached ? 1 : 0) +
                                               static_cast<int>(likely_.held.size() + adopted_.size()));
    m.insert(QStringLiteral("resolvePath"), QLatin1String(pathFromSpeculation_ ? "prefetch" : player::timingPathLabel(path_)));
    m.insert(QStringLiteral("refusedReports"), refusedReports_);
    // Stall watchdog: episodes detected, episodes that ended with playback advancing, and the last
    // recovery step run (0 none, 1 seek in place, 2 streams reopened, 3 fresh resolve).
    m.insert(QStringLiteral("stalls"), controller_.stall().stalls());
    m.insert(QStringLiteral("stallRecoveries"), controller_.stall().recoveries());
    m.insert(QStringLiteral("lastStallStep"), controller_.stall().lastStep());
    // E12 / RES-11 / RES-21: speculative resolves used vs wasted, row prefetches, keep-alives.
    m.insert(QStringLiteral("speculativeStarted"), specStats_.started);
    m.insert(QStringLiteral("speculativeUsed"), specStats_.used);
    m.insert(QStringLiteral("speculativeWasted"), specStats_.wasted);
    m.insert(QStringLiteral("speculationInFlight"), spec_.op != 0 && !spec_.attached);
    m.insert(QStringLiteral("rowPrefetches"), specStats_.rowPrefetches);
    m.insert(QStringLiteral("keepAliveWarmUps"), specStats_.warmUps);
    // Shorts likelyNext: preloads started, used by the following open, dropped unused; the id held.
    m.insert(QStringLiteral("likelyNextStarted"), likelyStarted_);
    m.insert(QStringLiteral("likelyNextUsed"), likelyUsed_);
    m.insert(QStringLiteral("likelyNextWasted"), likelyWasted_);
    m.insert(QStringLiteral("likelyNextVideo"), s(likely_.attached ? std::string() : likely_.videoId));
    m.insert(QStringLiteral("likelyNextHeldJobs"), static_cast<int>(likely_.held.size()));
    // Shorts pager: shortsAhead calls, second-in-line resolves into the cache, holds in force.
    m.insert(QStringLiteral("shortsAheadCalls"), shortsAheadCalls_);
    m.insert(QStringLiteral("shortsAheadCached"), shortsAheadCached_);
    m.insert(QStringLiteral("shortHolds"), controller_.shortHolds());
    m.insert(QStringLiteral("likelyHoldRemainingMs"), likelyHold_.isActive() ? likelyHold_.remainingTime() : -1);
    m.insert(QStringLiteral("shortActive"), shortActive());
    const player::PlayerDiagnostics d = player_->diagnostics();
    m.insert(QStringLiteral("framesDropped"), d.engine.framesDropped);
    m.insert(QStringLiteral("decodeFallbacks"), d.decodeFallbacks);
    // Cover video: frames the cover drew, and seconds decoding ran for the cover only.
    m.insert(QStringLiteral("coverFramesDrawn"), d.coverFramesDrawn);
    m.insert(QStringLiteral("coverDecodeSeconds"), static_cast<double>(coverMeter_.elapsedMs(now())) / 1000.0);
    return m;
}

QString PlaybackSession::statusText() const {
    switch (controller_.status()) {
    case PlaybackController::Resolving: return tr("Loading video…");
    case PlaybackController::ResolvingFull:
        if (static_cast<playback::StreamErrorClass>(controller_.retryCode()) == playback::StreamErrorClass::DeliveryChanged) {
            // RES-08: the first answer had no usable stream addresses; the whole resolver runs again.
            //: Status line while the video is retried with the full resolver
            return tr("YouTube changed how this video is delivered. Retrying with the full resolver…");
        }
        return tr("Resolving video again…");
    // RES-09: YouTube rate limited the resolve; one more try follows after a short pause.
    //: Status line while waiting to retry after YouTube limited requests
    case PlaybackController::RetryingLater: return tr("YouTube is limiting requests. Trying again shortly…");
    case PlaybackController::Starting: return tr("Starting playback…");
    //: Status while playback recovers automatically after an error (PLY-18)
    case PlaybackController::Reconnecting: return tr("Reconnecting…");
    //: Status of a playback error that is retried when the app is shown again
    case PlaybackController::WaitingForForeground: return tr("Reconnects when SailTube is shown again");
    case PlaybackController::NoStatus: break;
    }
    return QString();
}

QString PlaybackSession::errorText() const {
    if (controller_.streamNoticeActive()) {
        //: Shown for a few seconds when switching a downloaded video to its stream failed; the file plays on
        return tr("Streaming is not available right now.");
    }
    if (controller_.state() != PlaybackController::Failed) return QString();
    switch (controller_.failure()) {
    case PlaybackController::ResolveFailed:
        return streamErrorText(static_cast<playback::StreamErrorClass>(controller_.failureCode()));
    case PlaybackController::NoPlayableStream: return sailfish::errorText(resolver::ErrorCode::NoPlayableStream);
    case PlaybackController::UpcomingStream: {
        const QString text = sailfish::errorText(resolver::ErrorCode::Upcoming);
        const qint64 start = controller_.scheduledStartUnixMs();
        if (start <= 0) return text;
        //: RES-19: appended to "not started yet"; %1 is the scheduled start (date and time)
        return text + QLatin1Char(' ') +
               tr("Scheduled to start %1.").arg(QLocale().toString(QDateTime::fromMSecsSinceEpoch(start), QLocale::ShortFormat));
    }
    case PlaybackController::NetworkFailed: return sailfish::errorText(resolver::ErrorCode::Network);
    case PlaybackController::PlaybackFailed:
        if (controller_.stalledFailure()) {
            //: Shown with a Retry button when the stream stopped delivering and the automatic recovery steps did not bring it back
            return tr("Playback stopped responding.");
        }
        //: Shown with a Retry button after automatic recovery gave up (PLY-18)
        return tr("Playback stopped after repeated errors.");
    case PlaybackController::NoFailure: break;
    }
    return QString();
}

QString PlaybackSession::errorSettingKey() const {
    // RES-09: the resolver's "no playable stream" class (the controller only keeps the opaque code).
    const bool noPlayable = controller_.failure() == PlaybackController::ResolveFailed &&
        static_cast<playback::StreamErrorClass>(controller_.failureCode()) == playback::StreamErrorClass::NoPlayableStream;
    return s(sailfish::errorSettingKey(controller_.errorSettingFacts(noPlayable)));
}

QVariantList PlaybackSession::localCaptions() const {
    return controller_.local() ? localCaptions_ : QVariantList();
}

bool PlaybackSession::recovering() const {
    const player::RecoveryState r = controller_.recovery().state();
    return r == player::RecoveryState::Waiting || r == player::RecoveryState::Recovering ||
           r == player::RecoveryState::Deferred || stallRecovering();
}

// A stall episode is open: the watchdog works on a stream that stopped delivering.
bool PlaybackSession::stallRecovering() const {
    const int st = controller_.state();
    return controller_.stall().recovering() && st != PlaybackController::Idle && st != PlaybackController::Failed;
}

bool PlaybackSession::playing() const { return player_->playing(); }

bool PlaybackSession::buffering() const {
    const int st = controller_.state();
    // Also while the stall watchdog recovers a stream the player still calls "playing".
    return st == PlaybackController::Preparing ||
           (st == PlaybackController::Ready && (player_->buffering() || stallRecovering()));
}

double PlaybackSession::position() const {
    if (player_->state() == player::VideoPlayer::Idle) return static_cast<double>(controller_.positionMs());
    return player_->position();
}

double PlaybackSession::duration() const {
    const double d = player_->duration();
    if (d > 0) return d;
    const catalog::VideoDetails &details = controller_.details();
    const double fromDetails = detailsLoaded() ? static_cast<double>(details.summary.durationSeconds) : -1.0;
    const double known = fromDetails > 0 ? fromDetails : hint("durationSeconds", -1).toDouble();
    return known > 0 ? known * 1000.0 : 0.0;
}

bool PlaybackSession::seekable() const { return player_->seekable(); }

QString PlaybackSession::artUrl() const {
    if (artVideoId_ != videoId() || artPath_.isEmpty() || !QFileInfo(artPath_).isFile()) return QString();
    return s(player::localArtUrl(u(artPath_)));  // MPR-01: file:// only
}

QString PlaybackSession::upNextVideoId() const { return s(controller_.upNextVideoId()); }

int PlaybackSession::upNextSecondsLeft() const {
    const std::int64_t ms = controller_.upNextRemainingMs(now());
    return static_cast<int>((ms + 999) / 1000);
}

QString PlaybackSession::sponsorNotice() const {
    if (!controller_.sponsorNoticeVisible()) return QString();
    return QString::fromLatin1(player::sponsorCategoryInfo(controller_.sponsorNoticeCategory()).apiName);
}

QString PlaybackSession::sponsorButton() const {
    if (!controller_.sponsorButtonVisible()) return QString();
    return QString::fromLatin1(player::sponsorCategoryInfo(controller_.sponsorButtonCategory()).apiName);
}

double PlaybackSession::highlightPosition() const {
    return controller_.highlightVisible() ? static_cast<double>(controller_.highlightMs()) : -1.0;
}

void PlaybackSession::setSinkOverrides(const player::SinkOverride &audio, const player::SinkOverride &video) {
    audioSink_ = audio;
    videoSink_ = video;
}

// ---- Artwork (MPR-01 local artUrl) ---------------------------------------------------------

void PlaybackSession::setArtworkCache(QNetworkAccessManager *network, const QString &directory) {
    network_ = network;
    artworkDir_ = directory;
}

void PlaybackSession::fetchArtwork() {
    const QString id = videoId();
    if (id == artVideoId_ && !artPath_.isEmpty()) return;
    const QString local = hint("thumbnailPath").toString();
    if (!local.isEmpty() && QFileInfo(local).isFile()) {
        artVideoId_ = id;
        artPath_ = local;
        emit artChanged();
        return;
    }
    if (!network_ || artworkDir_.isEmpty() || !validArtworkId(id)) return;
    const QUrl url(thumbnail());
    if (url.scheme() != QLatin1String("https")) return;
    const QString path = artworkDir_ + QLatin1Char('/') + id + QStringLiteral(".jpg");
    if (QFileInfo(path).isFile()) {
        artVideoId_ = id;
        artPath_ = path;
        emit artChanged();
        return;
    }
    artVideoId_ = id;
    artPath_.clear();
    QNetworkRequest request(url);
    QNetworkReply *reply = network_->get(request);
    QPointer<PlaybackSession> guard(this);
    connect(reply, &QNetworkReply::finished, this, [this, guard, reply, id, path]() {
        reply->deleteLater();
        if (!guard || reply->error() != QNetworkReply::NoError || id != videoId()) return;
        const QByteArray data = reply->read(4 * 1024 * 1024);
        if (data.isEmpty()) return;
        // Keep only the current track's picture.
        QDir dir(artworkDir_);
        if (!dir.exists()) QDir().mkpath(artworkDir_);
        for (const QString &old : dir.entryList(QStringList() << QStringLiteral("*.jpg"), QDir::Files)) dir.remove(old);
        QFile file(path);
        if (!file.open(QIODevice::WriteOnly) || file.write(data) != data.size()) return;
        file.close();
        artPath_ = path;
        emit artChanged();
    });
}

void PlaybackSession::setLocalMediaLookup(const LocalMediaLookup &lookup) {
    localLookup_ = lookup;
    if (!lookup) { controller_.setLocalAvailable(std::function<bool(const std::string &)>()); return; }
    controller_.setLocalAvailable([this](const std::string &videoId) {
        return localCopy(QString::fromStdString(videoId), QVariantMap(), nullptr);
    });
}

// ---- QML actions ------------------------------------------------------------------------

// PLY-26 / DL-07: the video's download record when it is playable from the device and the hints do
// not ask for the stream (preferStream: true, a bool).
bool PlaybackSession::localCopy(const QString &videoId, const QVariantMap &hints, QVariantMap *record) const {
    if (!localLookup_) return false;
    const QVariant prefer = hints.value(QStringLiteral("preferStream"));
    if (prefer.type() == QVariant::Bool && prefer.toBool()) return false;
    const QVariantMap r = localLookup_(videoId);
    if (!r.value(QStringLiteral("available")).toBool()) return false;  // e.g. error "file_missing": stream
    if (record) *record = r;
    return true;
}

void PlaybackSession::open(const QString &videoId, const QVariantMap &hints) {
    if (videoId.isEmpty()) return;
    const bool same = videoId == this->videoId() && controller_.state() != PlaybackController::Idle &&
                      controller_.state() != PlaybackController::Failed;
    // Shorts: an open of another id ends the preload of a different one; a deferred one waits for
    // the pager to name it again after this open. Re-attaching the playing video keeps both.
    const std::string id = u(videoId);
    if (!same) {
        if (likely_.token && likely_.videoId != id) dropLikely("open");
        likelyPendingId_.clear();
        likelyPendingHints_.clear();
        dropAdopted();  // a used preload whose load never came
    }
    const QVariant loopValue = hints.value(QStringLiteral("loop"));
    const int loopHint = loopValue.type() != QVariant::Bool ? PlaybackController::kLoopUnset
                       : loopValue.toBool() ? PlaybackController::kLoopOn : PlaybackController::kLoopOff;
    // Shorts: `short: true` marks the item (loop, no resume point, never parked, held in the
    // background); another type keeps the playing item's mark when it is re-attached.
    const QVariant shortValue = hints.value(QStringLiteral("short"));
    const bool shortHint = shortValue.type() == QVariant::Bool && shortValue.toBool();
    const bool shortKnown = shortValue.type() == QVariant::Bool;
    QVariantMap record;
    if (!same && localCopy(videoId, hints, &record)) {
        // No resolve and no network request at all (flight mode); a t= link start is carried.
        if (spec_.token) dropSpeculation();
        const QVariant listen = hints.value(QStringLiteral("audioOnly"));
        if (listen.type() == QVariant::Bool && listen.toBool() != controller_.audioOnly())
            controller_.setAudioOnly(now(), listen.toBool());
        if (!record.contains(QStringLiteral("videoId"))) record.insert(QStringLiteral("videoId"), videoId);
        const double startSeconds = hints.value(QStringLiteral("startSeconds"), -1).toDouble();
        openLocalRecord(record, startSeconds >= 0 ? static_cast<qint64>(startSeconds * 1000.0) : -1, loopHint, shortHint ? 1 : 0);
        return;
    }
    if (!same) {
        localCaptions_.clear();
        localSegments_ = false;
        localSidecarsPending_ = false;
        hints_ = hints;
        hintsVideoId_ = videoId;
        if (cancelMetadata_) cancelMetadata_();
        cancelMetadata_ = nullptr;
        ++metadataToken_;
        prepareSelection();
        // Hints contract (ARCHITECTURE.md §8): `audioOnly` (bool) starts this video in
        // listen-only mode; applied before the first resolve so only one runs (SEL-08).
        // Any other type means "not known": the Settings default from prepareSelection stays.
        const QVariant listen = hints.value(QStringLiteral("audioOnly"));
        if (listen.type() == QVariant::Bool && listen.toBool() != controller_.audioOnly())
            controller_.setAudioOnly(now(), listen.toBool());
        ++run_;
        // A Short's open is the pager's swipe: kind=short, ahead=1 when the preload of this id is
        // there (its resolve answered or in flight), so device runs split swipes from taps.
        const bool ahead = likely_.token && likely_.videoId == id;
        timingMark("tap", QStringLiteral("%1 %2%3").arg(run()).arg(net())
                              .arg(shortHint ? QStringLiteral(" kind=short ahead=%1").arg(ahead ? 1 : 0) : QString()));
    }
    controller_.open(now(), id, loopHint);
    if (!same || shortKnown) controller_.setShortItem(shortHint);  // after open(): its saves follow the previous item
    // LNK-02: a t= link start wins over the stored resume point the library
    // bridge applies when detailsChanged fires below.
    const double startSeconds = hints.value(QStringLiteral("startSeconds"), -1).toDouble();
    linkStartMs_ = !same && startSeconds >= 0 ? static_cast<qint64>(startSeconds * 1000.0) : -1;
    if (linkStartMs_ >= 0) controller_.setStartPosition(linkStartMs_);
    // E12: a press this open did not use (the same video re-attached, or a different request).
    if (spec_.claimed) dropSpeculation();
    sync();
}

void PlaybackSession::openLocal(const QVariantMap &media) { openLocalRecord(media, -1, PlaybackController::kLoopUnset); }

void PlaybackSession::openShort(const QString &videoId, const QVariantMap &hints) {
    QVariantMap h = hints;
    h.insert(QStringLiteral("loop"), true);
    h.insert(QStringLiteral("short"), true);
    open(videoId, h);
}

void PlaybackSession::holdShort(const QString &reason) {
    controller_.holdShort(u(reason));
    sync();
}

void PlaybackSession::releaseShort(const QString &reason) {
    controller_.releaseShort(now(), u(reason));
    sync();
}

void PlaybackSession::streamInstead() {
    if (!canStreamInstead()) return;
    prepareSelection();  // the preferences and network class open() would use (SEL-02)
    controller_.streamInstead(now());
    sync();
}

void PlaybackSession::openLocalRecord(const QVariantMap &media, qint64 startMs, int loopHint, int shortHint) {
    if (!media.value(QStringLiteral("available"), true).toBool()) return;
    LocalMedia m;
    m.videoId = u(media.value(QStringLiteral("videoId")).toString());
    m.layout = u(media.value(QStringLiteral("layout")).toString());
    m.filePath = u(media.value(QStringLiteral("filePath")).toString());
    m.videoPath = u(media.value(QStringLiteral("videoPath")).toString());
    m.audioPath = u(media.value(QStringLiteral("audioPath")).toString());
    m.title = u(media.value(QStringLiteral("title")).toString());
    m.width = media.value(QStringLiteral("width"), 0).toInt();    // videoAspect before the first frame
    m.height = media.value(QStringLiteral("height"), 0).toInt();
    m.durationMs = static_cast<std::int64_t>(media.value(QStringLiteral("durationSeconds"), 0).toDouble() * 1000.0);
    m.startPositionMs = startMs;  // LNK-02
    if (m.filePath.empty() && m.videoPath.empty() && m.audioPath.empty()) return;
    hints_.clear();
    hintsVideoId_ = QString::fromStdString(m.videoId.empty() ? "local:" + m.filePath : m.videoId);
    hints_.insert(QStringLiteral("title"), media.value(QStringLiteral("title")));
    hints_.insert(QStringLiteral("channel"), media.value(QStringLiteral("channel")));
    hints_.insert(QStringLiteral("channelId"), media.value(QStringLiteral("channelId")));
    hints_.insert(QStringLiteral("durationSeconds"), media.value(QStringLiteral("durationSeconds")));
    hints_.insert(QStringLiteral("thumbnailPath"), media.value(QStringLiteral("thumbnailPath")));
    const QString thumb = media.value(QStringLiteral("thumbnailPath")).toString();
    if (!thumb.isEmpty()) hints_.insert(QStringLiteral("thumbnail"), QUrl::fromLocalFile(thumb).toString());
    // DL-07: saved captions for the caption model; SB-02: saved segments for the skipper.
    localCaptions_ = media.value(QStringLiteral("captions")).toList();
    localSidecarsPending_ = media.value(QStringLiteral("sidecarsPending")).toBool();
    const std::vector<player::SponsorSegment> segments = sponsorSegmentsFor(media.value(QStringLiteral("segments")).toList(), 1000.0);
    linkStartMs_ = startMs;  // LNK-02: the link wins over the library bridge's resume
    if (cancelMetadata_) cancelMetadata_();
    cancelMetadata_ = nullptr;
    ++metadataToken_;
    prepareSelection();
    ++run_;
    const std::string id = m.videoId.empty() ? "local:" + m.filePath : m.videoId;
    if (likely_.token && likely_.videoId != id) dropLikely("open");
    lastResolveVideoId_ = id;  // its stream instead is not an open of another id
    controller_.openLocal(now(), m, loopHint);
    controller_.setShortItem(shortHint == 1);
    // DL-07 / SEL-06: the download's saved tracks go to the caption models (no fetch).
    if (captionSource_ && !localCaptions_.isEmpty()) captionSource_->setLocalTracks(videoId(), localCaptions_);
    localSegments_ = false;
    if (controller_.local() && controller_.videoId() == u(hintsVideoId_)) {
        controller_.setSponsorSegments(segments);
        localSegments_ = !segments.empty();
    }
    sync();
}

void PlaybackSession::play() { controller_.play(now()); sync(); }
void PlaybackSession::pause() { controller_.pause(); sync(); }
void PlaybackSession::togglePlayback() { controller_.togglePlayback(now()); sync(); }

void PlaybackSession::stop() {
    linkStartMs_ = -1;
    dropLikely("stop");
    likelyPendingId_.clear();
    likelyPendingHints_.clear();
    controller_.stop();
    closeParked();  // MPR-01: no stream job survives Stop
    dropAdopted();
    if (cancelMetadata_) cancelMetadata_();
    cancelMetadata_ = nullptr;
    sync();
}

void PlaybackSession::seek(double milliseconds) {
    const qint64 target = static_cast<qint64>(qMax(0.0, milliseconds));
    if (player_->state() != player::VideoPlayer::Idle && controller_.started()) {
        ++seekCount_;
        timingMark("seek-start", QStringLiteral("seek=%1 target=%2 container=%3 %4")
                   .arg(seekCount_).arg(target).arg(QLatin1String(containerKey(player_))).arg(net()));
    }
    controller_.seek(target);
    sync();
}

void PlaybackSession::setQuality(int height, bool highFrameRate) {
    media::QualityChoice choice;
    choice.height = qMax(0, height);
    choice.highFrameRate = choice.height > 0 && highFrameRate;
    // SEL-03: the menu entry with that height and rate carries its format id.
    for (const media::QualityOption &q : controller_.qualityOptions())
        if (q.height == choice.height && q.highFrameRate == choice.highFrameRate && !q.premium) {
            choice = media::qualityChoice(q);
            break;
        }
    ++switchCount_;
    timingMark("switch-start", QStringLiteral("switch=%1 kind=quality %2").arg(switchCount_).arg(net()));
    controller_.setQuality(now(), choice);
    sync();
}

void PlaybackSession::setAudioOnlyMode(bool audioOnly) {
    if (audioOnly == controller_.audioOnly()) return;
    if (controller_.started()) {
        ++switchCount_;
        timingMark("switch-start", QStringLiteral("switch=%1 kind=quality %2").arg(switchCount_).arg(net()));
    }
    controller_.setAudioOnly(now(), audioOnly);
    if (settings_) settings_->setAudioOnly(audioOnly);  // per session (SEL-08), never stored
    sync();
}

void PlaybackSession::retry() { controller_.retry(now()); sync(); }

void PlaybackSession::setStartPosition(double milliseconds) {
    if (linkStartMs_ >= 0 && !controller_.started()) return;  // LNK-02: the link wins
    // Shorts: a loop item has no resume point (its link start is applied by open() itself).
    if (controller_.loop()) return;
    controller_.setStartPosition(static_cast<std::int64_t>(qMax(0.0, milliseconds)));
    // PLY-06 / LIB-05: a resume point that arrives after the load (a local file starts in the
    // same call) still lands before the first frame; the player ignores it after that.
    if (player_) player_->setStartPosition(static_cast<qint64>(qMax(0.0, milliseconds)));
}

void PlaybackSession::setRate(double rate) { controller_.setRate(rate); sync(); }

int PlaybackSession::claimContext() {
    const int ctx = controller_.claimContext();
    sync();
    return ctx;
}

void PlaybackSession::activateContext(int contextId) { controller_.activateContext(now(), contextId); sync(); }
void PlaybackSession::closeContext(int contextId) { controller_.closeContext(now(), contextId); sync(); }

void PlaybackSession::setQueue(const QStringList &videoIds, int currentIndex) {
    std::vector<std::string> ids;
    for (const QString &id : videoIds) ids.push_back(u(id));
    controller_.setQueue(ids, currentIndex);
    sync();
}

void PlaybackSession::clearQueue() { controller_.clearQueue(); sync(); }
void PlaybackSession::setRelatedCandidate(const QString &videoId) { controller_.setRelatedCandidate(u(videoId)); }

void PlaybackSession::next() {
    prepareSelection();
    controller_.next(now());
    sync();
}

void PlaybackSession::previous() {
    prepareSelection();
    controller_.previous(now());
    sync();
}

void PlaybackSession::cancelUpNext() { controller_.cancelUpNext(); sync(); }

void PlaybackSession::setSponsorSegments(const QVariantList &segments) {
    // SB-02 "segments are cached with downloads": a download's saved segments are not replaced
    // by a SponsorBlock answer (offline, that answer is empty).
    if (localSegments_ && controller_.local()) return;
    controller_.setSponsorSegments(sponsorSegmentsFor(segments, 1.0));
    sync();
}

void PlaybackSession::skipSponsor() { controller_.skipSponsor(); sync(); }

void PlaybackSession::continueWatching() {
    prepareSelection();
    hints_.clear();
    controller_.continueWatching(now());
    sync();
}

void PlaybackSession::surfaceFrameReady() {
    if (frameMarkedRun_ == run_ || !controller_.hasVideo()) return;
    frameMarkedRun_ = run_;
    awaitingSwapRun_.store(run_);
}

void PlaybackSession::frameSwapped() {
    const int r = awaitingSwapRun_.exchange(0);
    if (r) timingMark("first-frame", QStringLiteral("run=%1").arg(r));
}

void PlaybackSession::setControlsVisible(bool visible) {
    controlsVisible_ = visible;
    updatePositionCadence();
}

void PlaybackSession::setCoverVideoEnabled(bool enabled) {
#ifdef SAILTUBE_STORE
    enabled = false;  // no cover video in STORE
#endif
    if (enabled == controller_.coverVideoSetting()) return;
    controller_.setCoverVideoSetting(enabled);
    sync();
}

void PlaybackSession::setCoverVisible(bool visible) {
    controller_.setCoverVisible(visible);
    sync();
}

void PlaybackSession::setAppActive(bool active) {
    timingMark(active ? "shown" : "hidden");
    player_->setAppActive(active);  // PLY-09
    if (!active) {  // E12 / Shorts likelyNext: never in the background
        dropSpeculation();
        dropLikely("background");
        likelyPendingId_.clear();
        likelyPendingHints_.clear();
    }
    controller_.setWallClock(now(), QDateTime::currentMSecsSinceEpoch());  // RES-19 after a suspend
    controller_.setAppActive(now(), active);
    updatePositionCadence();
    sync();
}

// ---- Watch overlay ------------------------------------------------------------------------------

double PlaybackSession::bufferedPosition() const {
    return player_->state() == player::VideoPlayer::Idle ? 0.0 : player_->bufferedPosition();
}

QVariantList PlaybackSession::audioTracks() const {
    QVariantList list;
    const std::vector<media::AudioTrackOption> options = controller_.audioTrackOptions();
    const int playing = controller_.playingAudioTrackIndex();
    for (std::size_t i = 0; i < options.size(); ++i) {
        const media::AudioTrackOption &o = options[i];
        QVariantMap m;
        m.insert(QStringLiteral("id"), s(audioTrackId(o)));
        m.insert(QStringLiteral("label"), audioTrackLabel(o));
        m.insert(QStringLiteral("language"), s(o.language));
        m.insert(QStringLiteral("original"), o.original);
        m.insert(QStringLiteral("descriptive"), o.descriptive);
        m.insert(QStringLiteral("selected"), static_cast<int>(i) == playing);
        list.append(m);
    }
    return list;
}

void PlaybackSession::setAudioTrack(const QString &id) {
    controller_.setAudioTrack(now(), u(id));
    // SEL-05: the pick is remembered for later videos and sessions ("" = the original track).
    if (settings_ && controller_.dubRemembered()) {
        const QString dub = s(controller_.rememberedDub());
        if (languageTagLooksValid(dub) && settings_->value(QStringLiteral("playback/dubLanguage")).toString() != dub)
            settings_->setValue(QStringLiteral("playback/dubLanguage"), dub);
    }
    sync();
}

void PlaybackSession::setStableVolume(bool on) {
    controller_.setStableVolume(now(), on);
    if (settings_ && settings_->value(QStringLiteral("playback/stableVolume")).toBool() != on)
        settings_->setValue(QStringLiteral("playback/stableVolume"), on);
    sync();
}

void PlaybackSession::setLoop(bool on) {
    controller_.setLoop(on);
    sync();
}

void PlaybackSession::holdSpeed(bool held) {
    controller_.holdSpeed(held);
    sync();
}

void PlaybackSession::setSleepTimer(int minutes) {
    controller_.setSleepTimer(now(), minutes);
    sync();
}

bool PlaybackSession::sleepTimerShown() const {
    static const QMetaMethod changed = QMetaMethod::fromSignal(&PlaybackSession::sleepTimerChanged);
    return controller_.appActive() && isSignalConnected(changed);
}

QVariantList PlaybackSession::sponsorSegments() const {
    QVariantList list;
    for (const player::SponsorSegment &seg : controller_.sponsorSegments()) {
        QVariantMap m;
        m.insert(QStringLiteral("startMs"), static_cast<double>(seg.startMs));
        m.insert(QStringLiteral("endMs"), static_cast<double>(seg.endMs));
        m.insert(QStringLiteral("category"), QString::fromLatin1(player::sponsorCategoryInfo(seg.category).apiName));
        list.append(m);
    }
    return list;
}

void PlaybackSession::setCaptionSource(PlaybackCaptionSource *source) {
    if (captionSource_) captionSource_->setChangedCallback(std::function<void()>());
    captionSource_ = source;
    if (source) {
        QPointer<PlaybackSession> guard(this);
        source->setChangedCallback([guard]() { if (guard) guard->captionsUpdated(); });
    }
    captionsUpdated();
}

QVariantList PlaybackSession::captionTracks() const {
    return captionSource_ ? captionSource_->tracks(videoId()) : QVariantList();
}

QString PlaybackSession::captionTrackId() const {
    return captionSource_ ? captionSource_->currentTrackId(videoId()) : QString();
}

void PlaybackSession::setCaptionTrack(const QString &id) {
    if (captionSource_) captionSource_->select(videoId(), id);  // the change callback updates
}

void PlaybackSession::captionsUpdated() {
    captionTrackNotified_ = videoId();
    emit captionsChanged();
    updateCaptionText();
    updatePositionCadence();
}

void PlaybackSession::updateCaptionText() {
    const QString text = captionSource_ && !captionTrackId().isEmpty()
                       ? captionSource_->textAt(static_cast<qint64>(position())) : QString();
    if (text == captionText_) return;
    captionText_ = text;
    emit captionTextChanged();
}

// PLY-24: the 250 ms poll while the controls show, and while captions are on screen.
void PlaybackSession::updatePositionCadence() {
    const bool fast = controlsVisible_ || (controller_.appActive() && !captionTrackId().isEmpty());
    if (fast == positionFast_) return;
    positionFast_ = fast;
    player_->setControlsVisible(fast);
}

void PlaybackSession::setStoryboard(const QVariantMap &storyboard) {
    storyboard_ = storyboardFor(storyboard);
    storyboardVideoId_ = controller_.videoId();
}

QVariantMap PlaybackSession::scrubPreview(double positionMs) const {
    QVariantMap m;
    StoryboardCell cell;
    if (storyboardVideoId_.empty() || storyboardVideoId_ != controller_.videoId() ||
        !storyboardCell(storyboard_, static_cast<std::int64_t>(positionMs), &cell))
        return m;
    m.insert(QStringLiteral("url"), s(cell.url));
    m.insert(QStringLiteral("x"), cell.x);
    m.insert(QStringLiteral("y"), cell.y);
    m.insert(QStringLiteral("width"), cell.width);
    m.insert(QStringLiteral("height"), cell.height);
    return m;
}

QString PlaybackSession::scrubPreviewUrl(double positionMs) const {
    return scrubPreview(positionMs).value(QStringLiteral("url")).toString();
}

// ---- E12 speculative resolve and row prefetch (RES-11, RES-21) -------------------------------

void PlaybackSession::setSpeculativeResolve(bool on) {
    speculativeResolve_ = on;
    if (!on) dropSpeculation();
}

SpeculationContext PlaybackSession::speculationContext(int networkClass) const {
    SpeculationContext c;
    c.nowMs = now();
    c.appActive = controller_.appActive();
    c.networkClass = networkClass;
    c.starting = controller_.starting();
    c.lastStartMs = lastStartMs_;
    return c;
}

// The request open(videoId, hints) would make (beginOpen: Auto quality, listen-only from the hint
// or the session), marked speculative (RES-11: never starts helpers).
playback::StreamRequest PlaybackSession::speculativeRequest(const std::string &videoId, bool audioOnly,
                                                            const media::PlaybackPreferences &preferences,
                                                            int networkClass) const {
    media::PlaybackPreferences p = preferences;
    p.mode = audioOnly ? media::PlaybackMode::AudioOnly : media::PlaybackMode::Video;
    ResolveRequest r = resolveRequestFor(videoId, capabilities_, p, media::QualityChoice());
    r.speculative = true;
    return streamRequestFor(r, networkClass, poProviderReady());
}

void PlaybackSession::pressStarted(const QString &videoId, const QVariantMap &hints, int pressId) {
    const std::string id = u(videoId);
    if (!speculativeResolve_ || !resolver_ || !catalog::validVideoId(id)) return;
    if (spec_.attached) return;  // an open uses the slot; its start's quiet window applies anyway
    if (spec_.token && spec_.pressId == pressId && spec_.videoId == id) return;  // the same press again
    dropSpeculation();  // latest press wins
    int networkClass = -1;
    const media::PlaybackPreferences preferences = selectionPreferences(&networkClass);
    if (!speculationAllowed(speculationContext(networkClass))) return;
    if (controller_.resolveBlocked(now(), id)) return;  // RES-19: upcoming, not before its start
    // open() of the session's own video re-attaches without a resolve.
    if (id == controller_.videoId() && controller_.state() != PlaybackController::Idle &&
        controller_.state() != PlaybackController::Failed)
        return;
    if (localCopy(videoId, hints, nullptr)) return;  // DL-07: the open plays the download, no network
    // Shorts: likelyNext already asks for this id (one in flight per id); the open uses that.
    if ((likely_.token && !likely_.attached && likely_.videoId == id) || likelyPendingId_ == id) return;
    const QVariant listen = hints.value(QStringLiteral("audioOnly"));
    const bool audioOnly = listen.type() == QVariant::Bool ? listen.toBool() : controller_.audioOnly();
    const playback::StreamRequest request = speculativeRequest(id, audioOnly, preferences, networkClass);
    const quint64 token = ++specTokens_;
    spec_.token = token;
    spec_.pressId = pressId;
    spec_.videoId = id;
    spec_.key = playback::streamCacheKey(request);
    ++specStats_.started;
    timingMark("speculative-start", QStringLiteral("run=%1").arg(pressId));
    QPointer<PlaybackSession> guard(this);
    const quint64 op = resolver_->resolve(request, [guard, token](const playback::StreamSet &set, const playback::StreamError &error) {
        if (guard) guard->speculationDone(token, set, error);
    });
    if (spec_.token == token && !spec_.done) spec_.op = op;
}

void PlaybackSession::speculationDone(quint64 token, const playback::StreamSet &set, const playback::StreamError &error) {
    if (!token || token != spec_.token) return;  // cancelled or superseded
    spec_.op = 0;
    if (spec_.attached) {  // an open waits: this is its answer
        const quint64 resolveToken = spec_.resolveToken;
        const std::string videoId = spec_.videoId;
        const unsigned generation = spec_.generation;
        spec_ = Speculation();
        if (resolveToken != resolveToken_) return;
        resolveOp_ = 0;
        onStreams(videoId, generation, set, error, error.ok());
        sync();
        return;
    }
    if (!error.ok()) {  // an open will resolve itself
        ++specStats_.wasted;
        spec_ = Speculation();
        return;
    }
    spec_.done = true;
    spec_.set = set;
}

void PlaybackSession::pressCancelled(const QString &videoId, int pressId) {
    if (!spec_.token || spec_.attached || spec_.pressId != pressId || spec_.videoId != u(videoId)) return;
    dropSpeculation();
}

void PlaybackSession::openingFromPress(const QString &videoId, int pressId) {
    if (spec_.token && !spec_.attached && spec_.pressId == pressId && spec_.videoId == u(videoId)) spec_.claimed = true;
}

// Forgets the speculation no open() uses: a running one is cancelled (only this caller's share of
// the work: a joined resolve keeps running for its other callers), a completed answer stays in the
// resolver's cache. Either way it was not used (speculative-cancel, wasted).
void PlaybackSession::dropSpeculation() {
    if (!spec_.token || spec_.attached) return;
    const quint64 op = spec_.op;
    const int pressId = spec_.pressId;
    spec_ = Speculation();
    ++specStats_.wasted;
    if (op && resolver_) resolver_->cancel(op);
    timingMark("speculative-cancel", QStringLiteral("run=%1").arg(pressId));
}

// Port::resolve: a claimed speculation with the same RES-10 key answers this resolve (joined while
// it runs, delivered next turn when it already answered).
bool PlaybackSession::takeSpeculation(const playback::StreamRequest &request, const std::string &videoId,
                                      unsigned generation, quint64 token) {
    if (!spec_.token || !spec_.claimed || spec_.attached || spec_.videoId != videoId) return false;
    if (playback::streamCacheKey(request) != spec_.key) {  // settings or hints changed since the press
        dropSpeculation();
        return false;
    }
    ++specStats_.used;
    timingMark("speculative-used", QStringLiteral("run=%1").arg(spec_.pressId));
    if (spec_.done) {
        const playback::StreamSet set = spec_.set;
        spec_ = Speculation();
        deferred([this, token, videoId, generation, set]() {
            if (token != resolveToken_) return;
            onStreams(videoId, generation, set, playback::StreamError(), true);
        });
        return true;
    }
    spec_.claimed = false;
    spec_.attached = true;
    spec_.resolveToken = token;
    spec_.generation = generation;
    resolveOp_ = spec_.op;
    return true;
}

void PlaybackSession::rowsVisible(const QStringList &videoIds) {
    if (!resolver_) return;
    int networkClass = -1;
    const media::PlaybackPreferences preferences = selectionPreferences(&networkClass);
    std::vector<std::string> ids;
    const qint64 at = now();
    for (const QString &id : videoIds) {
        const std::string v = u(id);
        if (!controller_.resolveBlocked(at, v)) ids.push_back(v);  // RES-19
    }
    const bool active = controller_.state() != PlaybackController::Idle && controller_.state() != PlaybackController::Failed;
    const RowsVisibleDecision d = rowsVisibleDecision(speculationContext(networkClass), ids,
                                                      active ? controller_.videoId() : std::string(),
                                                      spec_.op != 0 && !spec_.attached, rows_);
    const qint64 t = now();
    // Shorts: the row the pager preloads with likelyNext is not prefetched a second time.
    const char *reason = d.reason;
    std::string prefetchId = d.prefetchVideoId;
    if (!prefetchId.empty() && ((likely_.token && likely_.videoId == prefetchId) || likelyPendingId_ == prefetchId)) {
        prefetchId.clear();
        reason = "likely-next";
    }
    // E12 / RES-11 device runs: one line per likelyVisible, with the outcome and the reason.
    timingMark("rows-visible", QStringLiteral("n=%1 decision=%2 reason=%3 net=%4")
                                   .arg(videoIds.size())
                                   .arg(QLatin1String(!prefetchId.empty() ? "prefetch" : d.warmUp ? "warmup" : "skip"))
                                   .arg(QLatin1String(*reason ? reason : "-"))
                                   .arg(networkClass));
    if (!prefetchId.empty()) {
        resolver_->prefetch(speculativeRequest(prefetchId, controller_.audioOnly(), preferences, networkClass));
        timingMark("row-prefetch", QStringLiteral("video=%1").arg(QString::fromStdString(prefetchId)));
        rows_.lastVideoId = prefetchId;
        rows_.lastPrefetchMs = t;
        ++specStats_.rowPrefetches;
    }
    if (d.warmUp && warmUp_) {
        rows_.lastWarmUpMs = t;
        ++specStats_.warmUps;
        warmUp_();
    }
}

// ---- Shorts likelyNext (DSC-17 MAY swipe viewer) -------------------------------------------------

void PlaybackSession::likelyNext(const QString &videoId, const QVariantMap &hints) {
    const std::string id = u(videoId);
    LikelyNextContext c;
    c.appActive = controller_.appActive();
    c.resolverAvailable = resolver_ != nullptr;
    c.validId = catalog::validVideoId(id);
    c.sessionVideo = id == controller_.videoId() && controller_.state() != PlaybackController::Idle &&
                     controller_.state() != PlaybackController::Failed;
    c.local = c.validId && localCopy(videoId, hints, nullptr);
    c.blocked = controller_.resolveBlocked(now(), id);
    c.alreadyPreloaded = (likely_.token && !likely_.attached && likely_.videoId == id) || likelyPendingId_ == id;
    c.pressSpeculation = spec_.token && spec_.videoId == id;
    // An open still waiting for the previous preload's answer counts as a start.
    c.starting = controller_.starting() || likely_.attached;
    const LikelyNextDecision d = likelyNextDecision(c);
    const char *decision = d.action == LikelyNextDecision::Start ? "start" : d.action == LikelyNextDecision::Defer ? "defer"
                         : d.action == LikelyNextDecision::Keep ? "keep" : "skip";
    timingMark("likely-next", QStringLiteral("video=%1 decision=%2 reason=%3")
                                  .arg(c.validId ? videoId : QStringLiteral("-"), QLatin1String(decision),
                                       QLatin1String(*d.reason ? d.reason : "-")));
    if (d.action == LikelyNextDecision::Skip || d.action == LikelyNextDecision::Keep) return;
    dropLikely("replaced");  // one preloaded id at a time
    likelyPendingId_.clear();
    likelyPendingHints_.clear();
    if (d.action == LikelyNextDecision::Defer) {
        likelyPendingId_ = id;
        likelyPendingHints_ = hints;
        return;
    }
    startLikely(id, hints);
}

// The pager's next Shorts: the first one fully prepared (likelyNext), the second one resolved into
// the cache only. Nothing in the background (likelyNext decides for the first).
void PlaybackSession::shortsAhead(const QStringList &videoIds) {
    std::vector<std::string> ids;
    for (const QString &raw : videoIds) {
        const std::string id = u(raw);
        if (!catalog::validVideoId(id) || id == controller_.videoId()) continue;
        if (std::find(ids.begin(), ids.end(), id) != ids.end()) continue;
        ids.push_back(id);
        if (ids.size() == 2) break;
    }
    ++shortsAheadCalls_;
    timingMark("shorts-ahead", QStringLiteral("n=%1 second=%2").arg(ids.size())
                                   .arg(ids.size() > 1 ? QString::fromStdString(ids[1]) : QStringLiteral("-")));
    if (ids.empty()) return;
    likelyNext(s(ids[0]), QVariantMap());
    if (ids.size() > 1) prepareShortCache(ids[1]);
}

// One speculative resolve into the resolver's cache for a Short two swipes away: no stream jobs.
void PlaybackSession::prepareShortCache(const std::string &videoId) {
    if (!resolver_ || !controller_.appActive() || controller_.resolveBlocked(now(), videoId)) return;
    if (likely_.token && likely_.videoId == videoId) return;  // already the preloaded one
    if (std::find(shortCacheIds_.begin(), shortCacheIds_.end(), videoId) != shortCacheIds_.end()) return;
    shortCacheIds_.push_back(videoId);
    if (shortCacheIds_.size() > 4) shortCacheIds_.erase(shortCacheIds_.begin());
    int networkClass = -1;
    const media::PlaybackPreferences preferences = selectionPreferences(&networkClass);
    resolver_->prefetch(speculativeRequest(videoId, controller_.audioOnly(), preferences, networkClass));
    ++shortsAheadCached_;
    timingMark("shorts-cache", QStringLiteral("video=%1").arg(QString::fromStdString(videoId)));
}

// The deferred likelyNext, once the play that was starting has prerolled (sync()).
void PlaybackSession::runPendingLikely() {
    const QString id = s(likelyPendingId_);
    const QVariantMap hints = likelyPendingHints_;
    likelyPendingId_.clear();
    likelyPendingHints_.clear();
    likelyNext(id, hints);
}

// One speculative resolve with open()'s request (pressStarted's): Auto quality, listen-only from
// the hint or the session, the selection preferences open() would apply now.
void PlaybackSession::startLikely(const std::string &videoId, const QVariantMap &hints) {
    int networkClass = -1;
    media::PlaybackPreferences preferences = selectionPreferences(&networkClass);
    const QVariant listen = hints.value(QStringLiteral("audioOnly"));
    const bool audioOnly = listen.type() == QVariant::Bool ? listen.toBool() : controller_.audioOnly();
    const playback::StreamRequest request = speculativeRequest(videoId, audioOnly, preferences, networkClass);
    preferences.mode = audioOnly ? media::PlaybackMode::AudioOnly : media::PlaybackMode::Video;
    const quint64 token = ++likelyTokens_;
    likely_ = Likely();
    likely_.token = token;
    likely_.videoId = videoId;
    likely_.key = playback::streamCacheKey(request);
    likely_.preferences = preferences;
    ++likelyStarted_;
    QPointer<PlaybackSession> guard(this);
    const quint64 op = resolver_->resolve(request, [guard, token](const playback::StreamSet &set, const playback::StreamError &error) {
        if (guard) guard->likelyAnswered(token, set, error);
    });
    if (likely_.token == token && !likely_.done) likely_.op = op;
}

void PlaybackSession::likelyAnswered(quint64 token, const playback::StreamSet &set, const playback::StreamError &error) {
    if (!token || token != likely_.token) return;  // dropped or replaced
    likely_.op = 0;
    if (likely_.attached) {  // an open waits: this is its answer
        const quint64 resolveToken = likely_.resolveToken;
        const std::string videoId = likely_.videoId;
        const unsigned generation = likely_.generation;
        likely_ = Likely();
        if (resolveToken != resolveToken_) return;
        resolveOp_ = 0;
        onStreams(videoId, generation, set, error, error.ok());
        sync();
        return;
    }
    if (!error.ok() || (!set.videoId.isEmpty() && set.videoId != s(likely_.videoId))) {  // an open resolves itself
        ++likelyWasted_;
        likely_ = Likely();
        return;
    }
    likely_.done = true;
    likely_.set = set;
    // The plan open() would start with; its branches' first bytes through the fetcher, held.
    PlaybackLoad load;
    if (fetcher_ && firstLoadFor(likely_.videoId, selectionInputFor(set), capabilities_, likely_.preferences,
                                 controller_.startLowRung(), &load, controller_.stableVolume())) {
        std::map<std::string, playback::StreamFormat> formats;
        for (const QList<playback::StreamFormat> *list : {&set.video, &set.audio, &set.muxed})
            for (const playback::StreamFormat &f : *list) formats[f.url.toEncoded().toStdString()] = f;
        const player::SourceSpec *specs[3] = {&load.video, &load.audio, &load.muxed};
        QPointer<PlaybackSession> guard(this);
        for (int slot = 0; slot < 3; ++slot) {
            const player::SourceSpec &spec = *specs[slot];
            if (!spec.present() || spec.uri.compare(0, 7, "file://") == 0) continue;
            const std::map<std::string, playback::StreamFormat>::const_iterator known = formats.find(spec.uri);
            if (known == formats.end() || known->second.isHls) continue;  // RES-19: a manifest opens in the player
            Stream st;
            st.remote = spec.uri;
            st.format = known->second;
            likely_.held.push_back(st);
        }
        for (std::size_t i = 0; i < likely_.held.size(); ++i) {
            // The handle is known before the answer (completions are asynchronous, StreamSource.h).
            std::shared_ptr<quint64> handle(new quint64(0));
            *handle = fetcher_->open(likely_.held[i].format, [guard, handle](const QUrl &local, const playback::StreamError &e) {
                if (guard) guard->likelyStreamOpened(*handle, local, e);
            });
            likely_.held[i].handle = *handle;
        }
    }
    timingMark("likely-held", QStringLiteral("video=%1 jobs=%2").arg(s(likely_.videoId)).arg(likely_.held.size()));
    likelyHold_.start(controller_.shortItem() ? kShortsAheadHoldMs : kLikelyNextHoldMs);
}

// A held job's open answered: still held, adopted by the next load (still opening), or gone.
void PlaybackSession::likelyStreamOpened(quint64 handle, const QUrl &local, const playback::StreamError &error) {
    if (!handle) return;
    for (std::vector<Stream> *list : {&likely_.held, &adopted_}) {
        for (std::vector<Stream>::iterator it = list->begin(); it != list->end(); ++it) {
            if (it->handle != handle) continue;
            if (!error.ok() || local.isEmpty()) {  // not servable: the open will open it itself
                if (fetcher_) fetcher_->close(handle);
                list->erase(it);
            } else {
                it->local = local;
                it->ready = true;
            }
            return;
        }
    }
    for (std::size_t i = 0; i < streams_.size(); ++i)
        if (streams_[i].handle == handle && !streams_[i].ready) {
            branchOpened(loadGeneration_, static_cast<int>(i), local, error);
            return;
        }
}

// Port::resolve: the preload of this video with the same RES-10 key answers the open's resolve
// (joined while it runs, delivered next turn when it already answered); its held jobs wait in
// adopted_ for the load (openBranches).
bool PlaybackSession::takeLikely(const playback::StreamRequest &request, const std::string &videoId, unsigned generation,
                                 quint64 token) {
    if (!likely_.token || likely_.attached || likely_.videoId != videoId) return false;
    if (playback::streamCacheKey(request) != likely_.key) {  // settings or hints changed since
        dropLikely("request");
        return false;
    }
    ++likelyUsed_;
    timingMark("speculative-used", QStringLiteral("run=%1 kind=likely").arg(run_));
    likelyHold_.stop();
    dropAdopted();
    adopted_.swap(likely_.held);
    if (likely_.done) {
        const playback::StreamSet set = likely_.set;
        likely_ = Likely();
        deferred([this, token, videoId, generation, set]() {
            if (token != resolveToken_) return;
            onStreams(videoId, generation, set, playback::StreamError(), true);
        });
        return true;
    }
    likely_.attached = true;
    likely_.resolveToken = token;
    likely_.generation = generation;
    resolveOp_ = likely_.op;
    return true;
}

// Forgets the preload no open uses: its resolve is cancelled (only this caller's share), its held
// jobs are closed; a completed answer stays in the resolver's cache. Marker likely-drop.
void PlaybackSession::dropLikely(const char *reason) {
    if (!likely_.token || likely_.attached) return;
    const quint64 op = likely_.op;
    const QString videoId = s(likely_.videoId);
    std::vector<Stream> held;
    held.swap(likely_.held);
    likely_ = Likely();
    likelyHold_.stop();
    ++likelyWasted_;
    if (op && resolver_) resolver_->cancel(op);
    const int jobs = static_cast<int>(held.size());
    closeStreams(&held);
    timingMark("likely-drop", QStringLiteral("video=%1 reason=%2 jobs=%3").arg(videoId, QLatin1String(reason), QString::number(jobs)));
}

void PlaybackSession::dropAdopted() { closeStreams(&adopted_); }

}}}
