// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#pragma once
#include <cstddef>
#include <cstdint>
#include <string>

namespace nordtile { namespace music { namespace accounts { namespace infrastructure {

// SHA-1 (FIPS 180-4), pure C++11. Used ONLY because the documented per-request authorization
// header of the web client is a SHA-1 construction; it is not a security primitive of this
// application. Tested against the FIPS vectors and against Python hashlib (tests/ytm_session).
class Sha1 {
public:
    Sha1();
    void update(const void *data, std::size_t size);
    void update(const std::string &text) { update(text.data(), text.size()); }
    // Finishes the computation (the object must not be reused) and returns 20 bytes.
    void digest(unsigned char out[20]);
    // Convenience: lower-case hex of SHA-1(input).
    static std::string hex(const std::string &input);
    static std::string toHex(const unsigned char *bytes, std::size_t size);
private:
    void block(const unsigned char *chunk);
    std::uint32_t h_[5];
    unsigned char buffer_[64];
    std::size_t buffered_;
    std::uint64_t total_;
};

}}}}
