// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#pragma once
// Incremental HTTP/1.1 response parser (status line, headers, Content-Length / chunked / until
// close). Qt-free so that it can be tested with recorded wire bytes. Bounded: a header block
// larger than maxHeaderBytes or a body larger than maxBodyBytes is an error, never a growth.
#include <cstddef>
#include <cstdint>
#include <map>
#include <string>

namespace nordtile { namespace music { namespace sharedplayback { namespace domain {

class HttpResponseParser {
public:
    enum State { NeedMore, HeadersDone, Done, Error };

    // headersOnly: stop at HeadersDone and ignore the declared length (a probe of a large range).
    // streaming: the caller drains the body with takeBody() after every feed(); only the part not yet
    // taken counts against maxBodyBytes, and a declared length of any size is accepted.
    explicit HttpResponseParser(std::size_t maxBodyBytes = 2 * 1024 * 1024, std::size_t maxHeaderBytes = 64 * 1024,
                                bool headersOnly = false, bool streaming = false)
        : maxBody_(maxBodyBytes), maxHeader_(maxHeaderBytes), headersOnly_(headersOnly), streaming_(streaming) {}

    // Feeds received bytes. Returns HeadersDone once, when the header block has just been read
    // (a caller that wants only the headers can stop there), then keeps returning HeadersDone
    // until the body is complete (Done) or the message is bad (Error).
    State feed(const char *data, std::size_t size) {
        if (state_ == Error || state_ == Done) return state_;
        buffer_.append(data, size);
        for (;;) {
            if (!headersRead_) {
                const std::size_t end = buffer_.find("\r\n\r\n");
                if (end == std::string::npos) {
                    if (buffer_.size() > maxHeader_) return fail();
                    return NeedMore;
                }
                if (end > maxHeader_ || !parseHeaders(buffer_.substr(0, end))) return fail();
                buffer_.erase(0, end + 4);
                // An interim response (100 Continue, 103 Early Hints) is not the answer: the final one follows.
                if (status_ >= 100 && status_ < 200 && status_ != 101) {
                    if (++interim_ > 8) return fail();
                    headers_.clear(); status_ = 0; contentLength_ = 0; chunked_ = false; untilClose_ = false;
                    continue;
                }
                headersRead_ = true;
                state_ = HeadersDone;
                if (headersOnly_) return state_;
                if (noBody()) { state_ = Done; return state_; }
                if (!chunked_ && !untilClose_ && contentLength_ == 0) { state_ = Done; return state_; }
            }
            if (chunked_) {
                const int r = readChunks();
                if (r < 0) return fail();
                if (r == 1) { state_ = Done; return state_; }
                return state_;
            }
            if (untilClose_) {
                if (body_.size() + buffer_.size() > maxBody_) return fail();
                body_.append(buffer_); buffer_.clear();
                return state_;
            }
            const std::uint64_t need = contentLength_ - received_;
            const std::size_t take = buffer_.size() < need ? buffer_.size() : std::size_t(need);
            if (body_.size() + take > maxBody_) return fail();
            body_.append(buffer_, 0, take);
            received_ += take;
            buffer_.erase(0, take);
            if (received_ == contentLength_) { state_ = Done; return state_; }
            return state_;
        }
    }

    // The peer closed the connection: complete for a body that runs until close, an error otherwise.
    State closed() {
        if (state_ == Done || state_ == Error) return state_;
        if (headersRead_ && untilClose_) { state_ = Done; return state_; }
        return fail();
    }

    int statusCode() const { return status_; }
    // Moves out the body received so far (streaming use).
    std::string takeBody() { std::string out; out.swap(body_); return out; }
    const std::map<std::string, std::string> &headers() const { return headers_; }
    bool headersComplete() const { return headersRead_; }
    const std::string &body() const { return body_; }
    // Lower-case name; empty when absent.
    std::string header(const std::string &lowerName) const {
        std::map<std::string, std::string>::const_iterator it = headers_.find(lowerName);
        return it == headers_.end() ? std::string() : it->second;
    }
    bool keepAlive() const { return keepAlive_ && !untilClose_; }
    std::uint64_t contentLength() const { return contentLength_; }

private:
    State fail() { state_ = Error; return state_; }
    int interim_ = 0;
    bool noBody() const { return status_ == 204 || status_ == 304 || (status_ >= 100 && status_ < 200); }

    static std::string lower(std::string s) {
        for (std::size_t i = 0; i < s.size(); ++i) if (s[i] >= 'A' && s[i] <= 'Z') s[i] = char(s[i] - 'A' + 'a');
        return s;
    }
    static std::string trim(const std::string &s) {
        std::size_t a = 0, b = s.size();
        while (a < b && (s[a] == ' ' || s[a] == '\t')) ++a;
        while (b > a && (s[b - 1] == ' ' || s[b - 1] == '\t')) --b;
        return s.substr(a, b - a);
    }

