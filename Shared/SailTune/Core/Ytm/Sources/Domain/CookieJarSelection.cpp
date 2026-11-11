// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "CookieJarSelection.h"
#include <map>

namespace nordtile { namespace music { namespace ytm { namespace sources { namespace domain {

namespace {

// SPEC order first (minus "PREF", which the session allow-list does not carry), then the rest of
// the allow-list. A test pins "every entry is allowed" and "every allowed name is here".
const char *const kOrder[] = {
    "__Secure-3PAPISID", "SAPISID", "__Secure-1PAPISID",
    "__Secure-3PSID", "__Secure-1PSID",
    "SID", "HSID", "SSID", "APISID",
    "SIDCC", "__Secure-3PSIDCC", "__Secure-1PSIDCC",
    "LOGIN_INFO", "VISITOR_INFO1_LIVE",
    "__Secure-3PSIDTS", "__Secure-1PSIDTS",
    "VISITOR_PRIVACY_METADATA", "SOCS", "CONSENT", "__Secure-ROLLOUT_TOKEN", "YSC"
};
const std::size_t kOrderCount = sizeof(kOrder) / sizeof(kOrder[0]);

std::string lower(const std::string &s) {
    std::string out(s);
    for (std::size_t i = 0; i < out.size(); ++i) if (out[i] >= 'A' && out[i] <= 'Z') out[i] = char(out[i] - 'A' + 'a');
    return out;
}

bool digits(const std::string &v, std::size_t minLen, std::size_t maxLen) {
    if (v.size() < minLen || v.size() > maxLen) return false;
    for (std::size_t i = 0; i < v.size(); ++i) if (v[i] < '0' || v[i] > '9') return false;
    return true;
}

// true when `a` is a better pick than `b` for the same cookie name
bool better(const ports::RawCookieRow &a, int rankA, const ports::RawCookieRow &b, int rankB, std::int64_t expA, std::int64_t expB) {
    if (rankA != rankB) return rankA < rankB;
    const bool plainA = a.originAttributes.empty(), plainB = b.originAttributes.empty();
    if (plainA != plainB) return plainA;
    if (expA != expB) return expA > expB;
    if (a.lastAccessed != b.lastAccessed) return a.lastAccessed > b.lastAccessed;
    return false;   // equal: the earlier row stays (deterministic for a given row order)
}

}  // namespace

void SelectionResult::wipe() {
    accounts::domain::wipeString(text);
    names.clear();
    gateName.clear();
}

std::size_t CookieJarSelection::orderSize() { return kOrderCount; }
const char *CookieJarSelection::orderAt(std::size_t index) { return index < kOrderCount ? kOrder[index] : ""; }

bool CookieJarSelection::isGateCookie(const std::string &name) {
    return name == "__Secure-3PAPISID" || name == "SAPISID";
}

int CookieJarSelection::hostRank(const std::string &hostIn) {
    if (hostIn.empty() || hostIn.size() > 255) return -1;
    const std::string host = lower(hostIn);
    // Gecko writes a domain cookie with a leading dot and a host-only cookie without one. A request to
    // music.youtube.com carries: the domain cookies of youtube.com and of music.youtube.com, and the
    // host-only cookies of music.youtube.com itself. A host-only cookie of the youtube.com apex or of
    // www.youtube.com is NOT sent there, and a name merely ending in "youtube.com" is another site.
    if (host == ".youtube.com" || host == ".music.youtube.com" || host == "music.youtube.com") return 0;
    // The fallback side: sign-in cookies that only exist on google.com (the browser would not send them
    // to YouTube; SPEC "hosts ending youtube.com or google.com", preferring the YouTube row).
    if (host == ".google.com" || host == "google.com") return 1;
    return -1;
}

std::int64_t CookieJarSelection::normaliseExpiry(std::int64_t expiry) {
    if (expiry > 100000000000LL) return expiry / 1000;
    return expiry;
}

bool CookieJarSelection::validAuthUser(const std::string &v) { return digits(v, 1, 2); }
bool CookieJarSelection::validPageId(const std::string &v) { return digits(v, 1, 32); }

SelectionResult CookieJarSelection::compose(const std::vector<ports::RawCookieRow> &rows, int matchingRows,
                                            const SelectionOptions &options) {
    SelectionResult result;

    struct Pick { std::size_t row; int rank; std::int64_t expiry; };
    std::map<std::string, Pick> best;
    for (std::size_t i = 0; i < rows.size(); ++i) {
        const ports::RawCookieRow &row = rows[i];
        if (!accounts::domain::validCookieName(row.name) || !accounts::domain::isNeededCookie(row.name)) { ++result.skippedNotNeeded; continue; }
        const int rank = hostRank(row.host);
        if (rank < 0 || (rank == 1 && !options.allowGoogleFallback)) { ++result.skippedHost; continue; }
        if (row.path != "/") { ++result.skippedPath; continue; }
        const std::int64_t expiry = row.hasExpiry ? normaliseExpiry(row.expiry) : 0;
        if (options.nowUtcSeconds > 0 && row.hasExpiry && expiry > 0 && expiry <= options.nowUtcSeconds) { ++result.skippedExpired; continue; }
        if (!accounts::domain::validCookieValue(row.value)) { ++result.skippedInvalid; continue; }
        std::map<std::string, Pick>::iterator it = best.find(row.name);
        if (it == best.end()) {
            Pick p; p.row = i; p.rank = rank; p.expiry = expiry;
            best[row.name] = p;
        } else {
            ++result.duplicatesDropped;
            const ports::RawCookieRow &cur = rows[it->second.row];
            if (better(row, rank, cur, it->second.rank, expiry, it->second.expiry)) {
                it->second.row = i; it->second.rank = rank; it->second.expiry = expiry;
            }
        }
    }

    // The gate (SPEC): one of the two sign-in cookies. 3PAPISID is the hash secret and is named first.
    const char *gate = nullptr;
    if (best.find("__Secure-3PAPISID") != best.end()) gate = "__Secure-3PAPISID";
    else if (best.find("SAPISID") != best.end()) gate = "SAPISID";
    if (!gate) {
        result.status = (matchingRows > 0 || !rows.empty()) ? SelectionResult::Status::NotSignedIn : SelectionResult::Status::NoCookies;
        return result;
    }
    result.gateName = gate;

    // Compose in the fixed order. The cookie line stays below the cap; the gate and identity
    // cookies come first, so a cap can only ever drop client-side cookies.
    std::string line;
    for (std::size_t i = 0; i < kOrderCount; ++i) {
        std::map<std::string, Pick>::const_iterator it = best.find(kOrder[i]);
        if (it == best.end()) continue;
        const ports::RawCookieRow &row = rows[it->second.row];
        const std::size_t add = row.name.size() + 1 + row.value.size() + (line.empty() ? 0 : 2);
        if (line.size() + add > static_cast<std::size_t>(Limits::kMaxComposedBytes)) break;
        if (!line.empty()) line += "; ";
        line += row.name; line += '='; line += row.value;
        result.names.push_back(row.name);
        if (it->second.rank == 1) ++result.fromGoogleHost;
    }

    std::string out;
    if (validAuthUser(options.authUser)) { out += "X-Goog-AuthUser: "; out += options.authUser; out += '\n'; result.selectionApplied = true; }
    if (validPageId(options.pageId)) { out += "X-Goog-PageId: "; out += options.pageId; out += '\n'; result.selectionApplied = true; }
    out += line;
    accounts::domain::wipeString(line);
    result.text.swap(out);
    result.status = SelectionResult::Status::Ok;
    return result;
}

}}}}}
