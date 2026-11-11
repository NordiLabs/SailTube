// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "SearchFilter.h"

#include <cstddef>

namespace nordtile { namespace video { namespace catalog {

namespace {

const char kAlphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

void putVarint(std::vector<std::uint8_t> &out, std::uint64_t v) {
    while (v >= 0x80) { out.push_back(static_cast<std::uint8_t>((v & 0x7f) | 0x80)); v >>= 7; }
    out.push_back(static_cast<std::uint8_t>(v));
}

void putField(std::vector<std::uint8_t> &out, int field, std::uint64_t value) {
    putVarint(out, static_cast<std::uint64_t>(field) << 3);   // wire type 0 (varint)
    putVarint(out, value);
}

bool readVarint(const std::vector<std::uint8_t> &in, std::size_t &pos, std::size_t end, std::uint64_t *v) {
    *v = 0;
    for (int shift = 0; shift < 64; shift += 7) {
        if (pos >= end) return false;
        const std::uint8_t b = in[pos++];
        *v |= static_cast<std::uint64_t>(b & 0x7f) << shift;
        if (!(b & 0x80)) return true;
    }
    return false;
}

int base64Value(char c) {
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+' || c == '-') return 62;   // standard and URL-safe alphabets
    if (c == '/' || c == '_') return 63;
    return -1;
}

int hexValue(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

}  // namespace

std::string SearchFilter::key() const {
    std::string k;
    k += 's'; k += std::to_string(static_cast<int>(sort));
    k += 'd'; k += std::to_string(static_cast<int>(uploadDate));
    k += 't'; k += std::to_string(static_cast<int>(type));
    k += 'l'; k += std::to_string(static_cast<int>(duration));
    k += 'v'; k += live ? '1' : '0';
    return k;
}

SearchFilter effectiveSearchFilter(const SearchFilter &filter, bool mixed) {
    SearchFilter f = filter;
    if (!mixed && f.type == ResultTypeFilter::Any) f.type = ResultTypeFilter::Video;
    return f;
}

std::vector<std::uint8_t> encodeSearchFilterBytes(const SearchFilter &filter) {
    std::vector<std::uint8_t> out;
    if (filter.sort != SearchSort::Relevance) putField(out, 1, static_cast<std::uint64_t>(filter.sort));
    std::vector<std::uint8_t> sub;
    if (filter.uploadDate != UploadDateFilter::Any) putField(sub, 1, static_cast<std::uint64_t>(filter.uploadDate));
    if (filter.type != ResultTypeFilter::Any) putField(sub, 2, static_cast<std::uint64_t>(filter.type));
    if (filter.duration != DurationFilter::Any) putField(sub, 3, static_cast<std::uint64_t>(filter.duration));
    if (filter.live) putField(sub, 8, 1);
    if (!sub.empty()) {
        putVarint(out, (2u << 3) | 2u);                         // field 2, length-delimited
        putVarint(out, sub.size());
        out.insert(out.end(), sub.begin(), sub.end());
    }
    return out;
}

std::string searchParams(const SearchFilter &filter) { return sp::base64Encode(encodeSearchFilterBytes(filter)); }

std::string searchSp(const SearchFilter &filter) { return sp::percentEncode(searchParams(filter)); }

bool decodeSearchParams(const std::string &spOrBase64, SearchFilter *out) {
    std::vector<std::uint8_t> bytes;
    if (!sp::base64Decode(sp::percentDecode(spOrBase64), &bytes)) return false;
    SearchFilter f;
    std::size_t pos = 0;
    while (pos < bytes.size()) {
        std::uint64_t tag = 0;
        if (!readVarint(bytes, pos, bytes.size(), &tag)) return false;
        const std::uint64_t field = tag >> 3, wire = tag & 7;
        if (field == 1 && wire == 0) {
            std::uint64_t v = 0;
            if (!readVarint(bytes, pos, bytes.size(), &v) || v < 1 || v > 3) return false;
            f.sort = static_cast<SearchSort>(v);
        } else if (field == 2 && wire == 2) {
            std::uint64_t len = 0;
            if (!readVarint(bytes, pos, bytes.size(), &len) || len > bytes.size() - pos) return false;
            const std::size_t end = pos + static_cast<std::size_t>(len);
            while (pos < end) {
                std::uint64_t t = 0, v = 0;
                if (!readVarint(bytes, pos, end, &t) || (t & 7) != 0 || !readVarint(bytes, pos, end, &v)) return false;
                switch (t >> 3) {
                case 1: if (v < 1 || v > 5) return false; f.uploadDate = static_cast<UploadDateFilter>(v); break;
                case 2: if (v < 1 || v > 3) return false; f.type = static_cast<ResultTypeFilter>(v); break;
                case 3: if (v < 1 || v > 3) return false; f.duration = static_cast<DurationFilter>(v); break;
                case 8: if (v != 1) return false; f.live = true; break;
                default: return false;
                }
            }
        } else {
            return false;
        }
    }
    if (out) *out = f;
    return true;
}

std::string sp::base64Encode(const std::vector<std::uint8_t> &bytes) {
    std::string out;
    out.reserve((bytes.size() + 2) / 3 * 4);
    std::size_t i = 0;
    for (; i + 2 < bytes.size(); i += 3) {
        const std::uint32_t n = (static_cast<std::uint32_t>(bytes[i]) << 16) | (static_cast<std::uint32_t>(bytes[i + 1]) << 8) | bytes[i + 2];
        out += kAlphabet[(n >> 18) & 63]; out += kAlphabet[(n >> 12) & 63];
        out += kAlphabet[(n >> 6) & 63]; out += kAlphabet[n & 63];
    }
    const std::size_t rest = bytes.size() - i;
    if (rest == 1) {
        const std::uint32_t n = static_cast<std::uint32_t>(bytes[i]) << 16;
        out += kAlphabet[(n >> 18) & 63]; out += kAlphabet[(n >> 12) & 63]; out += "==";
    } else if (rest == 2) {
        const std::uint32_t n = (static_cast<std::uint32_t>(bytes[i]) << 16) | (static_cast<std::uint32_t>(bytes[i + 1]) << 8);
        out += kAlphabet[(n >> 18) & 63]; out += kAlphabet[(n >> 12) & 63]; out += kAlphabet[(n >> 6) & 63]; out += '=';
    }
    return out;
}

bool sp::base64Decode(const std::string &text, std::vector<std::uint8_t> *out) {
    std::vector<std::uint8_t> bytes;
    std::uint32_t acc = 0;
    int bits = 0;
    bool padding = false;
    for (char c : text) {
        if (c == '=') { padding = true; continue; }
        if (padding) return false;                 // data after padding
        const int v = base64Value(c);
        if (v < 0) return false;
        acc = (acc << 6) | static_cast<std::uint32_t>(v);
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            bytes.push_back(static_cast<std::uint8_t>((acc >> bits) & 0xff));
        }
    }
    if (out) *out = bytes;
    return true;
}

std::string sp::percentEncode(const std::string &text) {
    static const char hex[] = "0123456789ABCDEF";
    std::string out;
    for (unsigned char c : text) {
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' || c == '~') {
            out += static_cast<char>(c);
        } else {
            out += '%'; out += hex[c >> 4]; out += hex[c & 15];
        }
    }
    return out;
}

std::string sp::percentDecode(const std::string &text) {
    std::string out;
    for (std::size_t i = 0; i < text.size(); ++i) {
        if (text[i] == '%' && i + 2 < text.size()) {
            const int hi = hexValue(text[i + 1]), lo = hexValue(text[i + 2]);
            if (hi >= 0 && lo >= 0) { out += static_cast<char>(hi * 16 + lo); i += 2; continue; }
        }
        out += text[i];
    }
    return out;
}

}}}
