// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Versioned, line-based serialisation of a SessionBundle: the payload stored in the credential
// vault. Pure C++11. Bounded (a vault secret is limited to 64 KiB) and strict on read: a
// payload that is not exactly this format is refused, never half-trusted.
//
//   ytm-session-bundle v1\n
//   source=paste\n   imported=<unix seconds>\n   authuser=<digits>\n   pageid=<digits>\n
//   c.<cookie name>=<cookie value>\n          (allow-listed names only)
//
// Unknown keys are ignored (forward compatibility); unknown versions are refused.
#pragma once

#include "../Domain/SessionTypes.h"
#include <cstddef>
#include <string>

namespace nordtile { namespace music { namespace accounts { namespace infrastructure {

enum class BundleDecode { Ok, Empty, Malformed, UnsupportedVersion, TooLarge, Unusable };

class SessionBundleCodec {
public:
    enum { kMaxBytes = domain::SessionBundle::kMaxEncodedBytes };
    // False when the bundle is not usable() or would exceed kMaxBytes. `out` is SECRET.
    static bool encode(const domain::SessionBundle &bundle, std::string *out);
    static BundleDecode decode(const char *data, std::size_t size, domain::SessionBundle *out);
};

}}}}
