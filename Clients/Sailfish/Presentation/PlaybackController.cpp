// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "PlaybackController.h"

#include "Core/Player/Domain/ResumePolicy.h"
#include "Core/Player/Domain/SpeedPolicy.h"

#include <algorithm>
#include <cmath>

namespace nordtile { namespace video { namespace sailfish {

namespace {

player::Container playerContainer(media::Container c, const media::MediaFormat &f) {
    if (c == media::Container::None) c = media::containerOf(f);
    switch (c) {
    case media::Container::WebM: return player::Container::Matroska;
    case media::Container::Mp4: return player::Container::IsoBmff;
    default: break;
    }
    return player::containerFromExtension(f.container);
}

std::string lower(const std::string &s) {
    std::string out(s);
    for (std::size_t i = 0; i < out.size(); ++i)
        if (out[i] >= 'A' && out[i] <= 'Z') out[i] = static_cast<char>(out[i] - 'A' + 'a');
    return out;
}

player::SourceSpec specFor(const media::MediaFormat &f, media::Container c) {
    player::SourceSpec s;
    s.uri = f.url;
    s.container = playerContainer(c, f);
    for (std::map<std::string, std::string>::const_iterator it = f.headers.begin(); it != f.headers.end(); ++it) {
        if (lower(it->first) == "user-agent") s.userAgent = it->second;
        else s.extraHeaders[it->first] = it->second;
    }
    return s;
}

// file:// URI for a local path; percent-encodes outside [A-Za-z0-9-._~/].
std::string fileUri(const std::string &path) {
    static const char hex[] = "0123456789ABCDEF";
    std::string out = "file://";
    for (std::size_t i = 0; i < path.size(); ++i) {
        const unsigned char c = static_cast<unsigned char>(path[i]);
        const bool plain = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' ||
                           c == '.' || c == '_' || c == '~' || c == '/';
        if (plain) { out += static_cast<char>(c); continue; }
        out += '%';
        out += hex[c >> 4];
        out += hex[c & 15];
    }
    return out;
}

player::SourceSpec localSpec(const std::string &path) {
    player::SourceSpec s;
    s.uri = fileUri(path);
    const std::string::size_type dot = path.rfind('.');
    s.container = dot == std::string::npos ? player::Container::Unknown : player::containerFromExtension(path.substr(dot));
    return s;
}

template <typename T> void keepEarliest(std::int64_t &best, T candidate) {
    if (candidate >= 0 && (best < 0 || candidate < best)) best = candidate;
}

} // namespace

PlaybackLoad loadForCandidate(const media::StreamCandidate &c, const std::string &videoId) {
    PlaybackLoad l;
    l.videoId = videoId;
    switch (c.kind) {
    case media::StreamKind::Adaptive:
        l.mode = player::PlayMode::Dual;
        l.video = specFor(c.video, c.videoContainer);
        l.audio = specFor(c.audio, c.audioContainer);
        break;
    case media::StreamKind::Live:
        l.mode = player::PlayMode::Hls;
        l.muxed = specFor(c.muxed, c.muxedContainer);
        l.muxed.container = player::Container::Unknown;
        l.liveHls = true;
        break;
    case media::StreamKind::AudioOnly:
        l.mode = player::PlayMode::AudioOnly;
        if (!c.audio.url.empty()) l.audio = specFor(c.audio, c.audioContainer);
        else l.muxed = specFor(c.muxed, c.muxedContainer);
        break;
    case media::StreamKind::Progressive:
    case media::StreamKind::None:
        l.mode = player::PlayMode::Muxed;
        l.muxed = specFor(c.muxed, c.muxedContainer);
        break;
    }
    // PLY-21 interim rule: this item has no software-decodable format.
    l.decode = c.hardwareDecodeFallback ? player::DecodeMode::Hardware : player::DecodeMode::Software;
    return l;
}

// ---- Watch overlay controls: audio choice and storyboard (pure) -------------------------------

void applyAudioChoice(std::vector<media::StreamCandidate> *chain, const media::SelectionInput &in,
                      const media::DecoderCapabilities &caps, const AudioChoice &choice) {
    if (!chain || chain->empty() || (!choice.chosen && !choice.stableVolume)) return;
    const std::vector<media::MediaFormat> pool = media::playableAudioFormats(in, caps);
    for (std::size_t i = 0; i < chain->size(); ++i) {
        media::StreamCandidate &c = (*chain)[i];
        if ((c.kind != media::StreamKind::Adaptive && c.kind != media::StreamKind::AudioOnly) || c.audio.url.empty())
            continue;
        const std::string language = choice.chosen ? choice.language : media::normalizedLanguage(c.audio.language);
        const bool descriptive = choice.chosen ? choice.descriptive : media::descriptiveTrack(c.audio);
        const media::MediaFormat *best =
            media::bestEntryTrack(pool, language, descriptive, choice.stableVolume, c.audio.audioCodec);
        if (!best || best->url == c.audio.url) continue;
        c.audio = *best;
        c.audioContainer = media::containerOf(*best);
        if (c.kind == media::StreamKind::Adaptive) {
            const std::string::size_type plus = c.label.find(" + ");
            if (plus != std::string::npos)
                c.label = c.label.substr(0, plus) + " + " + media::audioCodecName(best->audioCodec) + " (dual)";
        }
    }
}

std::string audioTrackId(const media::AudioTrackOption &o) {
    return (o.language.empty() ? std::string("und") : o.language) + (o.descriptive ? "#ad" : "");
}

bool storyboardCell(const Storyboard &b, std::int64_t positionMs, StoryboardCell *cell) {
    const int cells = b.columns * b.rows;
    if (b.width <= 0 || b.height <= 0 || b.columns <= 0 || b.rows <= 0 || b.fragments.empty() ||
        b.fragments[0].durationMs <= 0)
        return false;
    const double cellMs = static_cast<double>(b.fragments[0].durationMs) / cells;
    if (positionMs < 0) positionMs = 0;
    std::int64_t start = 0;
    std::size_t f = 0;
    for (; f + 1 < b.fragments.size(); ++f) {
        if (b.fragments[f].durationMs <= 0) return false;
        if (positionMs < start + b.fragments[f].durationMs) break;
        start += b.fragments[f].durationMs;
    }
    const StoryboardFragment &fragment = b.fragments[f];
    if (fragment.url.empty()) return false;
    int index = static_cast<int>(std::floor(static_cast<double>(positionMs - start) / cellMs));
    // The last fragment may hold fewer cells: never past its own duration.
    const std::int64_t length = fragment.durationMs > 0 ? fragment.durationMs : b.fragments[0].durationMs;
    const int used = std::max(1, std::min(cells, static_cast<int>(std::ceil(static_cast<double>(length) / cellMs))));
    index = std::max(0, std::min(index, used - 1));
    if (cell) {
        cell->url = fragment.url;
        cell->width = b.width;
        cell->height = b.height;
        cell->x = (index % b.columns) * b.width;
        cell->y = (index / b.columns) * b.height;
    }
    return true;
}

ResolveRequest resolveRequestFor(const std::string &videoId, const media::DecoderCapabilities &caps,
                                 const media::PlaybackPreferences &prefs, const media::QualityChoice &choice) {
    ResolveRequest r;
    r.videoId = videoId;
    r.audioOnly = prefs.mode == media::PlaybackMode::AudioOnly;
    r.hardwareDecode = media::hardwareDecodeActive(caps, prefs);
    // SEL-02: the Auto ceiling; an explicit taller choice raises it (a "best" ceiling is none).
    const int ceiling = media::qualityCeiling(caps, prefs);
    r.maxHeight = std::max(ceiling, choice.height);
    if (r.maxHeight >= 100000) r.maxHeight = 0;
    r.highFrameRate = choice.height > 0 && choice.highFrameRate;
    // SEL-01 / SEL-02 codec order.
    const char *h264 = "avc1", *vp9 = "vp09";
    bool vp9First = r.hardwareDecode;
    if (prefs.codec == media::CodecPreference::H264) vp9First = false;
    else if (prefs.codec == media::CodecPreference::Vp9) vp9First = true;
    r.videoCodecs.push_back(vp9First ? vp9 : h264);
    r.videoCodecs.push_back(vp9First ? h264 : vp9);
    if (caps.av1SelfTestPassed) r.videoCodecs.push_back("av01");
    r.dubLanguage = prefs.audioLanguage;
    return r;
}

// ---- E12 / RES-11 / RES-21 speculation bounds -----------------------------------------------

namespace {
bool withinQuiet(const SpeculationContext &c) {
    return c.lastStartMs >= 0 && c.nowMs - c.lastStartMs < kSpeculationQuietMs;
}
}

bool speculationAllowed(const SpeculationContext &c) { return c.appActive && !withinQuiet(c); }

RowsVisibleDecision rowsVisibleDecision(const SpeculationContext &c, const std::vector<std::string> &ids,
                                        const std::string &sessionVideoId, bool speculationInFlight,
                                        const RowPrefetchState &state) {
    RowsVisibleDecision d;
    if (!c.appActive) { d.reason = "background"; return d; }
    if (c.networkClass != 0) { d.reason = c.networkClass < 0 ? "unknown-network" : "metered"; return d; }
    if (c.starting) { d.reason = "starting"; return d; }
    if (withinQuiet(c)) { d.reason = "quiet"; return d; }
    d.warmUp = state.lastWarmUpMs < 0 || c.nowMs - state.lastWarmUpMs >= kKeepAliveIntervalMs;
    if (speculationInFlight) { d.reason = "speculating"; return d; }
    if (state.lastPrefetchMs >= 0 && c.nowMs - state.lastPrefetchMs < kRowPrefetchInFlightMs) {
        d.reason = "in-flight";
        return d;
    }
    d.reason = "no-row";
    for (std::size_t i = 0; i < ids.size(); ++i) {
        const std::string &id = ids[i];
        if (!catalog::validVideoId(id) || id == sessionVideoId) continue;
        // The first row only; when it was prefetched lately its answer is still cached.
        const bool repeat = id == state.lastVideoId && state.lastPrefetchMs >= 0 &&
                            c.nowMs - state.lastPrefetchMs < kRowPrefetchRepeatMs;
        if (!repeat) { d.prefetchVideoId = id; d.reason = ""; }
        else d.reason = "repeat";
        break;
    }
    return d;
}

// ---- Shorts likelyNext (DSC-17 MAY swipe viewer) -------------------------------------------

LikelyNextDecision likelyNextDecision(const LikelyNextContext &c) {
    LikelyNextDecision d;
    if (!c.appActive) { d.reason = "background"; return d; }
    if (!c.resolverAvailable) { d.reason = "no-resolver"; return d; }
    if (!c.validId) { d.reason = "invalid-id"; return d; }
    if (c.sessionVideo) { d.reason = "playing"; return d; }
    if (c.local) { d.reason = "local"; return d; }
    if (c.blocked) { d.reason = "upcoming"; return d; }
    if (c.alreadyPreloaded) { d.action = LikelyNextDecision::Keep; d.reason = "preloaded"; return d; }
    if (c.pressSpeculation) { d.reason = "pressed"; return d; }
    if (c.starting) { d.action = LikelyNextDecision::Defer; d.reason = "starting"; return d; }
    d.action = LikelyNextDecision::Start;
    return d;
}

std::vector<media::StreamCandidate> lowRungChain(const media::SelectionInput &in, const media::DecoderCapabilities &caps,
                                                 const media::PlaybackPreferences &prefs,
                                                 const media::StreamCandidate &normal) {
    if (normal.kind != media::StreamKind::Adaptive || normal.height <= kLowRungMaxHeight)
        return std::vector<media::StreamCandidate>();
    const media::DecoderPlan plan = media::decoderPlanFor(in, caps, prefs);
    const std::vector<media::QualityOption> options = media::qualityOptions(in, plan.capabilities, plan.preferences);
    for (std::size_t i = 0; i < options.size(); ++i) {  // tallest first
        const media::QualityOption &o = options[i];
        if (o.premium || o.highFrameRate || o.height <= 0 || o.height > kLowRungMaxHeight ||
            o.codec != normal.video.videoCodec)
            continue;
        const std::vector<media::StreamCandidate> low =
            media::selectStreamsWithFallback(in, caps, prefs, media::qualityChoice(o));
        if (low.empty() || low[0].kind != media::StreamKind::Adaptive || low[0].height > kLowRungMaxHeight ||
            low[0].video.videoCodec != normal.video.videoCodec)
            continue;
        return low;
    }
    return std::vector<media::StreamCandidate>();
}

bool firstLoadFor(const std::string &videoId, const media::SelectionInput &in, const media::DecoderCapabilities &caps,
                  const media::PlaybackPreferences &preferences, bool startLowRung, PlaybackLoad *load,
                  bool stableVolume) {
    media::PlaybackPreferences p = preferences;
    p.preferSeekIndex = false;  // a start from 0 (effectivePreferences before the first preroll)
    std::vector<media::StreamCandidate> chain = media::selectStreamsWithFallback(in, caps, p, media::QualityChoice());
    if (chain.empty()) return false;
    // onResolved: the first plain plan of a video may start on the low rung (never listen-only).
    if (startLowRung && p.mode != media::PlaybackMode::AudioOnly) {
        const std::vector<media::StreamCandidate> low = lowRungChain(in, caps, p, chain[0]);
        if (!low.empty()) chain = low;
    }
    AudioChoice audio;
    audio.stableVolume = stableVolume;
    applyAudioChoice(&chain, in, caps, audio);
    if (load) *load = loadForCandidate(chain[0], videoId);
    return true;
}

const int PlaybackController::kLoopUnset;
const int PlaybackController::kLoopOff;
const int PlaybackController::kLoopOn;

PlaybackController::PlaybackController(PlaybackPort *port) : port_(port) {}

void PlaybackController::setRememberedRate(double rate) { rememberedRate_ = player::speed::snap(rate); }

// ---- Contexts (PLY-10) --------------------------------------------------------------------

int PlaybackController::open(std::int64_t nowMs, const std::string &videoId, int loopHint) {
    if (videoId.empty()) return player::kNoContext;
    const int ctx = nextContext_++;
    pendingContext_ = ctx;
    syncQueue(videoId);
    autoplay_.cancelCountdown();
    if (!upNextVideo_.empty()) { upNextVideo_.clear(); changes_ |= UpNextChange; }
    const player::SessionDecision d = arbiter_.open(ctx, videoId);
    applyLoopHint(d.reattach, loopHint);
    if (d.reattach && state_ == Failed) {  // the same failed video again: start over
        retry(nowMs);
        return ctx;
    }
    execute(d, nowMs);
    return ctx;
}

int PlaybackController::openLocal(std::int64_t nowMs, const LocalMedia &media, int loopHint) {
    const std::string id = media.videoId.empty() ? "local:" + media.filePath : media.videoId;
    if (media.filePath.empty() && media.videoPath.empty() && media.audioPath.empty()) return player::kNoContext;
    const int ctx = nextContext_++;
    pendingContext_ = ctx;
    syncQueue(id);
    autoplay_.cancelCountdown();
    upNextVideo_.clear();
    localMedia_ = media;
    localVideoId_ = id;
    pendingLocal_ = true;
    const player::SessionDecision d = arbiter_.open(ctx, id);
    applyLoopHint(d.reattach, loopHint);
    if (d.reattach && state_ == Failed) retry(nowMs);
    else execute(d, nowMs);
    pendingLocal_ = false;
    return ctx;
}

// Shorts: a new video loops only when asked; the session's own video keeps its mode unless asked.
void PlaybackController::applyLoopHint(bool sameVideo, int loopHint) {
    const bool loop = loopHint == kLoopOn || (sameVideo && loopHint == kLoopUnset && loop_);
    if (!sameVideo) loopEndSaved_ = false;
    if (loop == loop_) return;
    loop_ = loop;
    changes_ |= ControlsChange;
}

int PlaybackController::claimContext() {
    if (pendingContext_ != player::kNoContext) {
        const int ctx = pendingContext_;
        pendingContext_ = player::kNoContext;
        return ctx;
    }
    const int ctx = nextContext_++;
    // A page shown without open() (e.g. expanding the parked session) re-attaches.
    if (arbiter_.hasSession()) arbiter_.open(ctx, arbiter_.sessionVideoId());
    changes_ |= ControlsChange;
    return ctx;
}

void PlaybackController::activateContext(std::int64_t nowMs, int contextId) {
    const player::SessionDecision d = arbiter_.activate(contextId);
    if (d.any()) execute(d, nowMs);
}

void PlaybackController::closeContext(std::int64_t nowMs, int contextId) {
    if (pendingContext_ == contextId) pendingContext_ = player::kNoContext;
    const bool active = state_ != Failed && state_ != Idle;
    // A Short is never parked (no mini-player for it): closing its page ends it.
    const bool parkable = active && !shortItem_;
    const player::PlaybackSnapshot snapshot(parkable && wantPlay_, parkable ? capturePosition() : 0);
    const player::SessionDecision d = arbiter_.close(contextId, snapshot);
    execute(d, nowMs);
    if (state_ == Idle) clearShortHolds();
}

// PLY-10 decisions in field order: save -> stop -> park / reattach / resolve.
void PlaybackController::execute(const player::SessionDecision &d, std::int64_t nowMs) {
    if (d.savePosition && !d.stoppedVideoId.empty()) {
        const std::int64_t point = resumePointFor(capturePosition(), durationMs_);
        port_->savePosition(d.stoppedVideoId, point);
        noteResumable(d.stoppedVideoId, point);
    }
    // NFR-PERF-02 tap -> resolve-start: a streamed video's resolve is dispatched before the
    // previous video's player is stopped; the stop follows right after the dispatch.
    const bool toLocal = pendingLocal_ || (!localVideoId_.empty() && d.videoId == localVideoId_);
    const bool resolveFirst = d.stop && d.resolve && !toLocal;
    if (d.stop) {
        if (resolving_) { port_->cancelResolve(); resolving_ = false; }
        retryDeadline_ = -1;
        cancelStreamInstead();
        if (!resolveFirst) releaseMedia();
        if (!d.resolve) {
            applyAudio(audio_.stop());
            autoplay_.cancelCountdown();
            upNextVideo_.clear();
            setState(Idle);
        }
    }
    if (d.park || d.reattach) changes_ |= ControlsChange;
    if (d.resolve) {
        generation_ = d.generation;
        if (toLocal) beginLocal(nowMs);
        else beginOpen(nowMs, d.videoId);
    }
    if (resolveFirst) releaseMedia();  // no-op when the open already failed (RES-19 hold)
}

void PlaybackController::resetVideo(const std::string &videoId) {
    clearShortHolds();  // a hold belongs to the Short it paused
    videoId_ = videoId;
    details_ = catalog::VideoDetails();
    detailsLoaded_ = false;
    fullProfile_ = false;
    setAudioOnly_ = false;
    setMaxHeight_ = 0;
    setExpiresAtUnixMs_ = 0;
    prefetchDeadline_ = -1;
    prefetchPending_ = false;
    prefetchedVideo_.clear();
    local_ = false;
    chain_.clear();
    qualityOptions_.clear();
    playingQualityIndex_ = -1;
    choice_ = media::QualityChoice();
    attempt_ = -1;
    started_ = false;
    everStarted_ = false;
    switching_ = false;
    resumeMs_ = 0;
    positionMs_ = 0;
    durationMs_ = 0;
    seekable_ = false;
    recovery_.reset();
    recoveryResolve_ = false;
    streamResolving_ = false;
    streamNoticeDeadline_ = -1;
    decodeFallbackReason_.clear();
    failure_ = NoFailure;
    failureCode_ = 0;
    retryCode_ = 0;
    retryDeadline_ = -1;
    failureReason_.clear();
    skipper_.clear();
    noticeDeadline_ = -1;
    buttonVisible_ = false;
    highlightVisible_ = false;
    wantPlay_ = true;
    upcomingWaiting_ = false;
    clearLowRung();
    lowRungTried_ = false;
    loadHardwareSetting_ = false;
    failedMaxHeight_ = 0;
    firstFrameSeen_ = false;
    speedHeld_ = false;
    audioChoice_ = AudioChoice();
    stall_.reset();  // a new video: no episode, a full budget
    stallNextLoad_ = false;
    stallReload_ = false;
    stallFailed_ = false;
    if (!segments_.empty()) { segments_.clear(); changes_ |= SegmentsChange; }
    rate_ = player::speed::rateToApply(rememberedRate_, false);
    changes_ |= DetailsChange | StreamChange | StateChange | PlayerChange | ControlsChange | SponsorChange;
}

void PlaybackController::beginOpen(std::int64_t nowMs, const std::string &videoId) {
    resetVideo(videoId);
    if (holdUpcoming(nowMs)) return;  // RES-19
    startResolve(false);
}

// ---- Upcoming videos (RES-19) ------------------------------------------------------------

PlaybackController::UpcomingBlock PlaybackController::upcomingBlock(std::int64_t nowMs, const std::string &videoId) const {
    const std::map<std::string, Upcoming>::const_iterator it = upcoming_.find(videoId);
    if (it == upcoming_.end()) return NotBlocked;
    // A known start: nothing resolves before it; at the start the one armed resolve (or the
    // next open) asks again.
    if (it->second.scheduled) return it->second.startUnixMs - unixOffsetMs_ > nowMs ? AllResolves : NotBlocked;
    // Unknown start, or the scheduled resolve already ran: Retry only.
    return AutomaticOnly;
}

bool PlaybackController::resolveBlocked(std::int64_t nowMs, const std::string &videoId) const {
    return upcomingBlock(nowMs, videoId) != NotBlocked;
}

std::int64_t PlaybackController::scheduledStartUnixMs() const {
    const std::map<std::string, Upcoming>::const_iterator it = upcoming_.find(videoId_);
    return it == upcoming_.end() ? 0 : it->second.startUnixMs;
}

// open() of a video that answered upcoming: show it again without resolving, while the block holds.
bool PlaybackController::holdUpcoming(std::int64_t nowMs) {
    const UpcomingBlock block = upcomingBlock(nowMs, videoId_);
    if (block == NotBlocked || (block == AutomaticOnly && !automaticOpen_)) return false;
    details_.summary.videoId = videoId_;
    showUpcoming();
    return true;
}

void PlaybackController::noteUpcoming(std::int64_t nowMs, std::int64_t startUnixMs) {
    // Forget holds whose scheduled start passed (their next open resolves anyway); stay small.
    for (std::map<std::string, Upcoming>::iterator it = upcoming_.begin(); it != upcoming_.end();) {
        if (it->first != videoId_ && it->second.scheduled && it->second.startUnixMs - unixOffsetMs_ <= nowMs)
            it = upcoming_.erase(it);
        else
            ++it;
    }
    while (upcoming_.size() >= 32 && upcoming_.count(videoId_) == 0) upcoming_.erase(upcoming_.begin());
    Upcoming &u = upcoming_[videoId_];
    u.startUnixMs = startUnixMs > 0 ? startUnixMs : 0;
    // A start that already passed while the video is still upcoming counts as unknown: no loop.
    u.scheduled = u.startUnixMs > 0 && u.startUnixMs - unixOffsetMs_ > nowMs;
}

// Failed with UpcomingStream; the one deadline is armed at a known future start.
void PlaybackController::showUpcoming() {
    const std::map<std::string, Upcoming>::const_iterator it = upcoming_.find(videoId_);
    details_.summary.liveStatus = media::LiveStatus::Upcoming;
    changes_ |= DetailsChange;
    fail(UpcomingStream, failureCode_, std::string());
    upcomingWaiting_ = it != upcoming_.end() && it->second.scheduled;
}

void PlaybackController::beginLocal(std::int64_t nowMs) {
    resetVideo(localVideoId_);
    local_ = true;
    details_.summary.videoId = localMedia_.videoId;
    details_.summary.title = localMedia_.title;
    details_.summary.durationSeconds = localMedia_.durationMs > 0 ? localMedia_.durationMs / 1000 : -1;
    details_.summary.liveStatus = media::LiveStatus::NotLive;
    detailsLoaded_ = true;
    durationMs_ = localMedia_.durationMs;
    // LNK-02: the link start of the open that found this download, applied once.
    if (localMedia_.startPositionMs >= 0) {
        resumeMs_ = localMedia_.startPositionMs;
        positionMs_ = resumeMs_;
        localMedia_.startPositionMs = -1;
    }
    startAttempt(nowMs, 0);
}

// A fresh generation for a re-resolve of the current video (recovery, retry,
// full-profile fallback), re-activating the session when it ended.
bool PlaybackController::renewGeneration() {
    player::SessionDecision d = arbiter_.reresolve(arbiter_.owner());
    if (!d.resolve) {
        int ctx = arbiter_.owner() > 0 ? arbiter_.owner() : arbiter_.topContext();
        if (ctx <= 0) { ctx = nextContext_++; pendingContext_ = ctx; }
        d = arbiter_.open(ctx, videoId_);
        if (d.reattach) d = arbiter_.reresolve(ctx);
        if (!d.resolve) return false;
    }
    generation_ = d.generation;
    return true;
}

void PlaybackController::startResolve(bool fresh) {
    if (resolving_ || streamResolving_) port_->cancelResolve();
    streamResolving_ = false;  // this resolve replaces a stream-instead one
    ResolveRequest r = resolveRequestFor(videoId_, capabilities_, effectivePreferences(), choice_);
    r.generation = generation_;
    r.fresh = fresh;
    r.fault = fresh ? resolveFault_ : player::SourceFault::Unspecified;
    resolveFault_ = player::SourceFault::Unspecified;
    resolving_ = true;
    resolvingFull_ = fresh;
    resolvingAudioOnly_ = r.audioOnly;
    resolvingMaxHeight_ = r.maxHeight;
    setState(Loading, recoveryResolve_ ? Reconnecting : fresh ? ResolvingFull : Resolving);
    port_->resolve(r);
}

void PlaybackController::onResolved(std::int64_t nowMs, std::int64_t nowUnixMs, const std::string &videoId,
                                    unsigned generation, const ResolveAnswer &answer) {
    // PLY-10: answers for another id or an older generation are stale.
    if ((!resolving_ && !streamResolving_) || videoId != videoId_ || generation != generation_ ||
        !arbiter_.acceptResolve(videoId, generation))
        return;
    if (streamResolving_) {  // PLY-26 stream instead: the local file is still the item
        unixOffsetMs_ = nowUnixMs - nowMs;
        onStreamInsteadResolved(nowMs, answer);
        return;
    }
    resolving_ = false;
    const bool full = resolvingFull_;
    retryCode_ = 0;
    failedMaxHeight_ = resolvingMaxHeight_;  // errorSettingFacts: the ceiling this answer was for
    unixOffsetMs_ = nowUnixMs - nowMs;
    // RES-19: an upcoming video shows its scheduled start and is not resolved again until then.
    if (answer.upcoming || (answer.error == ResolveAnswer::None && answer.details.summary.liveStatus == media::LiveStatus::Upcoming)) {
        recoveryResolve_ = false;
        if (answer.error == ResolveAnswer::None) {
            details_ = answer.details;
            detailsLoaded_ = true;
        }
        failureCode_ = answer.errorCode;
        noteUpcoming(nowMs, answer.scheduledStartUnixMs);
        showUpcoming();
        return;
    }
    upcoming_.erase(videoId_);
    if (answer.error != ResolveAnswer::None) {
        if (recoveryResolve_) {  // a failed recovery is another fault (PLY-18)
            fault(nowMs, "resolve failed", true);
            return;
        }
        if (!full && answer.error == ResolveAnswer::Retryable) {
            // RES-09: one fresh re-resolve; a rate-limited answer waits kRetryLaterMs first.
            retryCode_ = answer.errorCode;
            if (answer.retryLater) {
                retryDeadline_ = nowMs + kRetryLaterMs;
                setState(Loading, RetryingLater);
                return;
            }
            startResolve(true);
            return;
        }
        fail(ResolveFailed, answer.errorCode, std::string());
        return;
    }
    details_ = answer.details;
    detailsLoaded_ = true;
    fullProfile_ = full;
    setAudioOnly_ = resolvingAudioOnly_;
    setMaxHeight_ = resolvingMaxHeight_;
    setExpiresAtUnixMs_ = answer.expiresAtUnixMs;
    if (details_.summary.durationSeconds > 0) durationMs_ = details_.summary.durationSeconds * 1000;
    changes_ |= DetailsChange;
    rebuildChain();
    if (chain_.empty()) {
        if (!full) { startResolve(true); return; }
        if (recoveryResolve_) { fault(nowMs, "no playable stream", false); return; }
        fail(NoPlayableStream, 0, std::string());
        return;
    }
    // NFR-PERF-02 A/B: the first plain plan of a video may start on the low rung.
    if (startLowRung_ && !lowRungTried_ && !full && !recoveryResolve_ && !everStarted_ && !switching_ &&
        choice_.height == 0 && !audioOnly_) {
        lowRungTried_ = true;
        applyLowRung();
    }
    recoveryResolve_ = false;
    startAttempt(nowMs, 0);
}

// ---- Stream instead (PLY-26, SEL-07) ------------------------------------------------------------

bool PlaybackController::canStreamInstead() const {
    return local_ && !localMedia_.videoId.empty() && catalog::validVideoId(videoId_) && state_ != Idle &&
           !streamResolving_;
}

bool PlaybackController::streamInstead(std::int64_t /*nowMs: time-injected API; the answer carries the time*/) {
    if (!canStreamInstead()) return false;
    if (resolving_) { port_->cancelResolve(); resolving_ = false; }  // not expected for a local item
    if (!renewGeneration()) return false;  // the owner, the context stack and the session stay
    // The request open() would make: Auto quality, the session's listen-only mode (RES-10).
    ResolveRequest r = resolveRequestFor(videoId_, capabilities_, effectivePreferences(), media::QualityChoice());
    r.generation = generation_;
    streamResolving_ = true;
    resolvingAudioOnly_ = r.audioOnly;
    resolvingMaxHeight_ = r.maxHeight;
    if (streamNoticeDeadline_ >= 0) { streamNoticeDeadline_ = -1; changes_ |= StateChange; }
    changes_ |= ControlsChange | StreamChange;  // canStreamInstead
    port_->resolve(r);  // no state change: the local file plays on meanwhile
    return true;
}

void PlaybackController::cancelStreamInstead() {
    if (!streamResolving_) return;
    streamResolving_ = false;
    if (!resolving_) port_->cancelResolve();
    changes_ |= ControlsChange | StreamChange;
}

void PlaybackController::onStreamInsteadResolved(std::int64_t nowMs, const ResolveAnswer &answer) {
    streamResolving_ = false;
    changes_ |= ControlsChange | StreamChange;
    const bool upcomingAnswer = answer.upcoming || (answer.error == ResolveAnswer::None &&
                                                    answer.details.summary.liveStatus == media::LiveStatus::Upcoming);
    std::vector<media::StreamCandidate> chain;
    if (answer.error == ResolveAnswer::None && !upcomingAnswer) {
        media::SelectionInput in;
        in.formats = answer.details.formats;
        in.liveStatus = answer.details.summary.liveStatus;
        in.manifestUrl = answer.details.manifestUrl;
        chain = media::selectStreamsWithFallback(in, capabilities_, effectivePreferences(), media::QualityChoice());
        applyAudioChoice(&chain, in, capabilities_, chainAudioChoice());
    }
    if (chain.empty()) {  // stay on the file, keep playing; a transient notice (no setting fixes it)
        streamNoticeDeadline_ = nowMs + kStreamNoticeMs;
        changes_ |= StateChange;
        return;
    }
    // SEL-07 CapturePosition: the file played on during the resolve.
    resumeMs_ = started_ ? positionMs_ : std::max(resumeMs_, positionMs_);
    // The display summary stays (no flicker); the set brings formats, live state and manifest.
    catalog::VideoSummary summary = details_.summary;
    const catalog::VideoSummary &more = answer.details.summary;
    if (summary.title.empty()) summary.title = more.title;
    if (summary.channel.empty()) summary.channel = more.channel;
    if (summary.channelId.empty()) summary.channelId = more.channelId;
    if (summary.thumbnailUrl.empty()) summary.thumbnailUrl = more.thumbnailUrl;
    if (summary.viewCount < 0) summary.viewCount = more.viewCount;
    if (summary.durationSeconds <= 0) summary.durationSeconds = more.durationSeconds;
    summary.videoId = videoId_;
    summary.liveStatus = more.liveStatus;
    details_.formats = answer.details.formats;
    details_.manifestUrl = answer.details.manifestUrl;
    details_.summary = summary;
    detailsLoaded_ = true;
    if (durationMs_ <= 0 && summary.durationSeconds > 0) durationMs_ = summary.durationSeconds * 1000;
    // From here on a stream item (a later open() / play() after Stop resolves, never the file).
    local_ = false;
    localVideoId_.clear();
    fullProfile_ = false;
    setAudioOnly_ = resolvingAudioOnly_;
    setMaxHeight_ = resolvingMaxHeight_;
    setExpiresAtUnixMs_ = answer.expiresAtUnixMs;
    choice_ = media::QualityChoice();
    recovery_.reset();  // the file's recoveries do not count against the stream
    recoveryResolve_ = false;
    clearLowRung();
    lowRungTried_ = true;  // NFR-PERF-02 A/B: never on a switch
    chain_ = chain;
    attempt_ = -1;
    switching_ = true;
    changes_ |= DetailsChange | StreamChange | StreamSwitchChange;
    startAttempt(nowMs, 0);
}

// ---- Low-rung start A/B (NFR-PERF-02, diagnostics/startLowRung) -------------------------------

// The normal pick is chain_[0]. A dual plan taller than kLowRungMaxHeight is replaced by the chain
// of the tallest non-premium, <= 30 fps dual rung <= kLowRungMaxHeight of the same video codec
// family; anything else (muxed, live, listen-only, already low) keeps the normal chain.
bool PlaybackController::applyLowRung() {
    if (chain_.empty()) return false;
    std::vector<media::StreamCandidate> low = lowRungChain(selectionInput(), capabilities_, effectivePreferences(), chain_[0]);
    if (low.empty()) return false;
    applyAudioChoice(&low, selectionInput(), capabilities_, chainAudioChoice());
    chain_ = low;
    attempt_ = -1;
    changes_ |= StreamChange;
    lowRungPending_ = true;
    return true;
}

void PlaybackController::clearLowRung() {
    lowRungPending_ = false;
    lowRungFrame_ = false;
    lowRungDeadline_ = -1;
}

// One same-set switch (SEL-07) from the low rung to the normal pick, at the captured position.
void PlaybackController::switchUpFromLowRung(std::int64_t nowMs) {
    clearLowRung();
    if (!loaded_ || !started_ || local_ || choice_.height != 0 || audioOnly_) return;
    resumeMs_ = capturePosition();
    rebuildChain();
    if (chain_.empty()) { fail(NoPlayableStream, 0, std::string()); return; }
    switching_ = true;
    changes_ |= LowRungSwitchChange;
    startAttempt(nowMs, 0);
}

// ---- Selection and attempts ------------------------------------------------------------

media::SelectionInput PlaybackController::selectionInput() const {
    media::SelectionInput in;
    in.formats = details_.formats;
    in.liveStatus = details_.summary.liveStatus;
    in.manifestUrl = details_.manifestUrl;
    return in;
}

media::PlaybackPreferences PlaybackController::effectivePreferences() const {
    media::PlaybackPreferences p = preferences_;
    p.mode = audioOnly_ ? media::PlaybackMode::AudioOnly : media::PlaybackMode::Video;
    // PLY-06: a deep start (resume / link far into the video) prefers WebM, whose Cues index makes
    // the start seek one range request; an index-less fragmented MP4 is walked moof by moof.
    p.preferSeekIndex = !started_ && resumeMs_ >= kDeepStartMs;
    if (dubRemembered_) p.audioLanguage = rememberedDub_;  // SEL-05: this session's pick
    return p;
}

void PlaybackController::rebuildChain() {
    // SEL-02/03/08 under the PLY-21 interim decoder rule.
    chain_ = media::selectStreamsWithFallback(selectionInput(), capabilities_, effectivePreferences(), choice_);
    applyAudioChoice(&chain_, selectionInput(), capabilities_, chainAudioChoice());  // dub picker, stable volume
    attempt_ = -1;
    changes_ |= StreamChange;
}

void PlaybackController::startAttempt(std::int64_t nowMs, int index) {
    releaseMedia();
    if (!local_ && (index < 0 || index >= static_cast<int>(chain_.size()))) return;
    attempt_ = index;
    started_ = false;
    changes_ |= StreamChange;
    setState(Preparing, stallNextLoad_ ? Reconnecting : Starting);
    if (local_) { loadNow(nowMs); return; }
    const media::StreamCandidate &c = chain_[static_cast<std::size_t>(index)];
    // SEL-03: the menu for this play plan, playing entry by format id.
    const media::SelectionInput in = selectionInput();
    const media::DecoderPlan plan = media::decoderPlanFor(in, capabilities_, effectivePreferences());
    const media::QualityMenu menu = media::qualityMenu(in, plan.capabilities, plan.preferences, c);
    qualityOptions_ = menu.entries;
    playingQualityIndex_ = menu.playingIndex;
    // Provider pre-playback wait (capped).
    std::int64_t available = 0;
    const media::MediaFormat *formats[] = { &c.video, &c.audio, &c.muxed };
    for (std::size_t i = 0; i < 3; ++i)
        if (!formats[i]->url.empty()) available = std::max(available, formats[i]->availableAtUnixSeconds * 1000);
    const std::int64_t wait = available > 0 ? available - (nowMs + unixOffsetMs_) : 0;
    if (wait > 0) {
        loadDeadline_ = nowMs + std::min<std::int64_t>(wait, kMaximumAvailableWaitMs);
        return;
    }
    loadNow(nowMs);
}

PlaybackLoad PlaybackController::localLoad() const {
    PlaybackLoad l;
    l.videoId = videoId_;
    const LocalMedia &m = localMedia_;
    const std::string video = !m.videoPath.empty() ? m.videoPath : std::string();
    if (!audioOnly_ && m.layout == "pair" && !video.empty() && !m.audioPath.empty()) {
        l.mode = player::PlayMode::Dual;
        l.video = localSpec(video);
        l.audio = localSpec(m.audioPath);
    } else if (!audioOnly_ && !video.empty()) {
        l.mode = player::PlayMode::Local;
        l.muxed = localSpec(video);
    } else {
        l.mode = player::PlayMode::AudioOnly;
        if (!m.audioPath.empty()) l.audio = localSpec(m.audioPath);
        else l.muxed = localSpec(!m.filePath.empty() ? m.filePath : video);
    }
    return l;
}

void PlaybackController::loadNow(std::int64_t nowMs) {
    loadDeadline_ = -1;
    PlaybackLoad l = local_ ? localLoad() : loadForCandidate(chain_[static_cast<std::size_t>(attempt_)], videoId_);
    loadHardwareSetting_ = !local_ && media::hardwareDecodeActive(capabilities_, effectivePreferences());
    if (loadHardwareSetting_) l.decode = player::DecodeMode::Hardware;  // PLY-20: setting on and verified
    l.startPositionMs = l.liveHls ? 0 : resumeMs_;
    // PLY-07: the remembered speed rides on the preroll barrier (before the first frame).
    l.rate = speedHeld_ && !l.liveHls ? kHoldSpeedRate : player::speed::rateToApply(rememberedRate_, l.liveHls);
    rate_ = l.rate;
    l.metadataDurationMs = durationMs_;
    // Stall recovery: new stream jobs; the watchdog times this reload itself (its next look).
    stallReload_ = stallNextLoad_;
    stallNextLoad_ = false;
    l.freshStreams = stallReload_;
    stallDirty_ = true;
    positionMs_ = l.startPositionMs;
    loaded_ = true;
    started_ = false;
    firstFrameSeen_ = false;
    watchdogDeadline_ = nowMs + kStartWatchdogMs;
    port_->load(l);
    if (wantPlay_) requestPlayback();
    changes_ |= PlayerChange | ControlsChange;
}

void PlaybackController::releaseMedia() {
    if (loaded_) port_->stopPlayer();  // keeps the picture while keepsPicture()
    stallReload_ = false;
    stall_.suspend();  // nothing to watch until the next load prerolls (an open episode stays open)
    loaded_ = false;
    started_ = false;
    firstFrameSeen_ = false;
    loadDeadline_ = -1;
    watchdogDeadline_ = -1;
    if (playerPlaying_ || seekable_) changes_ |= PlayerChange;
    playerPlaying_ = false;
    seekable_ = false;
}

// PLY-23: the resource first; PLAYING only once it is granted.
void PlaybackController::requestPlayback() { applyAudio(audio_.requestPlay()); }

void PlaybackController::applyAudio(const player::AudioResourceAction &a) {
    if (a.acquire) port_->acquireAudio();
    if (a.pause) port_->pause();
    if (a.play) {
        wantPlay_ = true;
        if (loaded_) port_->play();
    }
    if (a.release) port_->releaseAudio();
    if (a.any()) changes_ |= PlayerChange;
}

// ---- Failures and recovery (PLY-18) ------------------------------------------------------

void PlaybackController::fault(std::int64_t nowMs, const std::string &reason, bool network) {
    if (!owner()) return;  // PLY-10: nobody owns the player, no side effects
    if (lowRungPending_) {
        // NFR-PERF-02 A/B: the low rung failed before its switch; the normal pick takes over.
        const bool before = !started_;
        clearLowRung();
        if (before && !local_ && !resolving_) {
            rebuildChain();
            if (!chain_.empty()) { startAttempt(nowMs, 0); return; }
        }
    }
    lastFaultNetwork_ = network;
    lastFaultStartTimeout_ = startTimedOut_;
    failureReason_ = reason;
    watchdogDeadline_ = -1;
    // Stall watchdog: the reopened candidate failed before its preroll. The other candidates of
    // the same stale set would meet the same end: the fresh resolve is next.
    if (stallReload_ && !started_ && !local_ && !resolving_ && stall_.step() == player::StallStep::Reopen) {
        applyStall(nowMs, stall_.onStepFailed(nowMs));
        return;
    }
    if (!started_ && !local_ && recovery_.state() != player::RecoveryState::Failed) {
        // The attempt chain: the next candidate, then a full re-extraction.
        if (attempt_ + 1 < static_cast<int>(chain_.size()) && !resolving_) {
            startAttempt(nowMs, attempt_ + 1);
            return;
        }
        if (!fullProfile_ && !resolving_) {
            releaseMedia();
            if (renewGeneration()) {
                recoveryResolve_ = recovery_.state() == player::RecoveryState::Recovering;
                resolveFault_ = lastFaultKind();
                startResolve(true);
                return;
            }
        }
    }
    const std::int64_t position = started_ ? positionMs_ : resumeMs_;
    releaseMedia();
    applyRecovery(nowMs, recovery_.onFault(nowMs, position, live(), reason));
}

void PlaybackController::applyRecovery(std::int64_t nowMs, const player::RecoveryAction &a) {
    switch (a.kind) {
    case player::RecoveryAction::Wait:
        setState(Preparing, Reconnecting);
        break;
    case player::RecoveryAction::Recover:
        recoverNow(nowMs, a);
        break;
    case player::RecoveryAction::Fail:
        fail(lastFaultNetwork_ ? NetworkFailed : everStarted_ ? PlaybackFailed : NoPlayableStream, 0,
             recovery_.reason());
        break;
    case player::RecoveryAction::None:
        if (recovery_.state() == player::RecoveryState::Deferred) setState(Preparing, WaitingForForeground);
        break;
    }
}

void PlaybackController::recoverNow(std::int64_t nowMs, const player::RecoveryAction &a) {
    // The page may have lost the player during the backoff (PLY-10).
    if (!arbiter_.mayAct(arbiter_.owner())) return;
    resumeMs_ = a.resumeAtLive ? 0 : a.resumePositionMs;
    clearLowRung();
    releaseMedia();
    if (local_) { startAttempt(nowMs, 0); return; }
    if (!renewGeneration()) return;
    recoveryResolve_ = true;
    resolveFault_ = lastFaultKind();
    startResolve(fullProfile_);
}

// RES-07: what the last player fault says about the source. A network-class fault is a lost
// connection, the start watchdog an opening that ran out of time: neither is a strike.
player::SourceFault PlaybackController::lastFaultKind() const {
    return lastFaultStartTimeout_ ? player::SourceFault::Abandoned
         : lastFaultNetwork_ ? player::SourceFault::Connection : player::SourceFault::Unspecified;
}

void PlaybackController::fail(Failure failure, int code, const std::string &reason) {
    stallNextLoad_ = false;  // a failure shows no kept picture
    stallReload_ = false;
    stallFailed_ = false;    // the watchdog sets it after its own failure
    releaseMedia();
    if (resolving_) { port_->cancelResolve(); resolving_ = false; }
    recoveryResolve_ = false;
    setState(Failed);
    failure_ = failure;
    failureCode_ = code;
    if (!reason.empty()) failureReason_ = reason;
    switching_ = false;
}

void PlaybackController::retry(std::int64_t nowMs) {
    if (videoId_.empty()) return;
    // RES-19: before a known scheduled start nothing resolves; with an unknown start only the
    // user's Retry does (not autoplay).
    if (upcoming()) {
        const UpcomingBlock block = upcomingBlock(nowMs, videoId_);
        if (block == AllResolves || (block == AutomaticOnly && automaticOpen_)) { changes_ |= StateChange; return; }
    }
    cancelStreamInstead();  // Retry restarts the item as it is
    stall_.onUserSeek();    // the user asked again: a full stall budget
    retryCode_ = 0;
    retryDeadline_ = -1;
    const player::RecoveryAction a = recovery_.retry(nowMs);  // PLY-18: fresh budget
    resumeMs_ = std::max(resumeMs_, capturePosition());
    clearLowRung();
    failure_ = NoFailure;
    wantPlay_ = true;
    releaseMedia();
    if (local_) {
        if (renewGeneration()) startAttempt(nowMs, 0);
        return;
    }
    if (!renewGeneration()) return;
    recoveryResolve_ = a.kind == player::RecoveryAction::Recover;
    // Signed URLs may have expired: extract again with the full profile (core).
    startResolve(detailsLoaded_);
}

// ---- Stall watchdog (StallPolicy) ---------------------------------------------------------

// One look with fresh facts: at the policy's deadline, and after any event that may have changed
// a guard. What the controller knows comes first; a port without a player keeps the watchdog off.
void PlaybackController::watchStall(std::int64_t nowMs) {
    stallDirty_ = false;
    PlayerProgress p;
    const bool watchable = owner() && loaded_ && wantPlay_ && state_ != Idle && state_ != Failed &&
                           audio_.state() == player::AudioResourcePolicy::State::Granted;
    if (!watchable || !port_->progress(&p)) {
        stall_.suspend();
        return;
    }
    stallPort_ = true;
    player::StallFacts f;
    f.wantsPlay = true;
    f.started = started_;                       // false through a start, a switch and a reload
    f.reloading = stallReload_ && !started_;    // a recovery step's reload on its way to the preroll
    f.seeking = p.seeking;
    f.ended = p.ended;
    f.liveEdgeWaiting = liveEdge() && p.buffering;
    f.videoFrozen = p.videoFrozen;
    f.seekable = seekable_ && !liveEdge();
    f.resolvable = !local_;
    f.positionMs = p.positionMs;
    f.buffering = p.buffering;
    f.bufferingPercent = p.bufferingPercent;
    f.bufferedMs = p.bufferedMs;
    stallAtMs_ = started_ ? std::max(positionMs_, p.positionMs) : resumeMs_;
    applyStall(nowMs, stall_.observe(nowMs, f));
}

void PlaybackController::noteStall(StallEvent::Kind kind, int step, const char *reason, std::int64_t ms) {
    StallEvent e;
    e.kind = kind;
    e.step = step;
    e.reason = reason;
    e.ms = ms;
    e.positionMs = stallAtMs_;
    stallEvents_.push_back(e);
}

std::vector<PlaybackController::StallEvent> PlaybackController::takeStallEvents() {
    std::vector<StallEvent> events;
    events.swap(stallEvents_);
    return events;
}

// Never reportRefused from here: a stall or a connection loss is not a strike (RES-07). A byte-0
// HTTP refusal of a reload stays one, through the player's own error as before.
void PlaybackController::applyStall(std::int64_t nowMs, const player::StallDecision &d) {
    if (d.kind == player::StallDecision::None) return;
    // Markers, diagnostics (stream), the busy indicator (player) and `recovering` (state).
    changes_ |= StallChange | StreamChange | PlayerChange | StateChange;
    if (d.detected) noteStall(StallEvent::Detected, 0, d.reason, d.ms);
    if (d.kind == player::StallDecision::Recovered) {
        noteStall(StallEvent::Recovered, static_cast<int>(d.step), "", d.ms);
        return;
    }
    if (d.kind == player::StallDecision::Fail) {
        noteStall(StallEvent::Failed, static_cast<int>(d.step), d.failure, 0);
        resumeMs_ = stallAtMs_;  // Retry resumes there
        if (!appActive_) {
            // PLY-18: nothing fails in the background. The fault is deferred there (no budget
            // spent) and recovered when the app is shown again, with a full stall budget too.
            lastFaultNetwork_ = false;
            failureReason_ = "playback stopped responding";
            stallNextLoad_ = false;
            stallReload_ = false;
            const player::RecoveryAction a = recovery_.onFault(nowMs, stallAtMs_, live(), failureReason_);
            if (recovery_.state() == player::RecoveryState::Deferred) {
                releaseMedia();
                stall_.onUserSeek();
                applyRecovery(nowMs, a);
                return;
            }
        }
        fail(PlaybackFailed, 0, "playback stopped responding");
        stallFailed_ = true;
        return;
    }
    noteStall(StallEvent::Step, static_cast<int>(d.step), "", 0);
    stallDirty_ = true;  // look again right after the step
    switch (d.step) {
    case player::StallStep::Seek:
        // Cheap: a flush at the same position frees a stuck sink and re-requests a lost segment;
        // PLAYING is asked again in case the pipeline and the session disagree about it.
        port_->seek(stallAtMs_);
        port_->play();
        break;
    case player::StallStep::Reopen:
        // The same candidate through new stream jobs, at the position, the picture kept.
        resumeMs_ = stallAtMs_;
        clearLowRung();
        stallNextLoad_ = true;
        startAttempt(nowMs, local_ || attempt_ < 0 ? 0 : attempt_);
        break;
    case player::StallStep::Reresolve:
        // One fresh resolve (invalidate first), as a PLY-18 recovery resolve: a failed answer is a
        // fault with its backoff, and waits for the foreground in the background.
        resumeMs_ = stallAtMs_;
        clearLowRung();
        stallNextLoad_ = true;
        releaseMedia();
        if (!renewGeneration()) {  // nobody owns the session any more
            fail(PlaybackFailed, 0, "playback stopped responding");
            stallFailed_ = true;
            return;
        }
        recoveryResolve_ = true;
        resolveFault_ = player::SourceFault::Stalled;  // RES-07: a stall is not a strike
        startResolve(true);
        break;
    case player::StallStep::None:
        break;
    }
}

// ---- User actions ------------------------------------------------------------------------

void PlaybackController::play(std::int64_t nowMs) {
    if (state_ == Failed) { retry(nowMs); return; }
    if (heldByShort_) resumeAfterHold_ = false;  // the user (or the lock screen) chose: nothing to resume later
    wantPlay_ = true;
    stallDirty_ = true;
    changes_ |= PlayerChange;
    if (state_ == Idle) {  // after Stop: start the same video again
        if (videoId_.empty()) return;
        if (local_) { if (renewGeneration()) startAttempt(nowMs, 0); return; }
        if (!renewGeneration()) return;
        resetVideo(videoId_);
        startResolve(false);
        return;
    }
    // A pending resolve, provider wait or recovery starts playback itself.
    if (resolving_ || loadDeadline_ >= 0 || retryDeadline_ >= 0 || recovery_.state() == player::RecoveryState::Waiting ||
        recovery_.state() == player::RecoveryState::Deferred)
        return;
    if (!loaded_) {
        if (local_ || !chain_.empty()) startAttempt(nowMs, attempt_ < 0 ? 0 : attempt_);
        return;
    }
    requestPlayback();
}

void PlaybackController::pause() {
    if (heldByShort_) resumeAfterHold_ = false;  // a pause by the user wins over the release of a hold
    pauseNow();
}

void PlaybackController::pauseNow() {
    wantPlay_ = false;
    stall_.suspend();  // a paused position is not a stall
    applyAudio(audio_.requestPause());
    changes_ |= PlayerChange;
}

// ---- Shorts: holds and the position rule --------------------------------------------------------

void PlaybackController::setShortItem(bool on) {
    if (on == shortItem_) return;
    shortItem_ = on;
    changes_ |= ControlsChange | StateChange;
    if (!on) clearShortHolds();
    else if (!appActive_) holdShort("app");  // opened while the app is in the background
}

void PlaybackController::holdShort(const std::string &reason) {
    if (!shortItem_ || state_ == Idle || reason.empty()) return;
    const bool first = shortHolds_.empty();
    shortHolds_.insert(reason);
    if (!first) return;
    heldByShort_ = true;
    resumeAfterHold_ = wantPlay_;
    if (wantPlay_) pauseNow();
    changes_ |= ControlsChange | PlayerChange;
}

void PlaybackController::releaseShort(std::int64_t nowMs, const std::string &reason) {
    if (shortHolds_.erase(reason) == 0 || !shortHolds_.empty()) return;
    const bool resume = heldByShort_ && resumeAfterHold_;
    heldByShort_ = false;
    resumeAfterHold_ = false;
    changes_ |= ControlsChange | PlayerChange;
    if (resume && state_ != Idle && state_ != Failed) play(nowMs);
}

void PlaybackController::clearShortHolds() {
    shortHolds_.clear();
    heldByShort_ = false;
    resumeAfterHold_ = false;
}

// LIB-05 for a Short (no resume point: the history entry stays, the position is "none").
std::int64_t PlaybackController::resumePointFor(std::int64_t positionMs, std::int64_t durationMs) const {
    return shortItem_ ? player::kNoResume : player::resumePointToStore(positionMs, durationMs);
}

void PlaybackController::stop() {
    // MPR-01 StopActions in order: save, end session, release stream jobs, release audio.
    const player::StopActions s = player::mprisStopActions(arbiter_.hasSession());
    const player::SessionDecision d = arbiter_.stopSession();
    if (s.savePosition && d.savePosition && !d.stoppedVideoId.empty()) {
        const std::int64_t point = resumePointFor(capturePosition(), durationMs_);
        port_->savePosition(d.stoppedVideoId, point);
        noteResumable(d.stoppedVideoId, point);
    }
    clearShortHolds();
    if (resolving_) { port_->cancelResolve(); resolving_ = false; }
    retryDeadline_ = -1;
    cancelStreamInstead();
    recoveryResolve_ = false;
    clearLowRung();
    stallNextLoad_ = false;  // Stop keeps no picture
    stallReload_ = false;
    releaseMedia();
    applyAudio(audio_.stop());
    autoplay_.cancelCountdown();
    upNextVideo_.clear();
    wantPlay_ = false;
    recovery_.reset();
    stall_.onUserSeek();  // an open episode ends with the session
    if (sleepTimerActive()) { sleepDeadline_ = -1; sleepAtEnd_ = false; changes_ |= SleepTimerChange; }
    setState(Idle);
    changes_ |= ControlsChange | UpNextChange;
}

// A user seek: the stall budget is full again and an open episode is dropped.
void PlaybackController::seek(std::int64_t positionMs) {
    stall_.onUserSeek();
    seekTo(positionMs);
}

void PlaybackController::seekTo(std::int64_t positionMs) {
    stallDirty_ = true;
    if (positionMs < 0) positionMs = 0;
    positionMs_ = positionMs;
    if (lowRungFrame_) lowRungFromMs_ = positionMs;  // the low rung's 2 s count from the seek
    if (!loaded_) { resumeMs_ = positionMs; return; }
    if (!started_) resumeMs_ = positionMs;
    port_->seek(positionMs);
    changes_ |= PlayerChange;
}

bool PlaybackController::liveEdge() const {
    const media::StreamCandidate *c = candidate();
    return c && c->kind == media::StreamKind::Live;
}

void PlaybackController::setRate(double rate) {
    const bool edge = liveEdge();
    rememberedRate_ = player::speed::valueToRemember(rememberedRate_, rate, edge);
    port_->rememberRate(rememberedRate_);
    // Hold-to-speed: the new rate is remembered now and applies at the release.
    const double applied = speedHeld_ && !edge ? kHoldSpeedRate : player::speed::rateToApply(rememberedRate_, edge);
    if (applied != rate_) {
        rate_ = applied;
        if (loaded_) port_->setRate(applied);
    }
    changes_ |= PlayerChange;
}

bool PlaybackController::rateChangeable() const { return player::speed::speedChangeable(liveEdge()); }

std::int64_t PlaybackController::capturePosition() const { return started_ ? positionMs_ : resumeMs_; }

// SEL-07 / SEL-08 / RES-10: whether the set in details_ can serve the current mode and choice,
// so a switch re-selects from it instead of resolving again.
bool PlaybackController::setServes(std::int64_t nowMs) const {
    // URLs that expire within the margin are resolved again (fresh).
    if (setExpiresAtUnixMs_ > 0 && nowMs + unixOffsetMs_ + kExpiryMarginMs >= setExpiresAtUnixMs_) return false;
    if (audioOnly_) return true;  // every set carries audio
    if (setAudioOnly_) return false;  // a listen-only set has no video rungs
    if (choice_.height <= 0) return true;
    // An explicit rung above the set's ceiling that the set does not offer: ask once with it.
    if (setMaxHeight_ == 0 || choice_.height <= setMaxHeight_) return true;
    for (std::size_t i = 0; i < details_.formats.size(); ++i)
        if (details_.formats[i].hasVideo() && details_.formats[i].height == choice_.height) return true;
    return false;
}

void PlaybackController::setQuality(std::int64_t nowMs, const media::QualityChoice &choice) {
    if (!detailsLoaded_ || local_) return;
    if (upcoming()) { choice_ = choice; return; }  // RES-19: applies to the start
    clearLowRung();  // the user's choice replaces the A/B switch
    choice_ = choice;
    resumeMs_ = capturePosition();
    switching_ = true;
    if (!setServes(nowMs)) {
        const bool expired = setExpiresAtUnixMs_ > 0 && nowMs + unixOffsetMs_ + kExpiryMarginMs >= setExpiresAtUnixMs_;
        releaseMedia();
        if (renewGeneration()) startResolve(expired);
        return;
    }
    rebuildChain();
    if (chain_.empty()) { fail(NoPlayableStream, 0, std::string()); return; }
    startAttempt(nowMs, 0);
}

void PlaybackController::setAudioOnly(std::int64_t nowMs, bool audioOnly) {
    if (audioOnly == audioOnly_) return;
    audioOnly_ = audioOnly;
    changes_ |= StreamChange | ControlsChange;
    if (!detailsLoaded_ || state_ == Idle || state_ == Failed) return;  // RES-19: upcoming is Failed
    if (local_) {
        resumeMs_ = capturePosition();
        switching_ = true;
        const bool streamPending = streamResolving_;
        startAttempt(nowMs, 0);
        if (streamPending) {  // stream instead: ask again for the new purpose (SEL-08)
            cancelStreamInstead();
            streamInstead(nowMs);
        }
        return;
    }
    setQuality(nowMs, choice_);
}

void PlaybackController::setStartPosition(std::int64_t positionMs) {
    if (started_ || videoId_.empty()) return;
    resumeMs_ = positionMs < 0 ? 0 : positionMs;
    positionMs_ = resumeMs_;
}

// ---- Queue and autoplay (PLY-25, MPR-01) ---------------------------------------------------

void PlaybackController::setRelatedCandidate(const std::string &videoId) {
    relatedCandidate_ = videoId;
    autoplay_.setRelatedCandidate(videoId);
    maybePrefetch();
}

// RES-11: the queue's next item, else the related candidate when it would autoplay.
std::string PlaybackController::prefetchCandidate() const {
    const std::vector<std::string> &q = autoplay_.queue();
    const int i = autoplay_.currentIndex();
    if (autoplay_.hasQueue()) {
        if (autoplay_.canGoNext() && i >= 0 && i + 1 < static_cast<int>(q.size())) return q[static_cast<std::size_t>(i + 1)];
        return std::string();
    }
    return autoplay_.relatedAutoplay() ? relatedCandidate_ : std::string();
}

// RES-11: once per item, when due; waits for a candidate that arrives later.
void PlaybackController::maybePrefetch() {
    if (!prefetchPending_ || local_) return;  // DL-07: a downloaded item makes no network request
    if (loop_) return;  // Shorts: a loop item never advances, there is no next item to prepare
    const std::string next = prefetchCandidate();
    if (next.empty() || next == videoId_ || next.compare(0, 6, "local:") == 0 || !catalog::validVideoId(next)) return;
    if (upcoming_.count(next)) return;  // RES-19: an upcoming item is not resolved ahead of its start
    prefetchPending_ = false;
    prefetchedVideo_ = next;
    ResolveRequest r = resolveRequestFor(next, capabilities_, effectivePreferences(), media::QualityChoice());
    r.speculative = true;
    port_->prefetch(r);
}

void PlaybackController::setQueue(const std::vector<std::string> &videoIds, int currentIndex) {
    autoplay_.setQueue(videoIds, currentIndex);
    upNextVideo_.clear();
    changes_ |= ControlsChange | UpNextChange;
    maybePrefetch();
}

int PlaybackController::queueLength() const {
    return autoplay_.queue().empty() ? -1 : static_cast<int>(autoplay_.queue().size());
}

int PlaybackController::queueIndex() const { return autoplay_.queue().empty() ? -1 : autoplay_.currentIndex(); }

// A video opened from outside the queue ends the queue; one from inside moves it.
void PlaybackController::syncQueue(const std::string &videoId) {
    if (autoplay_.queue().empty()) return;
    const std::vector<std::string> &q = autoplay_.queue();
    const int current = autoplay_.currentIndex();
    if (current >= 0 && current < static_cast<int>(q.size()) && q[static_cast<std::size_t>(current)] == videoId) return;
    std::vector<std::string>::const_iterator it = std::find(q.begin(), q.end(), videoId);
    if (it == q.end()) autoplay_.clearQueue();
    else autoplay_.setQueue(std::vector<std::string>(q), static_cast<int>(it - q.begin()));
    changes_ |= ControlsChange;
}

void PlaybackController::applyAutoplay(std::int64_t nowMs, const player::AutoplayDecision &d) {
    switch (d.kind) {
    case player::AutoplayDecision::PlayItem:
        upNextVideo_.clear();
        changes_ |= UpNextChange;
        if (d.replacePage) { replaceVideo_ = d.videoId; changes_ |= ReplacePageChange; }
        if (localAvailable_ && localAvailable_(d.videoId)) {
            // PLY-26: the session plays the download (no resolve, works offline).
            pendingLocalOpen_ = d.videoId;
            changes_ |= LocalOpenChange;
            break;
        }
        automaticOpen_ = true;  // RES-19: an upcoming item stays held until its start
        open(nowMs, d.videoId);
        automaticOpen_ = false;
        break;
    case player::AutoplayDecision::StartCountdown:
        upNextVideo_ = d.videoId;
        changes_ |= UpNextChange;
        break;
    case player::AutoplayDecision::RestartCurrent:
        seek(0);
        if (!wantPlay_) play(nowMs);
        break;
    case player::AutoplayDecision::StopAtEnd:
    case player::AutoplayDecision::None:
        break;
    }
}

void PlaybackController::next(std::int64_t nowMs) { applyAutoplay(nowMs, autoplay_.next()); }

void PlaybackController::previous(std::int64_t nowMs) { applyAutoplay(nowMs, autoplay_.previous(positionMs_)); }

void PlaybackController::cancelUpNext() {
    autoplay_.cancelCountdown();
    upNextVideo_.clear();
    changes_ |= UpNextChange;
}

// ---- SponsorBlock (SB-02) -------------------------------------------------------------------

void PlaybackController::setSponsorSegments(const std::vector<player::SponsorSegment> &segments) {
    skipper_.setSegments(segments);
    segments_ = segments;
    changes_ |= SegmentsChange;
    buttonVisible_ = false;
    highlightVisible_ = false;
    changes_ |= SponsorChange;
}

void PlaybackController::skipSponsor() {
    const std::int64_t target = skipper_.manualSkip(positionMs_, owner());
    if (target >= 0) {
        stall_.onInternalSeek();  // the jump is not progress
        port_->postSeek(target);
    }
    if (buttonVisible_) { buttonVisible_ = false; changes_ |= SponsorChange; }
}

// ---- Resume (PLY-26, MPR-02) ----------------------------------------------------------------

void PlaybackController::noteResumable(const std::string &videoId, std::int64_t positionMs) {
    if (positionMs != player::kNoResume) resumableVideo_ = videoId;
    else if (resumableVideo_ == videoId) resumableVideo_.clear();
    changes_ |= ControlsChange;
}

bool PlaybackController::continueWatching(std::int64_t nowMs) {
    if (resumableVideo_.empty()) return false;
    if (resumableVideo_ == localVideoId_) openLocal(nowMs, localMedia_);
    else open(nowMs, resumableVideo_);
    return true;
}

// ---- Player events ----------------------------------------------------------------------------

void PlaybackController::onPlayerStarted(std::int64_t nowMs) {
    if (!loaded_ || started_) return;
    started_ = true;
    // RES-11: the next item's resolve waits until this item's preroll is done.
    if (!everStarted_ && prefetchedVideo_.empty() && !prefetchPending_) prefetchDeadline_ = nowMs + kPrefetchDelayMs;
    everStarted_ = true;
    watchdogDeadline_ = -1;
    setState(Ready);
    if (switching_) { switching_ = false; changes_ |= SwitchResumedChange; }
    changes_ |= PlayerChange | ControlsChange;
    stallReload_ = false;
    watchStall(nowMs);  // from here on the position has to advance
}

void PlaybackController::onFirstFrame(std::int64_t nowMs) {
    onPlayerStarted(nowMs);
    if (loaded_) firstFrameSeen_ = true;  // cover video
    // NFR-PERF-02 A/B: the low rung's playing time counts from its first frame.
    if (lowRungPending_ && !lowRungFrame_ && loaded_) {
        lowRungFrame_ = true;
        lowRungFromMs_ = positionMs_;
    }
}

void PlaybackController::onPlayerPlaying(bool playing) {
    if (playing == playerPlaying_) return;
    playerPlaying_ = playing;
    stallDirty_ = true;  // e.g. held for buffering: the shorter threshold applies
    changes_ |= PlayerChange | ControlsChange;
}

void PlaybackController::onPosition(std::int64_t nowMs, std::int64_t positionMs) {
    if (!loaded_) return;
    positionMs_ = positionMs;
    if (playerPlaying_) recovery_.onPlaybackProgress(positionMs);
    // Stall watchdog: the poll's report moves its one deadline (only ever None or Recovered here);
    // without a baseline the next tick takes one.
    if (stall_.armed()) applyStall(nowMs, stall_.onPosition(nowMs, positionMs));
    else stallDirty_ = true;
    // NFR-PERF-02 A/B: 2 s played on the low rung; the switch runs at the next tick (PLT-05).
    if (lowRungFrame_ && lowRungDeadline_ < 0 && playerPlaying_ && positionMs - lowRungFromMs_ >= kLowRungPlayMs)
        lowRungDeadline_ = nowMs;
    // SB-02: the decision is pure; the seek is deferred by the port.
    const player::SkipDecision s = skipper_.evaluate(positionMs, playerPlaying_, owner());
    if (s.seek) {
        stall_.onInternalSeek();  // the jump is not progress
        port_->postSeek(s.seekToMs);
    }
    if (s.notice) {
        noticeCategory_ = s.noticeCategory;
        noticeDeadline_ = nowMs + s.noticeMs;
        changes_ |= SponsorChange;
    }
    if (s.showButton != buttonVisible_ || (s.showButton && s.buttonCategory != buttonCategory_)) {
        buttonVisible_ = s.showButton;
        buttonCategory_ = s.buttonCategory;
        changes_ |= SponsorChange;
    }
    if (s.showHighlight != highlightVisible_) {
        highlightVisible_ = s.showHighlight;
        highlightMs_ = s.highlightMs;
        changes_ |= SponsorChange;
    }
}

void PlaybackController::onDuration(std::int64_t durationMs) {
    if (durationMs > 0) durationMs_ = durationMs;
}

void PlaybackController::onSeekable(bool seekable) {
    if (seekable == seekable_) return;
    seekable_ = seekable;
    changes_ |= PlayerChange | ControlsChange;
}

void PlaybackController::onPlayerFailed(std::int64_t nowMs, const std::string &reason, bool network) {
    if (!loaded_) return;
    fault(nowMs, reason, network);
}

void PlaybackController::onEnded(std::int64_t nowMs) {
    if (!loaded_) return;
    // PLY-24: the position is unreliable at the end; record the duration.
    const std::int64_t metadata = details_.summary.durationSeconds > 0 ? details_.summary.durationSeconds * 1000 : 0;
    positionMs_ = player::positionToRecordAtEnd(durationMs_, metadata, positionMs_);
    if (owner() && !(loop_ && loopEndSaved_)) {
        port_->savePosition(videoId_, resumePointFor(positionMs_, durationMs_ > 0 ? durationMs_ : metadata));
        noteResumable(videoId_, player::kNoResume);
        loopEndSaved_ = loop_;
    }
    // Sleep timer at the end of the video: pause here (never stop), nothing advances.
    const bool sleep = sleepAtEnd_;
    if (sleep) sleepNow();
    if (loop_) {
        // Shorts loop item: back to 0 in the same pipeline (one flush seek), playing on; nothing
        // advances (no countdown, no autoplay, no prefetch, queue untouched).
        stall_.onInternalSeek();  // not a user seek: the stall budget stays
        seekTo(0);
        if (wantPlay_) port_->play();
        changes_ |= PlayerChange;
        return;
    }
    playerPlaying_ = false;
    stall_.suspend();  // the end is not a stall
    changes_ |= EndedChange | PlayerChange | ControlsChange;
    // RES-11: an item that ended before its prefetch was due prefetches now (no preroll to spare).
    if (prefetchDeadline_ >= 0) {
        prefetchDeadline_ = -1;
        prefetchPending_ = true;
        maybePrefetch();
    }
    if (!sleep) applyAutoplay(nowMs, autoplay_.onEnded(nowMs, owner()));
}

void PlaybackController::onDecodeFallback(const std::string &reason) {
    decodeFallbackReason_ = reason;
    changes_ |= StreamChange;
}

void PlaybackController::onAudioGranted() {
    applyAudio(audio_.onGranted());
    stallDirty_ = true;
}

void PlaybackController::onAudioLost() {
    applyAudio(audio_.onLost(playerPlaying_));
    wantPlay_ = false;
    stall_.suspend();  // paused by a call or an alarm: not a stall
    changes_ |= PlayerChange;
}

void PlaybackController::setAppActive(std::int64_t nowMs, bool active) {
    if (active == appActive_) return;
    appActive_ = active;
    changes_ |= ControlsChange;
    // A Short never plays on in the background (the lock screen, the cover): the hold "app".
    if (shortItem_) {
        if (!active) holdShort("app");
        else releaseShort(nowMs, "app");
    }
    // Background playback off: leaving the foreground pauses what plays or is about to.
    else if (!active && !backgroundPlayback_ && wantPlay_ && state_ != Idle && state_ != Failed) pause();
    applyRecovery(nowMs, recovery_.setForeground(active, nowMs));
}

void PlaybackController::tick(std::int64_t nowMs) {
    if (loadDeadline_ >= 0 && nowMs >= loadDeadline_) loadNow(nowMs);
    // RES-09: the rate-limited resolve's one re-resolve, after its pause.
    if (retryDeadline_ >= 0 && nowMs >= retryDeadline_) {
        retryDeadline_ = -1;
        if (state_ == Loading && status_ == RetryingLater && !resolving_) startResolve(true);
    }
    if (lowRungDeadline_ >= 0 && nowMs >= lowRungDeadline_) switchUpFromLowRung(nowMs);
    if (watchdogDeadline_ >= 0 && nowMs >= watchdogDeadline_ && !started_) {
        watchdogDeadline_ = -1;
        startTimedOut_ = true;
        fault(nowMs, "no media within 25 s", true);
        startTimedOut_ = false;
    }
    const std::int64_t recoveryAt = recovery_.nextDeadlineMs();
    if (recoveryAt >= 0 && nowMs >= recoveryAt) applyRecovery(nowMs, recovery_.onTimer(nowMs));
    // Stall watchdog: its one deadline, or a guard that may have changed since the last look.
    const std::int64_t stallAt = stall_.nextDeadlineMs();
    if ((stallDirty_ && stallPort_) || (stallAt >= 0 && nowMs >= stallAt)) watchStall(nowMs);
    if (autoplay_.countdownActive() && nowMs >= autoplay_.nextDeadlineMs()) {
        const player::AutoplayDecision d = autoplay_.advance(nowMs);
        upNextVideo_.clear();
        changes_ |= UpNextChange;
        if (owner()) applyAutoplay(nowMs, d);
    }
    if (noticeDeadline_ >= 0 && nowMs >= noticeDeadline_) {
        noticeDeadline_ = -1;
        changes_ |= SponsorChange;
    }
    if (streamNoticeDeadline_ >= 0 && nowMs >= streamNoticeDeadline_) {
        streamNoticeDeadline_ = -1;
        changes_ |= StateChange;  // errorText
    }
    if (prefetchDeadline_ >= 0 && nowMs >= prefetchDeadline_) {
        prefetchDeadline_ = -1;
        prefetchPending_ = true;
        maybePrefetch();
    }
    if (sleepDeadline_ >= 0 && nowMs >= sleepDeadline_) sleepNow();
    // RES-19: the scheduled start: one fresh resolve (a cached "upcoming" answer is dropped).
    if (upcomingWaiting_ && nowMs >= scheduledStartUnixMs() - unixOffsetMs_) {
        upcomingWaiting_ = false;
        std::map<std::string, Upcoming>::iterator it = upcoming_.find(videoId_);
        if (it != upcoming_.end()) it->second.scheduled = false;
        changes_ |= StateChange;
        if (upcoming() && arbiter_.hasSession() && renewGeneration()) startResolve(true);
    }
}

std::int64_t PlaybackController::nextDeadlineMs() const {
    std::int64_t best = -1;
    keepEarliest(best, loadDeadline_);
    keepEarliest(best, retryDeadline_);
    keepEarliest(best, watchdogDeadline_);
    keepEarliest(best, recovery_.nextDeadlineMs());
    keepEarliest(best, autoplay_.nextDeadlineMs());
    keepEarliest(best, noticeDeadline_);
    keepEarliest(best, streamNoticeDeadline_);
    keepEarliest(best, prefetchDeadline_);
    keepEarliest(best, lowRungDeadline_);
    keepEarliest(best, sleepDeadline_);
    if (upcomingWaiting_) keepEarliest(best, std::max<std::int64_t>(0, scheduledStartUnixMs() - unixOffsetMs_));
    // Stall watchdog: one deadline that moves with the position (never fires while it advances);
    // "now" when a guard may have changed. Only with a port that has a player to ask.
    keepEarliest(best, stall_.nextDeadlineMs());
    if (stallDirty_ && stallPort_) keepEarliest(best, std::int64_t(0));
    return best;
}

// ---- Watch overlay controls ------------------------------------------------------------------

void PlaybackController::setLoop(bool on) {
    if (on == loop_) return;
    applyLoopHint(true, on ? kLoopOn : kLoopOff);  // the open hint on a re-open of the same video
    if (on && (autoplay_.countdownActive() || !upNextVideo_.empty())) {
        autoplay_.cancelCountdown();
        upNextVideo_.clear();
        changes_ |= UpNextChange;
    }
}

void PlaybackController::holdSpeed(bool held) {
    if (held == speedHeld_) return;
    const bool edge = liveEdge();
    if (held && edge) return;  // PLY-07: no speed change at the live edge
    speedHeld_ = held;
    const double applied = held ? kHoldSpeedRate : player::speed::rateToApply(rememberedRate_, edge);
    if (applied != rate_) {
        rate_ = applied;
        if (loaded_) port_->setRate(applied);
    }
    changes_ |= PlayerChange;
}

void PlaybackController::setSleepTimer(std::int64_t nowMs, int minutes) {
    const bool was = sleepTimerActive();
    sleepDeadline_ = -1;
    sleepAtEnd_ = false;
    if (minutes > 0) sleepDeadline_ = nowMs + static_cast<std::int64_t>(std::min(minutes, kSleepTimerMaxMinutes)) * 60000;
    else if (minutes == kSleepAtEndOfVideo) sleepAtEnd_ = true;
    if (was || sleepTimerActive()) changes_ |= SleepTimerChange;
}

int PlaybackController::sleepTimerSecondsLeft(std::int64_t nowMs) const {
    if (sleepDeadline_ >= 0) {
        const std::int64_t left = sleepDeadline_ > nowMs ? sleepDeadline_ - nowMs : 0;
        return static_cast<int>((left + 999) / 1000);
    }
    if (!sleepAtEnd_) return -1;
    if (durationMs_ <= 0) return 0;
    const double left = static_cast<double>(std::max<std::int64_t>(0, durationMs_ - positionMs_)) / (rate_ > 0 ? rate_ : 1.0);
    return static_cast<int>(std::ceil(left / 1000.0));
}

// The timer fired: pause, keep everything else (the page, the owner, the position, the queue).
void PlaybackController::sleepNow() {
    sleepDeadline_ = -1;
    sleepAtEnd_ = false;
    changes_ |= SleepTimerChange;
    if (autoplay_.countdownActive() || !upNextVideo_.empty()) {
        autoplay_.cancelCountdown();
        upNextVideo_.clear();
        changes_ |= UpNextChange;
    }
    if (state_ == Idle) return;
    pause();
}

AudioChoice PlaybackController::chainAudioChoice() const {
    AudioChoice a = audioChoice_;
    a.stableVolume = stableVolume_;
    return a;
}

std::vector<media::AudioTrackOption> PlaybackController::audioTrackOptions() const {
    const media::StreamCandidate *c = candidate();
    if (!detailsLoaded_ || local_ || !c || c->audio.url.empty() ||
        (c->kind != media::StreamKind::Adaptive && c->kind != media::StreamKind::AudioOnly))
        return std::vector<media::AudioTrackOption>();
    return media::audioTrackOptions(selectionInput(), capabilities_);
}

int PlaybackController::playingAudioTrackIndex() const {
    const std::vector<media::AudioTrackOption> options = audioTrackOptions();
    const media::StreamCandidate *c = candidate();
    if (options.empty() || !c) return -1;
    const std::string language = media::normalizedLanguage(c->audio.language);
    const bool descriptive = media::descriptiveTrack(c->audio);
    for (std::size_t i = 0; i < options.size(); ++i)
        if (options[i].language == language && options[i].descriptive == descriptive) return static_cast<int>(i);
    return -1;
}

bool PlaybackController::setAudioTrack(std::int64_t nowMs, const std::string &id) {
    if (!detailsLoaded_ || local_) return false;
    const std::vector<media::AudioTrackOption> options = media::audioTrackOptions(selectionInput(), capabilities_);
    for (std::size_t i = 0; i < options.size(); ++i) {
        const media::AudioTrackOption &o = options[i];
        if (audioTrackId(o) != id) continue;
        audioChoice_.chosen = true;
        audioChoice_.language = o.language;
        audioChoice_.descriptive = o.descriptive;
        if (!o.descriptive) {  // SEL-05: the original stores "no preference"
            rememberedDub_ = media::storedLanguagePreference(o);
            dubRemembered_ = true;
        }
        changes_ |= StreamChange;
        switchAudioIfNeeded(nowMs);
        return true;
    }
    return false;
}

bool PlaybackController::stableVolumeAvailable() const {
    const media::StreamCandidate *c = candidate();
    if (state_ == Idle || !detailsLoaded_ || local_ || !c || c->audio.url.empty() ||
        (c->kind != media::StreamKind::Adaptive && c->kind != media::StreamKind::AudioOnly))
        return false;
    const std::vector<media::MediaFormat> pool = media::playableAudioFormats(selectionInput(), capabilities_);
    // The entry applyAudioChoice() works on: the chosen one, else the playing track's.
    const std::string language = audioChoice_.chosen ? audioChoice_.language : media::normalizedLanguage(c->audio.language);
    const bool descriptive = audioChoice_.chosen ? audioChoice_.descriptive : media::descriptiveTrack(c->audio);
    const media::MediaFormat *levelled = media::bestEntryTrack(pool, language, descriptive, true, c->audio.audioCodec);
    const media::MediaFormat *plain = media::bestEntryTrack(pool, language, descriptive, false, c->audio.audioCodec);
    return levelled && plain && levelled->dynamicRangeCompressed && !plain->dynamicRangeCompressed;
}

void PlaybackController::setStableVolume(std::int64_t nowMs, bool on) {
    if (on == stableVolume_) return;
    stableVolume_ = on;
    changes_ |= StreamChange;
    if (detailsLoaded_ && !local_) switchAudioIfNeeded(nowMs);
}

// One same-set switch (SEL-07) when the audio the chain would now pick differs from the playing one.
bool PlaybackController::switchAudioIfNeeded(std::int64_t nowMs) {
    if (state_ == Idle || state_ == Failed || resolving_) return false;  // applies to the next selection
    const media::StreamCandidate *c = candidate();
    if (!c || c->audio.url.empty()) return false;
    std::vector<media::StreamCandidate> next =
        media::selectStreamsWithFallback(selectionInput(), capabilities_, effectivePreferences(), choice_);
    applyAudioChoice(&next, selectionInput(), capabilities_, chainAudioChoice());
    if (next.empty() || (next[0].kind == c->kind && next[0].audio.url == c->audio.url)) return false;
    changes_ |= AudioSwitchChange;
    setQuality(nowMs, choice_);  // same set, captured position, a re-resolve only near expiry
    return true;
}

void PlaybackController::setMetadata(const std::string &videoId, const catalog::VideoDetails &m) {
    if (local_ || videoId.empty() || videoId != videoId_) return;
    catalog::VideoSummary &s = details_.summary;
    if (s.title.empty()) s.title = m.summary.title;
    if (s.channel.empty()) s.channel = m.summary.channel;
    if (s.channelId.empty()) s.channelId = m.summary.channelId;
    if (s.thumbnailUrl.empty()) s.thumbnailUrl = m.summary.thumbnailUrl;
    if (s.viewCount < 0) s.viewCount = m.summary.viewCount;
    if (s.durationSeconds <= 0 && m.summary.durationSeconds > 0) {
        s.durationSeconds = m.summary.durationSeconds;
        if (durationMs_ <= 0) durationMs_ = s.durationSeconds * 1000;
    }
    if (details_.description.empty()) details_.description = m.description;
    if (details_.uploadDate.empty()) details_.uploadDate = m.uploadDate;
    if (details_.likeCount < 0) details_.likeCount = m.likeCount;
    if (details_.channelFollowerCount < 0) details_.channelFollowerCount = m.channelFollowerCount;
    if (details_.chapters.empty()) details_.chapters = m.chapters;
    if (details_.tags.empty()) details_.tags = m.tags;
    changes_ |= DetailsChange;
}

// ---- Read-out ----------------------------------------------------------------------------------

void PlaybackController::setState(State state, Status status) {
    if (state != state_ || status != status_) changes_ |= StateChange | PlayerChange | ControlsChange;
    state_ = state;
    status_ = status;
    if (state != Failed) {
        failure_ = NoFailure;
        stallFailed_ = false;
    }
}

const media::StreamCandidate *PlaybackController::candidate() const {
    if (local_ || attempt_ < 0 || attempt_ >= static_cast<int>(chain_.size())) return nullptr;
    return &chain_[static_cast<std::size_t>(attempt_)];
}

bool PlaybackController::hasVideo() const {
    if (local_) return !audioOnly_ && !localMedia_.videoPath.empty();
    const media::StreamCandidate *c = candidate();
    return c && c->kind != media::StreamKind::AudioOnly;
}

bool PlaybackController::coverKeepsDecoding() const {
    return coverVisible_ && coverVideoSetting_ && loaded_ && hasVideo() && !audioOnly_ && state_ != Failed;
}

const char *PlaybackController::coverOffReason() const {
    if (appActive_) return "foreground";
    if (!coverVisible_) return "cover-hidden";
    if (!coverVideoSetting_) return "setting-off";
    if (!loaded_) return "idle";
    if (!hasVideo() || audioOnly_) return "no-video";
    if (state_ == Failed) return "error";
    return "";
}

bool PlaybackController::coverVideoActive() const {
    return coverVideoSetting_ && !appActive_ && loaded_ && hasVideo() && !audioOnly_ && firstFrameSeen_ &&
           state_ != Failed;
}

double PlaybackController::plannedVideoAspect() const {
    if (!hasVideo() || audioOnly_) return 0.0;
    int w = 0, h = 0;
    if (local_) {
        w = localMedia_.width;
        h = localMedia_.height;
    } else if (const media::StreamCandidate *c = candidate()) {
        const media::MediaFormat &f = c->kind == media::StreamKind::Adaptive ? c->video : c->muxed;
        w = f.width;
        h = f.height;
    }
    return w > 0 && h > 0 ? static_cast<double>(w) / h : 0.0;
}

bool CoverDecodeMeter::update(bool lifted, std::int64_t nowMs) {
    if (lifted == on) return false;
    if (on && nowMs > sinceMs) totalMs += nowMs - sinceMs;
    on = lifted;
    sinceMs = nowMs;
    return true;
}

bool PlaybackController::live() const {
    return !local_ && details_.summary.liveStatus == media::LiveStatus::IsLive;
}

bool PlaybackController::hardwareDecodeFallback() const {
    const media::StreamCandidate *c = candidate();
    return c && c->hardwareDecodeFallback;
}

bool PlaybackController::keepDisplayOn() const {
    player::KeepOnInputs in;
    in.settingEnabled = keepOnSetting_;
    in.playing = playerPlaying_;
    in.foreground = appActive_ && !arbiter_.parked();  // a watch page shows the picture
    in.hasVideo = hasVideo();
    in.listenOnly = audioOnly_;
    in.keepAliveAvailable = keepAliveAvailable_;
    return player::preventBlanking(in);
}

player::MediaControlsState PlaybackController::mediaControlsState() const {
    player::MediaControlsState s;
    s.hasSession = !videoId_.empty() && state_ != Idle;
    s.playing = playerPlaying_;
    s.seekable = seekable_;
    s.live = live();
    s.canGoNext = autoplay_.canGoNext();
    s.canGoPrevious = autoplay_.canGoPrevious();
    s.hasResumable = hasResumable();
    return s;
}

player::MprisCapabilities PlaybackController::mprisCapabilities() const {
    return player::mprisCapabilities(mediaControlsState());
}

player::CoverState PlaybackController::coverState() const { return player::coverState(mediaControlsState()); }

// ---- Error -> setting ---------------------------------------------------------------------------

ErrorSettingFacts PlaybackController::errorSettingFacts(bool noPlayableAnswer) const {
    ErrorSettingFacts f;
    if (state_ != Failed) return f;
    f.failure = failure_;
    f.local = local_;
    f.stalled = stallFailed_;
    f.audioOnly = audioOnly_;
    f.noPlayableAnswer = noPlayableAnswer && failure_ == ResolveFailed;
    f.requestMaxHeight = failedMaxHeight_;
    f.mobileNetwork = preferences_.network == media::NetworkClass::Mobile;
    f.hardwareDecode = loadHardwareSetting_;
    // NoPlayableStream either from an answered set that gave no candidate, or after every
    // candidate failed in the player (the chain is kept then).
    f.playerFault = failure_ == PlaybackFailed || (failure_ == NoPlayableStream && !chain_.empty());
    if (failure_ == NoPlayableStream && chain_.empty() && detailsLoaded_ && !local_) {
        f.emptySelection = true;
        const media::SelectionInput in = selectionInput();
        f.noDecodableVideo = media::emptySelectionKind(in, capabilities_, effectivePreferences()) ==
                             media::EmptySelection::NoDecodableVideo;
        if (audioOnly_) {
            media::PlaybackPreferences video = effectivePreferences();
            video.mode = media::PlaybackMode::Video;
            f.videoModePlayable = !media::selectStreamsWithFallback(in, capabilities_, video, choice_).empty();
        }
    }
    return f;
}

std::string errorSettingKey(const ErrorSettingFacts &f) {
    if (f.local) return std::string();  // a downloaded file: no setting changes it
    if (f.stalled) return std::string();  // the stream stopped delivering: no setting fixes that
    switch (f.failure) {
    case PlaybackController::PlaybackFailed:
        return f.hardwareDecode ? "playback/hardwareDecode" : std::string();
    case PlaybackController::NoPlayableStream:
        if (f.playerFault) return f.hardwareDecode ? "playback/hardwareDecode" : std::string();
        if (!f.emptySelection) return std::string();
        if (f.audioOnly) return f.videoModePlayable ? "playback/audioOnly" : std::string();
        if (f.noDecodableVideo) return "playback/codecPreference";
        return std::string();
    case PlaybackController::ResolveFailed:
        if (!f.noPlayableAnswer) return std::string();  // network, provider and final classes
        if (f.audioOnly) return "playback/audioOnly";
        if (f.requestMaxHeight > 0) return f.mobileNetwork ? "playback/maxHeightMobile" : "playback/maxHeight";
        return std::string();
    case PlaybackController::NoFailure:
    case PlaybackController::UpcomingStream:
    case PlaybackController::NetworkFailed:
        break;
    }
    return std::string();
}

}}}
