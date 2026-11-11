// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "Comments.h"

#include <algorithm>

namespace nordtile { namespace video { namespace catalog {

const std::int64_t AutoLoadPolicy::kPlaybackDelayMs;

CommentSort commentSortFromName(const std::string &name) {
    return name == "newest" ? CommentSort::Newest : CommentSort::Top;
}

AutoLoadMode autoLoadModeFromName(const std::string &name) {
    if (name == "off") return AutoLoadMode::Off;
    if (name == "playback") return AutoLoadMode::AfterPlayback;
    return AutoLoadMode::Near;
}

const char *autoLoadModeName(AutoLoadMode mode) {
    switch (mode) {
    case AutoLoadMode::Off: return "off";
    case AutoLoadMode::Near: return "near";
    case AutoLoadMode::AfterPlayback: return "playback";
    }
    return "near";
}

int RevealWindow::grow(int available) {
    if (available > shown_) shown_ = std::min(available, shown_ + step_);
    return shown_;
}

bool AutoLoadPolicy::onScrolledNear() {
    if (done_ || mode_ == AutoLoadMode::Off) return false;
    // Near: this is the trigger. AfterPlayback: scrolling near loads sooner than the timer.
    return true;
}

std::int64_t AutoLoadPolicy::onPlaybackStarted(std::int64_t nowMs) {
    if (done_ || mode_ != AutoLoadMode::AfterPlayback) return -1;
    if (deadline_ < 0) deadline_ = nowMs + kPlaybackDelayMs;
    return std::max<std::int64_t>(0, deadline_ - nowMs);
}

bool AutoLoadPolicy::onTimer(std::int64_t nowMs) {
    if (done_ || mode_ != AutoLoadMode::AfterPlayback || deadline_ < 0) return false;
    return nowMs >= deadline_;
}

std::int64_t AutoLoadPolicy::remainingMs(std::int64_t nowMs) const {
    if (done_ || deadline_ < 0) return -1;
    return std::max<std::int64_t>(0, deadline_ - nowMs);
}

CommentsSession::CommentsSession(const CommentBudgets &budgets)
    : budgets_(budgets), requestAdded_(0), requestPages_(0), pagesFetched_(0), totalReplies_(0), failed_(false) {}

void CommentsSession::reset(const std::string &firstToken) {
    threads_.clear();
    states_.clear();
    threadBuffer_.clear();
    token_ = firstToken;
    requestAdded_ = 0;
    requestPages_ = 0;
    pagesFetched_ = 0;
    totalReplies_ = 0;
    failed_ = false;
}

bool CommentsSession::knownThread(const std::string &id) const {
    if (id.empty()) return false;
    for (const Comment &c : threads_) if (c.id == id) return true;
    for (const Comment &c : threadBuffer_) if (c.id == id) return true;
    return false;
}

bool CommentsSession::knownReply(std::size_t thread, const std::string &id) const {
    if (id.empty()) return false;
    for (const Comment &c : threads_[thread].replies) if (c.id == id) return true;
    for (const Comment &c : states_[thread].buffer) if (c.id == id) return true;
    return false;
}

int CommentsSession::beginRequest() {
    requestAdded_ = 0;
    requestPages_ = 0;
    while (!threadBuffer_.empty() && requestAdded_ < budgets_.topLevel) {
        threads_.push_back(threadBuffer_.front());
        threads_.back().replies.clear();
        states_.push_back(ThreadState());
        states_.back().token = threads_.back().replyContinuation;
        threadBuffer_.erase(threadBuffer_.begin());
        ++requestAdded_;
    }
    return requestAdded_;
}

int CommentsSession::applyThreadPage(const CommentsPage &page) {
    ++requestPages_;
    ++pagesFetched_;
    token_ = page.continuation;
    int added = 0;
    for (const Comment &c : page.comments) {
        if (knownThread(c.id)) continue;
        if (requestAdded_ < budgets_.topLevel) {
            threads_.push_back(c);
            threads_.back().replies.clear();
            states_.push_back(ThreadState());
            states_.back().token = c.replyContinuation;
            ++requestAdded_;
            ++added;
        } else {
            threadBuffer_.push_back(c);
        }
    }
    return added;
}

bool CommentsSession::requestNeedsAnotherPage() const {
    return requestAdded_ < budgets_.topLevel && requestPages_ < budgets_.pages && !token_.empty();
}

int CommentsSession::replyRoom(std::size_t thread) const {
    if (thread >= threads_.size()) return 0;
    const int perRequest = budgets_.repliesPerThread - states_[thread].requestAdded;
    const int total = budgets_.repliesTotal - totalReplies_;
    return std::max(0, std::min(perRequest, total));
}

bool CommentsSession::canLoadMoreReplies(std::size_t thread) const {
    if (thread >= threads_.size()) return false;
    if (totalReplies_ >= budgets_.repliesTotal) return false;
    return !states_[thread].buffer.empty() || !states_[thread].token.empty();
}

std::string CommentsSession::replyToken(std::size_t thread) const {
    return thread < states_.size() ? states_[thread].token : std::string();
}

int CommentsSession::bufferedReplies(std::size_t thread) const {
    return thread < states_.size() ? static_cast<int>(states_[thread].buffer.size()) : 0;
}

int CommentsSession::beginReplyRequest(std::size_t thread) {
    if (thread >= threads_.size()) return 0;
    ThreadState &st = states_[thread];
    st.requestAdded = 0;
    st.requestPages = 0;
    int added = 0;
    while (!st.buffer.empty() && replyRoom(thread) > 0) {
        threads_[thread].replies.push_back(st.buffer.front());
        st.buffer.erase(st.buffer.begin());
        ++st.requestAdded;
        ++totalReplies_;
        ++added;
    }
    return added;
}

int CommentsSession::applyReplyPage(std::size_t thread, const CommentsPage &page) {
    if (thread >= threads_.size()) return 0;
    ThreadState &st = states_[thread];
    ++st.requestPages;
    ++pagesFetched_;
    st.token = page.continuation;
    int added = 0;
    for (const Comment &c : page.comments) {
        if (knownReply(thread, c.id)) continue;
        Comment reply = c;
        reply.isReply = true;
        if (replyRoom(thread) > 0) {
            threads_[thread].replies.push_back(reply);
            ++st.requestAdded;
            ++totalReplies_;
            ++added;
        } else {
            st.buffer.push_back(reply);
        }
    }
    return added;
}

bool CommentsSession::replyRequestNeedsAnotherPage(std::size_t thread) const {
    if (thread >= threads_.size()) return false;
    const ThreadState &st = states_[thread];
    // One page per tap: walk on only past pages that added nothing (empty pages).
    return st.requestAdded == 0 && replyRoom(thread) > 0 && st.requestPages < budgets_.pages && !st.token.empty();
}

}}}
