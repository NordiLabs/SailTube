// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#pragma once

// Comments (DSC-16), Qt-free: the comment shape, the per-request budgets, the grow-only reveal
// window, the auto-load policy and the session that applies parsed pages under those budgets.
//
// Reading of the spec used here (see the package report):
//  * A "request" is one user-level action that needs the network: loading the top-level list
//    (load, loadMore, retry of either) or one replies step on one thread (expand, "more
//    replies", retry of either).
//  * A top-level request walks at most 10 pages and adds at most 50 threads (the NFR-PERF-08
//    unit). A replies request adds at most 12 replies to its thread: it hands out buffered
//    replies first, else fetches the thread's next reply page (walking on only past empty
//    pages, at most 10). Whatever a page carries beyond the budget is kept in a buffer and
//    handed out first by the next request, so no row is lost and nothing is fetched twice.
//  * At most 150 replies are held in total per loaded list (video + sort); then no further
//    replies requests start.
//  * Errors are shown inline; a failed request is repeated only by an explicit retry. Scrolling
//    never starts a request while a failure is pending.

#include "Outcome.h"

#include <cstdint>
#include <string>
#include <vector>

namespace nordtile { namespace video { namespace catalog {

struct Comment {
    Comment() : likeCount(-1), publishedAgeSeconds(-1), creator(false), verified(false), pinned(false),
                isReply(false), replyCount(0) {}
    std::string id;                  // "Ugz..." (replies: "<thread id>.<reply id>")
    std::string key;                 // commentViewModel.commentKey (the entity join key)
    std::string author;              // display name ("@handle")
    std::string authorChannelId;     // UC id when present
    std::string avatarUrl;
    std::string text;
    std::int64_t likeCount;          // -1 when the text did not parse
    std::string likeText;            // raw ("1.3K"), shown when the count did not parse
    std::string publishedText;       // raw ("1 year ago", "2 days ago (edited)")
    std::int64_t publishedAgeSeconds;
    bool creator;                    // the video owner wrote it
    bool verified;
    bool pinned;
    bool isReply;
    std::int64_t replyCount;         // announced replies (0 when none, -1 unknown)
    std::string replyCountText;
    std::string replyContinuation;   // threads: token of the first replies page
    std::vector<Comment> replies;    // filled by the session, not by the page parser
};

enum class CommentSort { Top, Newest };

inline const char *commentSortName(CommentSort s) { return s == CommentSort::Newest ? "newest" : "top"; }
CommentSort commentSortFromName(const std::string &name);  // anything but "newest" is Top

struct SortOption {
    SortOption() : sort(CommentSort::Top), selected(false) {}
    CommentSort sort;
    std::string title;               // provider text ("Top", "Newest"), localized
    std::string token;
    bool selected;
};

// One parsed answer: a top-level page (threads) or a replies page (replies).
struct CommentsPage {
    CommentsPage() : outcome(Outcome::ShapeDrift), total(-1), missingBodies(0) {}
    Outcome outcome;
    std::vector<Comment> comments;
    std::string continuation;        // next page of the same list, "" at the end
    std::string totalText;           // header countText when it holds a number
    std::int64_t total;
    std::vector<SortOption> sorts;   // from the header's sort menu, when present
    int missingBodies;               // view models whose entity was absent (dropped)
};

// What watch-next says about the comments of a video.
struct CommentsEntry {
    CommentsEntry() : outcome(Outcome::ShapeDrift), total(-1), commentsOff(false) {}
    Outcome outcome;                 // Ok with a token; ValidEmpty when comments are off
    std::string token;               // first top-level page
    std::string totalText;           // "7.7K"
    std::int64_t total;
    std::vector<SortOption> sorts;
    bool commentsOff;
    std::string message;             // provider message when comments are off
};

struct CommentBudgets {
    CommentBudgets() : topLevel(50), pages(10), repliesPerThread(12), repliesTotal(150) {}
    int topLevel;          // threads added per top-level request
    int pages;             // pages walked per request
    int repliesPerThread;  // replies added per replies request on one thread
    int repliesTotal;      // replies held per loaded list
};

// Rows are revealed in steps and the window never shrinks while the list stays the same,
// which keeps the scrollbar from thrashing (DSC-16, FinTube lesson). Only a new list (another
// video, another sort) resets it.
class RevealWindow {
public:
    explicit RevealWindow(int step = 5) : step_(step > 0 ? step : 1), shown_(0) {}
    int step() const { return step_; }
    int shown() const { return shown_; }
    // Grows by one step, capped at `available`. Never shrinks, even if `available` is smaller.
    int grow(int available);
    void reset() { shown_ = 0; }
private:
    int step_;
    int shown_;
};

// Auto-load setting (DSC-16): Off, "When I scroll near" (default) or "Shortly after playback
// starts" (3 s after playback starts; scrolling near loads sooner). Loads at most once per
// video; an explicit load always works. QSettings values: "off", "near", "playback".
enum class AutoLoadMode { Off, Near, AfterPlayback };

AutoLoadMode autoLoadModeFromName(const std::string &name);  // unknown -> Near
const char *autoLoadModeName(AutoLoadMode mode);

class AutoLoadPolicy {
public:
    static const std::int64_t kPlaybackDelayMs = 3000;

