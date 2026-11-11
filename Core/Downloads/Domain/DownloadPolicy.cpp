// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "DownloadPolicy.h"
#include "../../Catalog/Domain/Video.h"

#include <algorithm>
#include <climits>
#include <set>

namespace nordtile { namespace video { namespace downloads {

using media::AudioCodec;
using media::MediaFormat;
using media::VideoCodec;

namespace {

bool direct(const MediaFormat &f) { return f.protocol == media::Protocol::Https && !f.url.empty(); }
bool audioOnly(const MediaFormat &f) { return direct(f) && f.hasAudio() && f.videoCodec == VideoCodec::None; }
bool videoOnly(const MediaFormat &f) { return direct(f) && f.hasVideo() && f.audioCodec == AudioCodec::None; }
bool aacM4a(const MediaFormat &f) {
    return audioOnly(f) && f.audioCodec == AudioCodec::Aac && (f.container == "m4a" || f.container == "mp4");
}
bool opusWebm(const MediaFormat &f) { return audioOnly(f) && f.audioCodec == AudioCodec::Opus && f.container == "webm"; }
bool itag140(const MediaFormat &f) { return f.id == "140" || f.id.compare(0, 4, "140-") == 0; }
double audioRate(const MediaFormat &f) { return f.audioKbps > 0 ? f.audioKbps : f.totalKbps; }

// true when a is a better audio track than b.
bool betterAudio(const MediaFormat &a, const MediaFormat &b) {
    if (a.languagePreference != b.languagePreference) return a.languagePreference > b.languagePreference;
    if (a.dynamicRangeCompressed != b.dynamicRangeCompressed) return !a.dynamicRangeCompressed;
    if (itag140(a) != itag140(b)) return itag140(a);
    return audioRate(a) > audioRate(b);
}

const MediaFormat *bestAudio(const std::vector<MediaFormat> &formats, bool (*accept)(const MediaFormat &)) {
    const MediaFormat *best = nullptr;
    for (const MediaFormat &f : formats)
        if (accept(f) && (!best || betterAudio(f, *best))) best = &f;
    return best;
}

bool pairableVideo(const MediaFormat &f, const media::DecoderCapabilities &caps) {
    if (!videoOnly(f) || f.highDynamicRange || f.height <= 0) return false;
    const bool container = (f.videoCodec == VideoCodec::H264 && f.container == "mp4")
        || (f.videoCodec == VideoCodec::Vp9 && f.container == "webm");
    return container && caps.video(f.videoCodec).available;
}

bool progressiveVideo(const MediaFormat &f, const media::DecoderCapabilities &caps) {
    if (!direct(f) || !f.muxed() || f.highDynamicRange) return false;
    if (f.videoCodec == VideoCodec::Av1 || !caps.video(f.videoCodec).available || !caps.audio(f.audioCodec)) return false;
    return (f.container == "mp4" && f.videoCodec == VideoCodec::H264) || f.container == "webm";
}

struct Rank {
    int height = 0;
    bool muxable = false;
    bool hardware = false;
    bool h264 = false;
    double fps = 0;
    double kbps = 0;
    bool better(const Rank &o) const {
        if (height != o.height) return height > o.height;
        if (muxable != o.muxable) return muxable;
        if (hardware != o.hardware) return hardware;
        if (h264 != o.h264) return h264;
        if (fps != o.fps) return fps > o.fps;
        return kbps > o.kbps;
    }
};

} // namespace

AudioPick pickAudio(const std::vector<MediaFormat> &formats) {
    AudioPick pick;
    if (const MediaFormat *aac = bestAudio(formats, aacM4a)) {
        pick.ok = true; pick.format = *aac; pick.extension = "m4a";
    } else if (const MediaFormat *opus = bestAudio(formats, opusWebm)) {
        pick.ok = true; pick.format = *opus; pick.extension = "webm";
    }
    return pick;
}

int effectiveQualityHeight(int requested, int playbackCeiling) {
    return requested > 0 ? requested : (playbackCeiling > 0 ? playbackCeiling : 0);
}

DownloadDefault downloadDefaultFor(int qualitySetting, int playbackCeiling) {
    DownloadDefault d;
    if (qualitySetting == downloadQualityAudioOnly) {
        d.kind = Kind::Audio;
        d.qualityHeight = playbackCeiling > 0 ? playbackCeiling : downloadQualityFallbackHeight;
    } else if (qualitySetting > 0) {
        d.qualityHeight = qualitySetting;
    } else {
        d.qualityHeight = playbackCeiling > 0 ? playbackCeiling : 2160;
    }
    return d;
}

MuxTarget muxTargetFor(VideoCodec video, AudioCodec audio) {
    if (video == VideoCodec::H264 && audio == AudioCodec::Aac) return MuxTarget::Mp4;
    if (video == VideoCodec::Vp9 && audio == AudioCodec::Opus) return MuxTarget::WebM;
    return MuxTarget::None;
}

OutputLayout outputLayoutFor(MuxTarget target, const MuxCapabilities &mux) {
    if ((target == MuxTarget::Mp4 && mux.mp4) || (target == MuxTarget::WebM && mux.webm)) return OutputLayout::MuxedFile;
    return OutputLayout::TrackPair;
}

VideoPick pickVideo(const std::vector<MediaFormat> &formats, int maxHeight,
                    const media::DecoderCapabilities &decoders, const MuxCapabilities &mux) {
    const int cap = maxHeight > 0 ? maxHeight : INT_MAX;
    const MediaFormat *aac = bestAudio(formats, aacM4a);
    const MediaFormat *opus = bestAudio(formats, opusWebm);
    VideoPick pick;
    Rank bestRank;
    for (const MediaFormat &f : formats) {
        if (!pairableVideo(f, decoders) || f.height > cap) continue;
        const MediaFormat *audio = f.videoCodec == VideoCodec::H264 ? aac : opus;
        if (!audio) continue;
        Rank r;
        r.height = f.height;
        const MuxTarget target = muxTargetFor(f.videoCodec, audio->audioCodec);
        r.muxable = outputLayoutFor(target, mux) == OutputLayout::MuxedFile;
        r.hardware = decoders.video(f.videoCodec).hardware;
        r.h264 = f.videoCodec == VideoCodec::H264;
        r.fps = f.fps;
        r.kbps = f.totalKbps;
        if (pick.ok && !r.better(bestRank)) continue;
        bestRank = r;
        pick.ok = true;
        pick.progressive = false;
        pick.video = f;
        pick.audio = *audio;
        pick.mux = target;
        pick.layout = outputLayoutFor(target, mux);
        pick.videoExtension = f.container;
        pick.audioExtension = audio->audioCodec == AudioCodec::Aac ? "m4a" : "webm";
        pick.outputExtension = target == MuxTarget::Mp4 ? "mp4" : "webm";
    }
    if (pick.ok) return pick;
    for (const MediaFormat &f : formats) {
        if (!progressiveVideo(f, decoders) || f.height > cap) continue;
        Rank r;
        r.height = f.height;
        r.hardware = decoders.video(f.videoCodec).hardware;
        r.h264 = f.videoCodec == VideoCodec::H264;
        r.fps = f.fps;
        r.kbps = f.totalKbps;
        if (pick.ok && !r.better(bestRank)) continue;
        bestRank = r;
        pick.ok = true;
        pick.progressive = true;
        pick.video = f;
        pick.audio = MediaFormat();
        pick.mux = MuxTarget::None;
        pick.layout = OutputLayout::SingleFile;
        pick.videoExtension = f.container;
        pick.audioExtension.clear();
        pick.outputExtension = f.container;
    }
    return pick;
}

std::vector<int> videoQualityHeights(const std::vector<MediaFormat> &formats, const media::DecoderCapabilities &decoders) {
    const bool aac = bestAudio(formats, aacM4a) != nullptr;
    const bool opus = bestAudio(formats, opusWebm) != nullptr;
    std::set<int> heights;
    for (const MediaFormat &f : formats) {
        if (pairableVideo(f, decoders) && (f.videoCodec == VideoCodec::H264 ? aac : opus)) heights.insert(f.height);
        else if (progressiveVideo(f, decoders) && f.height > 0) heights.insert(f.height);
    }
    return std::vector<int>(heights.rbegin(), heights.rend());
}

// ---- state machine ------------------------------------------------------------

Transition transition(State s, Event e) {
    Transition t;
    t.next = s;
    State next = s;
    bool ok = false;
    switch (e) {
    case Event::Start: ok = s == State::Queued; next = State::Resolving; break;
    case Event::Resolved: ok = s == State::Resolving; next = State::Downloading; break;
    case Event::TracksDone: ok = s == State::Downloading; next = State::Remuxing; break;
    case Event::Finished: ok = s == State::Downloading || s == State::Remuxing; next = State::Done; break;
    case Event::Pause:
        ok = s == State::Queued || s == State::Resolving || s == State::Downloading || s == State::WaitingForNetwork;
        next = State::Paused;
        break;
    case Event::Resume: ok = s == State::Paused; next = State::Queued; break;
    case Event::Cancel: ok = s != State::Done && s != State::Cancelled; next = State::Cancelled; break;
    case Event::Fail:
        ok = s == State::Queued || s == State::Resolving || s == State::Downloading || s == State::Remuxing;
        next = State::Failed;
        break;
    case Event::Retry: ok = s == State::Failed || s == State::Cancelled; next = State::Queued; break;
    case Event::NetworkGated:
        ok = s == State::Queued || s == State::Resolving || s == State::Downloading;
        next = State::WaitingForNetwork;
        break;
    case Event::NetworkOpen: ok = s == State::WaitingForNetwork; next = State::Queued; break;
    case Event::Recover:
        ok = s == State::Resolving || s == State::Downloading || s == State::Remuxing;
        next = State::Queued;
        break;
    }
    if (ok) { t.allowed = true; t.next = next; }
    return t;
}

bool isTransfer(State s) { return s == State::Resolving || s == State::Downloading; }

bool isUnfinished(State s) { return s != State::Done && s != State::Failed && s != State::Cancelled; }

bool wantsKeepAlive(State s) {
    return s == State::Queued || s == State::Resolving || s == State::Downloading || s == State::Remuxing;
}

// ---- scheduler ----------------------------------------------------------------

ScheduleDecision schedule(const std::vector<ScheduleItem> &items, bool networkAllowed, int maxConcurrent) {
    std::vector<ScheduleItem> sorted = items;
    std::stable_sort(sorted.begin(), sorted.end(), [](const ScheduleItem &a, const ScheduleItem &b) {
        return a.sortKey != b.sortKey ? a.sortKey < b.sortKey : a.id < b.id;
    });
    ScheduleDecision d;
    if (!networkAllowed) {
        for (const ScheduleItem &i : sorted)
            if (i.state == State::Queued || isTransfer(i.state)) d.gate.push_back(i.id);
        return d;
    }
    int active = 0;
    for (const ScheduleItem &i : sorted)
        if (isTransfer(i.state)) ++active;
    for (const ScheduleItem &i : sorted) {
        const bool waiting = i.state == State::WaitingForNetwork;
        if (waiting) d.ungate.push_back(i.id);
        if ((waiting || i.state == State::Queued) && active < maxConcurrent) {
            d.start.push_back(i.id);
            ++active;
        }
    }
    return d;
}

// ---- resume -----------------------------------------------------------------------

ResumeAction resumeAtStart(std::int64_t partBytes, std::int64_t expectedTotal) {
    if (expectedTotal > 0 && partBytes > expectedTotal) return ResumeAction::RestartFromZero;
    if (expectedTotal > 0 && partBytes == expectedTotal) return ResumeAction::Complete;
    return ResumeAction::ContinueFromOffset;
}

ResumeAction resumeAfterResponse(const ResponseFacts &f, const ResumeLimits &limits) {
    const int s = f.httpStatus;
    if (s == 403 && f.requestOffset == 0 && f.nowMs - f.resolvedAtMs <= limits.youngTokenWindowMs
        && f.youngRetries < limits.youngTokenRetries)
        return ResumeAction::RetryAfterYoungTokenDelay;
    const bool refused = s == 403 || s == 410 || (s == 200 && f.requestOffset > 0);
    if (refused || f.urlExpired)
        return f.reResolves < limits.reResolves ? ResumeAction::ReResolveSameFormat : ResumeAction::Fail;
    if (s == 416) return ResumeAction::RestartFromZero;
    const bool transient = s == 0 || f.truncated || s == 408 || s == 429 || (s >= 500 && s <= 599);
    if (transient) return f.transientRetries < limits.transientRetries ? ResumeAction::ContinueFromOffset : ResumeAction::Fail;
    if (s == 206 || (s == 200 && f.requestOffset == 0)) return ResumeAction::ContinueFromOffset;
    return ResumeAction::Fail;
}

ResumeAction resumeAfterReResolve(std::int64_t storedTotal, std::int64_t newTotal) {
    if (storedTotal <= 0 || newTotal <= 0 || newTotal == storedTotal) return ResumeAction::ContinueFromOffset;
    return ResumeAction::Fail;
}

std::string queryParameter(const std::string &url, const std::string &name) {
    const std::size_t q = url.find('?');
    if (q == std::string::npos) return std::string();
    std::size_t end = url.find('#', q);
    if (end == std::string::npos) end = url.size();
    std::size_t pos = q + 1;
    while (pos < end) {
        std::size_t amp = url.find('&', pos);
        if (amp == std::string::npos || amp > end) amp = end;
        const std::size_t eq = url.find('=', pos);
        if (eq != std::string::npos && eq < amp && url.compare(pos, eq - pos, name) == 0 && eq - pos == name.size())
            return url.substr(eq + 1, amp - eq - 1);
        pos = amp + 1;
    }
    return std::string();
}

namespace {
std::int64_t positiveInteger(const std::string &s) {
    if (s.empty() || s.size() > 18) return 0;
    std::int64_t v = 0;
    for (char c : s) {
        if (c < '0' || c > '9') return 0;
        v = v * 10 + (c - '0');
    }
    return v;
}
}

bool urlExpired(const std::string &url, std::int64_t nowUnixSeconds, std::int64_t marginSeconds) {
    const std::int64_t expire = positiveInteger(queryParameter(url, "expire"));
    return expire > 0 && expire - marginSeconds <= nowUnixSeconds;
}

std::int64_t contentLengthOf(const MediaFormat &f) {
    if (f.fileSize > 0) return f.fileSize;
    return positiveInteger(queryParameter(f.url, "clen"));
}

std::int64_t rangeEnd(std::int64_t offset, std::int64_t total, std::int64_t chunkBytes) {
    std::int64_t end = offset + (chunkBytes > 0 ? chunkBytes : 1) - 1;
    if (total > 0 && end > total - 1) end = total - 1;
    return end;
}

std::string rangeHeader(std::int64_t offset, std::int64_t total, std::int64_t chunkBytes) {
    return "bytes=" + std::to_string(static_cast<long long>(offset)) + "-"
        + std::to_string(static_cast<long long>(rangeEnd(offset, total, chunkBytes)));
}

bool parseContentRange(const std::string &value, std::int64_t *first, std::int64_t *last, std::int64_t *total) {
    std::string v = value;
    if (v.compare(0, 6, "bytes ") != 0) return false;
    v = v.substr(6);
    const std::size_t dash = v.find('-');
    const std::size_t slash = v.find('/');
    if (dash == std::string::npos || slash == std::string::npos || dash > slash) return false;
    const std::string a = v.substr(0, dash), b = v.substr(dash + 1, slash - dash - 1), t = v.substr(slash + 1);
    if (a.empty() || b.empty() || (a != "0" && positiveInteger(a) == 0) || (b != "0" && positiveInteger(b) == 0)) return false;
    const std::int64_t f = positiveInteger(a), l = positiveInteger(b);
    if (l < f) return false;
    std::int64_t tot = 0;
    if (t != "*") {
        tot = positiveInteger(t);
        if (tot <= l) return false;
    }
    if (first) *first = f;
    if (last) *last = l;
    if (total) *total = tot;
    return true;
}

bool progressPersistDue(std::int64_t bytes, std::int64_t ms, std::int64_t everyMs, std::int64_t everyBytes) {
    return bytes >= everyBytes || ms >= everyMs;
}

// ---- DL-06 location -------------------------------------------------------------

LocationDecision chooseLocation(const LocationInput &in) {
    LocationDecision d;
    if (!in.preferred.empty() && in.preferredUsable) {
        d.directory = in.preferred;
        return d;
    }
    d.directory = in.defaultDirectory;
    if (!in.preferred.empty()) {
        d.fellBack = true;
        d.noticeKey = noticeKeys::folderUnavailable;
    }
    return d;
}

bool isTemporaryPath(const std::string &p) {
    const char *const roots[] = { "/tmp", "/var/tmp", "/dev/shm", "/run/user" };
    for (const char *r : roots) {
        const std::string root(r);
        if (p == root || p.compare(0, root.size() + 1, root + "/") == 0) return true;
    }
    return false;
}

std::string validateChosenFolder(const std::string &path, bool exists, bool writable) {
    if (path.empty() || path[0] != '/' || path.find("/../") != std::string::npos
        || (path.size() >= 3 && path.compare(path.size() - 3, 3, "/..") == 0))
        return "downloads_error_folder_invalid";
    if (isTemporaryPath(path)) return "downloads_error_folder_temporary";
    if (!exists) return "downloads_error_folder_missing";
    if (!writable) return "downloads_error_folder_readonly";
    return std::string();
}

// ---- DL-08 ---------------------------------------------------------------------------

std::vector<std::string> idsToEnqueue(const std::vector<std::string> &ids, const std::vector<ExistingDownload> &existing,
                                      Kind kind) {
    std::set<std::string> skip;
    for (const ExistingDownload &e : existing)
        if (e.kind == kind && (e.state == State::Done || isUnfinished(e.state))) skip.insert(e.videoId);
    std::vector<std::string> out;
    for (const std::string &id : ids) {
        if (!catalog::validVideoId(id) || skip.count(id)) continue;
        skip.insert(id);
        out.push_back(id);
    }
    return out;
}

// ---- NFR-SEC-05 response and space checks ---------------------------------------------

bool mediaContentTypeAllowed(const std::string &contentType) {
    std::string t;
    for (char c : contentType) {
        if (c == ';') break;
        if (c == ' ' || c == '\t') continue;
        t += static_cast<char>(c >= 'A' && c <= 'Z' ? c - 'A' + 'a' : c);
    }
    if (t.empty()) return true;
    const auto endsWith = [&t](const char *suffix) {
        const std::string s(suffix);
        return t.size() >= s.size() && t.compare(t.size() - s.size(), s.size(), s) == 0;
    };
    if (t.compare(0, 5, "text/") == 0) return false;
    static const char *const refused[] = { "application/json", "application/xml", "application/javascript",
                                           "application/xhtml+xml", "application/problem+json" };
    for (const char *r : refused) if (t == r) return false;
    return !endsWith("+json") && !endsWith("+xml");
}

std::int64_t spaceNeeded(std::int64_t remainingBytes, std::int64_t outputCopyBytes) {
    return std::max<std::int64_t>(0, remainingBytes) + std::max<std::int64_t>(0, outputCopyBytes) + freeSpaceReserveBytes;
}

bool enoughSpace(std::int64_t availableBytes, std::int64_t neededBytes) {
    return availableBytes < 0 || availableBytes >= neededBytes;
}

}}}