    bool parseHeaders(const std::string &block) {
        std::size_t lineEnd = block.find("\r\n");
        const std::string statusLine = block.substr(0, lineEnd);
        // "HTTP/1.1 206 Partial Content"
        if (statusLine.compare(0, 5, "HTTP/") != 0) return false;
        const std::size_t sp = statusLine.find(' ');
        if (sp == std::string::npos || statusLine.size() < sp + 4) return false;
        status_ = 0;
        for (std::size_t i = sp + 1; i < sp + 4; ++i) {
            if (statusLine[i] < '0' || statusLine[i] > '9') return false;
            status_ = status_ * 10 + (statusLine[i] - '0');
        }
        keepAlive_ = statusLine.compare(0, 8, "HTTP/1.1") == 0;
        std::size_t pos = lineEnd == std::string::npos ? block.size() : lineEnd + 2;
        while (pos < block.size()) {
            std::size_t next = block.find("\r\n", pos);
            if (next == std::string::npos) next = block.size();
            const std::string line = block.substr(pos, next - pos);
            pos = next + 2;
            const std::size_t colon = line.find(':');
            if (colon == std::string::npos || colon == 0) return false;
            headers_[lower(trim(line.substr(0, colon)))] = trim(line.substr(colon + 1));
        }
        const std::string connection = lower(header("connection"));
        if (connection.find("close") != std::string::npos) keepAlive_ = false;
        else if (connection.find("keep-alive") != std::string::npos) keepAlive_ = true;
        const std::string encoding = lower(header("transfer-encoding"));
        chunked_ = encoding.find("chunked") != std::string::npos;
        const std::string length = header("content-length");
        if (!chunked_ && !length.empty()) {
            std::uint64_t value = 0;
            for (std::size_t i = 0; i < length.size(); ++i) {
                if (length[i] < '0' || length[i] > '9') return false;
                if (value > (std::uint64_t(1) << 50)) return false;
                value = value * 10 + std::uint64_t(length[i] - '0');
            }
            if (value > maxBody_ && !headersOnly_ && !streaming_) return false;
            contentLength_ = value;
        } else if (!chunked_ && !noBody()) {
            untilClose_ = true;
        }
        return true;
    }

    // -1 error, 0 more needed, 1 finished.
    int readChunks() {
        for (;;) {
            if (chunkState_ == ChunkSize) {
                const std::size_t eol = buffer_.find("\r\n");
                if (eol == std::string::npos) return buffer_.size() > 1024 ? -1 : 0;
                std::string size = buffer_.substr(0, eol);
                const std::size_t semi = size.find(';');
                if (semi != std::string::npos) size.erase(semi);
                size = trim(size);
                if (size.empty() || size.size() > 8) return -1;
                std::uint64_t value = 0;
                for (std::size_t i = 0; i < size.size(); ++i) {
                    const char c = size[i];
                    int digit;
                    if (c >= '0' && c <= '9') digit = c - '0';
                    else if (c >= 'a' && c <= 'f') digit = c - 'a' + 10;
                    else if (c >= 'A' && c <= 'F') digit = c - 'A' + 10;
                    else return -1;
                    value = value * 16 + std::uint64_t(digit);
                }
                buffer_.erase(0, eol + 2);
                if (value == 0) { chunkState_ = ChunkTrailers; continue; }
                if (body_.size() + value > maxBody_) return -1;
                chunkRemaining_ = value;
                chunkState_ = ChunkData;
            } else if (chunkState_ == ChunkData) {
                const std::size_t take = buffer_.size() < chunkRemaining_ ? buffer_.size() : std::size_t(chunkRemaining_);
                body_.append(buffer_, 0, take);
                buffer_.erase(0, take);
                chunkRemaining_ -= take;
                if (chunkRemaining_ > 0) return 0;
                chunkState_ = ChunkDataEnd;
            } else if (chunkState_ == ChunkDataEnd) {
                if (buffer_.size() < 2) return 0;
                if (buffer_.compare(0, 2, "\r\n") != 0) return -1;
                buffer_.erase(0, 2);
                chunkState_ = ChunkSize;
            } else {   // trailers: lines up to the empty one, all ignored
                for (;;) {
                    const std::size_t eol = buffer_.find("\r\n");
                    if (eol == std::string::npos) return buffer_.size() > 8192 ? -1 : 0;
                    const bool empty = eol == 0;
                    buffer_.erase(0, eol + 2);
                    if (empty) return 1;
                }
            }
        }
    }

    enum ChunkState { ChunkSize, ChunkData, ChunkDataEnd, ChunkTrailers };
    std::size_t maxBody_, maxHeader_;
    bool headersOnly_ = false;
    bool streaming_ = false;
    ChunkState chunkState_ = ChunkSize;
    std::string buffer_, body_;
    std::map<std::string, std::string> headers_;
    int status_ = 0;
    std::uint64_t contentLength_ = 0;
    std::uint64_t received_ = 0;       // body bytes of a Content-Length message seen so far
    std::uint64_t chunkRemaining_ = 0;
    bool headersRead_ = false, chunked_ = false, untilClose_ = false, keepAlive_ = true;
    State state_ = NeedMore;
};

}}}}
