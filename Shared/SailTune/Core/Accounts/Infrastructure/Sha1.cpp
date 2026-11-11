// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "Sha1.h"
#include <cstring>

namespace nordtile { namespace music { namespace accounts { namespace infrastructure {
namespace {
inline std::uint32_t rol(std::uint32_t v, int n) { return (v << n) | (v >> (32 - n)); }
}

Sha1::Sha1() : buffered_(0), total_(0) {
    h_[0] = 0x67452301u; h_[1] = 0xEFCDAB89u; h_[2] = 0x98BADCFEu; h_[3] = 0x10325476u; h_[4] = 0xC3D2E1F0u;
    std::memset(buffer_, 0, sizeof(buffer_));
}

void Sha1::block(const unsigned char *p) {
    std::uint32_t w[80];
    for (int i = 0; i < 16; ++i)
        w[i] = (std::uint32_t(p[4 * i]) << 24) | (std::uint32_t(p[4 * i + 1]) << 16) |
               (std::uint32_t(p[4 * i + 2]) << 8) | std::uint32_t(p[4 * i + 3]);
    for (int i = 16; i < 80; ++i) w[i] = rol(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
    std::uint32_t a = h_[0], b = h_[1], c = h_[2], d = h_[3], e = h_[4];
    for (int i = 0; i < 80; ++i) {
        std::uint32_t f, k;
        if (i < 20) { f = (b & c) | (~b & d); k = 0x5A827999u; }
        else if (i < 40) { f = b ^ c ^ d; k = 0x6ED9EBA1u; }
        else if (i < 60) { f = (b & c) | (b & d) | (c & d); k = 0x8F1BBCDCu; }
        else { f = b ^ c ^ d; k = 0xCA62C1D6u; }
        const std::uint32_t t = rol(a, 5) + f + e + k + w[i];
        e = d; d = c; c = rol(b, 30); b = a; a = t;
    }
    h_[0] += a; h_[1] += b; h_[2] += c; h_[3] += d; h_[4] += e;
    volatile std::uint32_t *scrub = w;
    for (int i = 0; i < 80; ++i) scrub[i] = 0;
}

void Sha1::update(const void *data, std::size_t size) {
    const unsigned char *p = static_cast<const unsigned char *>(data);
    total_ += size;
    while (size > 0) {
        const std::size_t take = (64 - buffered_) < size ? (64 - buffered_) : size;
        std::memcpy(buffer_ + buffered_, p, take);
        buffered_ += take; p += take; size -= take;
        if (buffered_ == 64) { block(buffer_); buffered_ = 0; }
    }
}

void Sha1::digest(unsigned char out[20]) {
    const std::uint64_t bits = total_ * 8u;
    const unsigned char pad = 0x80;
    update(&pad, 1);
    const unsigned char zero = 0;
    while (buffered_ != 56) update(&zero, 1);
    unsigned char length[8];
    for (int i = 0; i < 8; ++i) length[i] = static_cast<unsigned char>(bits >> (56 - 8 * i));
    update(length, 8);
    for (int i = 0; i < 5; ++i) {
        out[4 * i] = static_cast<unsigned char>(h_[i] >> 24);
        out[4 * i + 1] = static_cast<unsigned char>(h_[i] >> 16);
        out[4 * i + 2] = static_cast<unsigned char>(h_[i] >> 8);
        out[4 * i + 3] = static_cast<unsigned char>(h_[i]);
    }
    volatile unsigned char *scrub = buffer_;
    for (std::size_t i = 0; i < sizeof(buffer_); ++i) scrub[i] = 0;
}

std::string Sha1::toHex(const unsigned char *bytes, std::size_t size) {
    static const char digits[] = "0123456789abcdef";
    std::string out; out.reserve(size * 2);
    for (std::size_t i = 0; i < size; ++i) { out.push_back(digits[bytes[i] >> 4]); out.push_back(digits[bytes[i] & 15]); }
    return out;
}

std::string Sha1::hex(const std::string &input) {
    Sha1 hash; hash.update(input);
    unsigned char out[20]; hash.digest(out);
    return toHex(out, 20);
}

}}}}
