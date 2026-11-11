// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#pragma once

// Link handling decisions (LNK-01..LNK-04, PLT-13). Qt-free.
//  * Hosts (LNK-01): youtube.com, www.youtube.com, m.youtube.com, youtu.be,
//    www.youtube-nocookie.com (embed). music.youtube.com is recognised but flagged as SailTune's.
//  * Forms (LNK-02), first match wins: 1 youtu.be/<id>, 2 v=, 3 /shorts/ /embed/ /live/ /v/,
//    4 list= (a video found by 1-3 keeps the list and index=), 5 /channel/UC..., 6 /@handle /c/
//    /user/, 7 /clip/ (MAY; recognised, needs a network resolve before it can play).
//  * t= / start= (90, 90s, 1m30s, 1h2m3s), also in the fragment (#t=), set a start position
//    that overrides resume.
//  * D-Bus argument normalisation (LNK-03): a string or a list of strings, first valid element.
//  * Cold-start queue (LNK-03): links that arrive before the engine is ready are kept in order.

#include <cstddef>
#include <deque>
#include <string>
#include <vector>

namespace nordtile { namespace video { namespace catalog {

enum class LinkKind { Unsupported, Video, Playlist, Channel, Clip };
enum class ChannelRefKind { None, ChannelId, Handle, Custom, User };

struct LinkTarget {
    LinkTarget() : kind(LinkKind::Unsupported), startSeconds(-1), index(-1),
                   channelKind(ChannelRefKind::None), sailTuneHost(false), embed(false) {}
    LinkKind kind;
    std::string url;          // the input, trimmed
    std::string host;         // lowercased, "" when not a YouTube host
    std::string videoId;      // Video
    int startSeconds;         // Video: -1 = none (resume applies); >= 0 overrides resume
    std::string playlistId;   // Playlist, or the queue of a Video
    int index;                // index= with a list (as in the URL, 1-based), -1 = none
    std::string channelRef;   // Channel: "UC...", "@handle", or a canonical https URL for /c/ and /user/
    ChannelRefKind channelKind;
    std::string clipId;       // Clip
    bool sailTuneHost;        // music.youtube.com: SailTune's host (LNK-01)
    bool embed;               // /embed/ or youtube-nocookie
    bool supported() const { return kind != LinkKind::Unsupported; }
};

// True for the hosts SailTube handles, and for music.youtube.com (flagged, see sailTuneHost).
bool isYouTubeHost(const std::string &lowercaseHost);
bool isValidVideoId(const std::string &id);      // exactly 11 of [A-Za-z0-9_-]
bool isValidPlaylistId(const std::string &id);   // 2..64 of [A-Za-z0-9_-]
bool isValidChannelId(const std::string &id);    // "UC" + 22 of [A-Za-z0-9_-]
// "90", "90s", "1m30s", "1h2m3s", "2m", "1h" -> seconds; -1 when malformed.
int parseStartTime(const std::string &value);

LinkTarget classifyLink(const std::string &url);

// D-Bus normalisation: index of the first candidate that classifies as supported; when none does,
// the first non-empty candidate (so the caller can report it); -1 when every candidate is empty.
int chooseLinkCandidate(const std::vector<std::string> &candidates);

// Cold-start queue. Before ready, push() keeps links in arrival order (bounded, the oldest kept);
// once ready, push() hands the link straight back for routing.
class LinkQueue {
public:
    static const std::size_t kCapacity = 32;
    LinkQueue() : ready_(false), dropped_(0) {}
    bool ready() const { return ready_; }
    // Returns true when the caller should route `link` now; false when it was queued.
    bool push(const LinkTarget &link);
    // Turning ready on returns the queued links in arrival order (the queue is emptied).
    std::vector<LinkTarget> setReady(bool ready);
    std::size_t pending() const { return queue_.size(); }
    std::size_t dropped() const { return dropped_; }
private:
    bool ready_;
    std::size_t dropped_;
    std::deque<LinkTarget> queue_;
};

}}}