    explicit AutoLoadPolicy(AutoLoadMode mode = AutoLoadMode::Near) : mode_(mode), done_(false), deadline_(-1) {}
    AutoLoadMode mode() const { return mode_; }
    void setMode(AutoLoadMode mode) { mode_ = mode; if (mode_ != AutoLoadMode::AfterPlayback) deadline_ = -1; }
    // A new video: armed again, no deadline.
    void reset() { done_ = false; deadline_ = -1; }
    // Something (auto or explicit) started the load: no further automatic load.
    void markLoaded() { done_ = true; deadline_ = -1; }
    bool loaded() const { return done_; }

    // The section came within one screen of the viewport: true when a load should start now.
    bool onScrolledNear();
    // Playback started at `nowMs`: returns the delay to wait before calling onTimer, or -1
    // when nothing is scheduled.
    std::int64_t onPlaybackStarted(std::int64_t nowMs);
    // Timer fired at `nowMs`: true when the load is due now.
    bool onTimer(std::int64_t nowMs);
    // Remaining wait at `nowMs` (-1 when nothing is scheduled).
    std::int64_t remainingMs(std::int64_t nowMs) const;
private:
    AutoLoadMode mode_;
    bool done_;
    std::int64_t deadline_;
};

// One loaded list (video + sort): threads with their replies, the budgets and the failure
// gate. The presentation model drives the network; this class decides.
class CommentsSession {
public:
    explicit CommentsSession(const CommentBudgets &budgets = CommentBudgets());

    const CommentBudgets &budgets() const { return budgets_; }
    void reset(const std::string &firstToken);

    const std::vector<Comment> &threads() const { return threads_; }
    const Comment &thread(std::size_t i) const { return threads_[i]; }
    std::size_t threadCount() const { return threads_.size(); }
    int totalReplies() const { return totalReplies_; }
    int pagesFetched() const { return pagesFetched_; }
    int bufferedThreads() const { return static_cast<int>(threadBuffer_.size()); }

    // --- top level ---
    // More threads exist (buffered or behind a token).
    bool canRequestMore() const { return !threadBuffer_.empty() || !token_.empty(); }
    const std::string &nextToken() const { return token_; }
    // Starts a top-level request; buffered threads are handed out first. Returns how many.
    int beginRequest();
    // Applies one fetched page to the running request. Returns the threads added.
    int applyThreadPage(const CommentsPage &page);
    // The running request should fetch another page.
    bool requestNeedsAnotherPage() const;
    int requestAdded() const { return requestAdded_; }

    // --- replies ---
    bool canLoadMoreReplies(std::size_t thread) const;
    std::string replyToken(std::size_t thread) const;
    int bufferedReplies(std::size_t thread) const;
    // Starts a replies request on `thread`; buffered replies are handed out first.
    int beginReplyRequest(std::size_t thread);
    int applyReplyPage(std::size_t thread, const CommentsPage &page);
    bool replyRequestNeedsAnotherPage(std::size_t thread) const;

    // --- failures: never retried automatically ---
    void markFailed() { failed_ = true; }
    void clearFailure() { failed_ = false; }
    bool failed() const { return failed_; }
    // Scrolling may start a request only while no failure is pending.
    bool scrollMayFetch() const { return !failed_; }

private:
    struct ThreadState {
        ThreadState() : requestAdded(0), requestPages(0) {}
        std::string token;
        std::vector<Comment> buffer;
        int requestAdded;
        int requestPages;
    };
    int replyRoom(std::size_t thread) const;
    bool knownThread(const std::string &id) const;
    bool knownReply(std::size_t thread, const std::string &id) const;

    CommentBudgets budgets_;
    std::vector<Comment> threads_;
    std::vector<ThreadState> states_;
    std::vector<Comment> threadBuffer_;
    std::string token_;
    int requestAdded_;
    int requestPages_;
    int pagesFetched_;
    int totalReplies_;
    bool failed_;
};

}}}
