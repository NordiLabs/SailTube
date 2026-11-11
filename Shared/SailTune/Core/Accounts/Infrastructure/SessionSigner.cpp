// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "SessionSigner.h"
#include "Sha1.h"
#include "../Domain/SessionTypes.h"

namespace nordtile { namespace music { namespace accounts { namespace infrastructure {
namespace {
SigningResult fail(const char *why) { SigningResult r; r.ok = false; r.error = why; return r; }

bool validSessionId(const std::string &id) {
    if (id.empty() || id.size() > 64) return false;
    for (std::size_t i = 0; i < id.size(); ++i) {
        const char c = id[i];
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_' || c == '-')) return false;
    }
    return true;
}
bool cookieOk(const std::string &value) { return value.empty() || domain::validCookieValue(value); }

std::string decimal(std::int64_t v) {
    if (v == 0) return "0";
    std::string out; bool neg = v < 0; std::uint64_t u = neg ? std::uint64_t(-(v + 1)) + 1u : std::uint64_t(v);
    while (u) { out.insert(out.begin(), char('0' + (u % 10))); u /= 10; }
    if (neg) out.insert(out.begin(), '-');
    return out;
}
}

SigningResult::~SigningResult() { domain::wipeString(value); }
void SigningInput::wipe() {
    domain::wipeString(sapisid); domain::wipeString(secure1Papisid); domain::wipeString(secure3Papisid);
    domain::wipeString(userSessionId);
}

bool SessionSigner::validOrigin(const std::string &origin) {
    static const char prefix[] = "https://";
    if (origin.size() <= sizeof(prefix) - 1 || origin.size() > 128) return false;
    if (origin.compare(0, sizeof(prefix) - 1, prefix) != 0) return false;
    for (std::size_t i = sizeof(prefix) - 1; i < origin.size(); ++i) {
        const char c = origin[i];
        const bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '.' || c == '-' || c == ':';
        if (!ok) return false;
    }
    return true;
}

std::string SessionSigner::hash(const std::string &timestamp, const std::string &cookie,
                                const std::string &origin, const std::string &userSessionId) {
    Sha1 sha;
    if (!userSessionId.empty()) { sha.update(userSessionId); sha.update(" ", 1); }
    sha.update(timestamp); sha.update(" ", 1); sha.update(cookie); sha.update(" ", 1); sha.update(origin);
    unsigned char out[20]; sha.digest(out);
    return Sha1::toHex(out, 20);
}

const char *SessionSigner::versionName(SignerVersion version) {
    switch (version) {
    case SignerVersion::V1: return "v1";
    case SignerVersion::V2: return "v2";
    case SignerVersion::V3: return "v3";
    }
    return "v1";
}

SigningResult SessionSigner::sign(const SigningInput &in) {
    if (!validOrigin(in.origin)) return fail("The signing origin is not acceptable.");
    if (in.nowUtcSeconds <= 0) return fail("The clock is not available for signing.");
    if (in.extraMillis < 0 || in.extraMillis > 999) return fail("The clock is not available for signing.");
    if (!cookieOk(in.sapisid) || !cookieOk(in.secure1Papisid) || !cookieOk(in.secure3Papisid))
        return fail("A signing cookie is not acceptable.");

    std::string stamp;
    if (in.options.unit == TimestampUnit::Seconds) stamp = decimal(in.nowUtcSeconds);
    else stamp = decimal(in.nowUtcSeconds * 1000 + in.extraMillis);

    // V1/V3 key: __Secure-3PAPISID first (what ytmusicapi uses), SAPISID as the fallback.
    // INFERENCE: the order only matters if the two differ, which no source reports.
    const std::string &primary = !in.secure3Papisid.empty() ? in.secure3Papisid : in.sapisid;

    SigningResult out;
    switch (in.options.version) {
    case SignerVersion::V1:
        if (primary.empty()) return fail("No signing cookie is available.");
        out.value = "SAPISIDHASH " + stamp + "_" + hash(stamp, primary, in.origin);
        break;
    case SignerVersion::V3:
        if (primary.empty()) return fail("No signing cookie is available.");
        if (!validSessionId(in.userSessionId)) return fail("A user-session id is required for this signing version.");
        out.value = "SAPISIDHASH " + stamp + "_" + hash(stamp, primary, in.origin, in.userSessionId) + "_u";
        break;
    case SignerVersion::V2: {
        // SAPISIDHASH uses SAPISID (3P fallback), the others their own cookie. Absent cookies
        // are skipped; at least one token is required.
        const std::string &first = !in.sapisid.empty() ? in.sapisid : in.secure3Papisid;
        if (!first.empty()) out.value += "SAPISIDHASH " + stamp + "_" + hash(stamp, first, in.origin);
        if (!in.secure1Papisid.empty()) {
            if (!out.value.empty()) out.value += ' ';
            out.value += "SAPISID1PHASH " + stamp + "_" + hash(stamp, in.secure1Papisid, in.origin);
        }
        if (!in.secure3Papisid.empty()) {
            if (!out.value.empty()) out.value += ' ';
            out.value += "SAPISID3PHASH " + stamp + "_" + hash(stamp, in.secure3Papisid, in.origin);
        }
        if (out.value.empty()) return fail("No signing cookie is available.");
        break;
    }
    }
    out.ok = true;
    return out;
}

}}}}
