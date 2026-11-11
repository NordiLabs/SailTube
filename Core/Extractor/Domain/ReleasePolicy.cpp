// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "ReleasePolicy.h"

#include <cstdlib>

namespace nordtile { namespace video { namespace extractor {

const char kAssetName[] = "yt-dlp";
const char kSumsName[] = "SHA2-256SUMS";
const char kSignatureName[] = "SHA2-256SUMS.sig";
const char kReleaseHost[] = "github.com";
const char kArchiveFile[] = "yt-dlp.pyz";
const char kManifestFile[] = "manifest.json";

namespace {
const char kStableRepository[] = "yt-dlp/yt-dlp";
const char kNightlyRepository[] = "yt-dlp/yt-dlp-nightly-builds";

bool isDigit(char c) { return c >= '0' && c <= '9'; }
char lower(char c) { return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c; }

std::vector<std::string> split(const std::string &s, char sep) {
    std::vector<std::string> out;
    std::string::size_type start = 0;
    for (;;) {
        const std::string::size_type at = s.find(sep, start);
        out.push_back(s.substr(start, at == std::string::npos ? std::string::npos : at - start));
        if (at == std::string::npos) return out;
        start = at + 1;
    }
}

bool allDigits(const std::string &s) {
    if (s.empty()) return false;
    for (char c : s) if (!isDigit(c)) return false;
    return true;
}

bool validFileName(const std::string &f) {
    if (f.empty() || f.size() > 64 || f[0] == '.' || f[0] == '-') return false;
    for (char c : f)
        if (!(isDigit(c) || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '.' || c == '_' || c == '-')) return false;
    return true;
}

bool startsWith(const std::string &s, const std::string &prefix) {
    return s.size() >= prefix.size() && s.compare(0, prefix.size(), prefix) == 0;
}
}

const char *channelName(Channel c) { return c == Channel::Nightly ? "nightly" : "stable"; }

bool parseChannel(const std::string &name, Channel *out) {
    if (name == "stable") { *out = Channel::Stable; return true; }
    if (name == "nightly") { *out = Channel::Nightly; return true; }
    return false;
}

const char *repository(Channel c) { return c == Channel::Nightly ? kNightlyRepository : kStableRepository; }

std::string latestUrl(Channel c, const std::string &file) {
    return std::string("https://") + kReleaseHost + "/" + repository(c) + "/releases/latest/download/" + file;
}

std::string taggedUrl(Channel c, const std::string &tag, const std::string &file) {
    return std::string("https://") + kReleaseHost + "/" + repository(c) + "/releases/download/" + tag + "/" + file;
}

bool isAssetHost(const std::string &host) {
    return host == "objects.githubusercontent.com" || host == "release-assets.githubusercontent.com";
}

bool validVersion(const std::string &v) {
    const std::vector<std::string> parts = split(v, '.');
    if (parts.size() != 3 && parts.size() != 4) return false;
    if (parts[0].size() != 4 || parts[1].size() != 2 || parts[2].size() != 2) return false;
    for (const std::string &p : parts) if (!allDigits(p)) return false;
    if (parts.size() == 4 && parts[3].size() > 6) return false;
    return true;
}

int compareVersions(const std::string &a, const std::string &b) {
    const bool va = validVersion(a), vb = validVersion(b);
    if (!va || !vb) return va == vb ? 0 : (va ? 1 : -1);
    const std::vector<std::string> pa = split(a, '.'), pb = split(b, '.');
    for (std::size_t i = 0; i < 4; ++i) {
        const long x = i < pa.size() ? std::strtol(pa[i].c_str(), nullptr, 10) : -1;
        const long y = i < pb.size() ? std::strtol(pb[i].c_str(), nullptr, 10) : -1;
        if (x != y) return x < y ? -1 : 1;
    }
    return 0;
}

bool validSha256(const std::string &s) {
    if (s.size() != 64) return false;
    for (char c : s) if (!(isDigit(c) || (c >= 'a' && c <= 'f'))) return false;
    return true;
}

bool parseUrl(const std::string &text, Url *out) {
    Url u;
    if (text.empty() || text.size() > 4096) return false;
    for (char c : text) if (static_cast<unsigned char>(c) <= 0x20 || static_cast<unsigned char>(c) >= 0x7f) return false;
    const std::string::size_type colon = text.find("://");
    if (colon == std::string::npos || colon == 0) return false;
    for (std::string::size_type i = 0; i < colon; ++i) {
        const char c = lower(text[i]);
        if (!((c >= 'a' && c <= 'z') || (i > 0 && (isDigit(c) || c == '+' || c == '-' || c == '.')))) return false;
        u.scheme.push_back(c);
    }
    const std::string::size_type authorityStart = colon + 3;
    std::string::size_type authorityEnd = text.find_first_of("/?#", authorityStart);
    if (authorityEnd == std::string::npos) authorityEnd = text.size();
    std::string authority = text.substr(authorityStart, authorityEnd - authorityStart);
    const std::string::size_type at = authority.rfind('@');
    if (at != std::string::npos) { u.userInfo = true; authority = authority.substr(at + 1); }
    if (authority.empty() || authority[0] == '[') return false;   // no IPv6 literals
    const std::string::size_type portColon = authority.find(':');
    std::string host = authority.substr(0, portColon);
    if (portColon != std::string::npos) {
        const std::string port = authority.substr(portColon + 1);
        if (!allDigits(port) || port.size() > 5) return false;
        u.port = std::atoi(port.c_str());
        if (u.port < 1 || u.port > 65535) return false;
    }
    if (host.empty() || host.size() > 253) return false;
    for (char &c : host) {
        c = lower(c);
        if (!((c >= 'a' && c <= 'z') || isDigit(c) || c == '-' || c == '.')) return false;
    }
    if (host.front() == '.' || host.back() == '.' || host.find("..") != std::string::npos) return false;
    u.host = host;
    std::string rest = text.substr(authorityEnd);
    const std::string::size_type hash = rest.find('#');
    if (hash != std::string::npos) rest = rest.substr(0, hash);
    const std::string::size_type question = rest.find('?');
    u.path = rest.substr(0, question);
    if (question != std::string::npos) u.query = rest.substr(question + 1);
    if (u.path.empty()) u.path = "/";
    if (u.path[0] != '/') return false;
    *out = u;
    return true;
}

std::string resolveLocation(const std::string &base, const std::string &location) {
    if (location.empty()) return std::string();
    Url probe;
    if (location.find("://") != std::string::npos) return parseUrl(location, &probe) ? location : std::string();
    if (location[0] != '/' || (location.size() > 1 && location[1] == '/')) return std::string();
    Url b;
    if (!parseUrl(base, &b)) return std::string();
    std::string out = b.scheme + "://" + b.host;
    if (b.port >= 0) out += ":" + std::to_string(b.port);
    out += location;
    return parseUrl(out, &probe) ? out : std::string();
}

const char *refusalLabel(Refusal r) {
    switch (r) {
    case Refusal::None: return "";
    case Refusal::InsecureUrl: return "insecure-url";
    case Refusal::HostNotAllowed: return "host-not-allowed";
    case Refusal::BadReleasePath: return "bad-release-path";
    case Refusal::UnexpectedRedirect: return "unexpected-redirect";
    case Refusal::RedirectLimit: return "redirect-limit";
    }
    return "refused";
}

RedirectChain::RedirectChain(Channel channel, const std::string &file, const std::string &expectedTag)
    : channel_(channel), file_(file), expectedTag_(expectedTag) {}

Refusal RedirectChain::next(const std::string &text) {
    if (last_ != Last::None) {
        if (hops_ >= kMaxRedirects) return Refusal::RedirectLimit;
        ++hops_;
    }
    Url u;
    if (!parseUrl(text, &u) || u.scheme != "https" || u.userInfo || (u.port != -1 && u.port != 443))
        return Refusal::InsecureUrl;
    if (!validFileName(file_)) return Refusal::BadReleasePath;
    if (u.host == kReleaseHost) {
        if (last_ == Last::Tagged || last_ == Last::Asset) return Refusal::UnexpectedRedirect;
        if (!u.query.empty()) return Refusal::BadReleasePath;
        const std::string base = std::string("/") + repository(channel_) + "/releases/";
        if (u.path == base + "latest/download/" + file_) {
            if (last_ != Last::None || !expectedTag_.empty()) return Refusal::BadReleasePath;
            last_ = Last::Latest;
            return Refusal::None;
        }
        const std::string prefix = base + "download/";
        const std::string suffix = "/" + file_;
        if (!startsWith(u.path, prefix) || u.path.size() <= prefix.size() + suffix.size()
            || u.path.compare(u.path.size() - suffix.size(), suffix.size(), suffix) != 0)
            return Refusal::BadReleasePath;
        const std::string tag = u.path.substr(prefix.size(), u.path.size() - prefix.size() - suffix.size());
        if (!validVersion(tag) || (!expectedTag_.empty() && tag != expectedTag_)) return Refusal::BadReleasePath;
        tag_ = tag;
        last_ = Last::Tagged;
        return Refusal::None;
    }
    if (isAssetHost(u.host)) {
        if (last_ != Last::Tagged) return Refusal::UnexpectedRedirect;
        last_ = Last::Asset;
        return Refusal::None;
    }
    return Refusal::HostNotAllowed;
}

bool RedirectChain::finalOk() const { return !tag_.empty() && (last_ == Last::Tagged || last_ == Last::Asset); }

std::string sumsEntry(const std::string &sums, const std::string &asset) {
    std::string found;
    int matches = 0;
    for (const std::string &raw : split(sums, '\n')) {
        std::string line = raw;
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.empty()) continue;
        const std::string::size_type space = line.find_first_of(" \t");
        if (space != 64) return std::string();
        std::string hash = line.substr(0, 64);
        for (char &c : hash) c = lower(c);
        if (!validSha256(hash)) return std::string();
        std::string::size_type nameAt = line.find_first_not_of(" \t", space);
        if (nameAt == std::string::npos) return std::string();
        if (line[nameAt] == '*') ++nameAt;
        if (line.compare(nameAt, std::string::npos, asset) == 0 && line.size() - nameAt == asset.size()) {
            found = hash;
            ++matches;
        }
    }
    return matches == 1 ? found : std::string();
}

Layout layoutFor(const std::string &root) {
    Layout l;
    l.root = root;
    l.current = root + "/current";
    l.previous = root + "/previous";
    l.staging = root + "/staging";
    l.swap = root + "/swap";
    l.trash = root + "/trash";
    return l;
}

bool manifestValid(const Manifest &m) {
    Channel channel;
    if (!validVersion(m.version) || !validSha256(m.sha256) || !parseChannel(m.channel, &channel)) return false;
    if (m.size < kMinArchiveBytes || m.size > kMaxArchiveBytes || m.installedAtMs <= 0) return false;
    return m.source == taggedUrl(channel, m.version, kAssetName);
}

Active chooseActive(const InstalledCopy &c, const std::string &bundledVersion) {
    if (!c.manifestReadable || !manifestValid(c.manifest) || !c.archivePresent) return Active::Bundled;
    if (c.archiveSize != c.manifest.size || c.archiveSha256 != c.manifest.sha256) return Active::Bundled;
    if (validVersion(bundledVersion) && compareVersions(c.manifest.version, bundledVersion) < 0) return Active::Bundled;
    return Active::Current;
}

bool worthOffering(const std::string &releaseVersion, const std::string &activeVersion) {
    if (!validVersion(releaseVersion)) return false;
    return !validVersion(activeVersion) || compareVersions(releaseVersion, activeVersion) > 0;
}

TokenVerdict checkToken(const Offer *offer, const std::string &version, long long size, const std::string &source,
                        long long nowMs) {
    if (!offer || offer->version.empty()) return TokenVerdict::NoOffer;
    if (offer->version != version || offer->size != size || offer->source != source) return TokenVerdict::Mismatch;
    if (nowMs < offer->offeredAtMs || nowMs - offer->offeredAtMs > kOfferLifetimeMs) return TokenVerdict::Expired;
    return TokenVerdict::Ok;
}

const char *stepName(Step s) {
    switch (s) {
    case Step::ClearStaging: return "clear-staging";
    case Step::ClearTrash: return "clear-trash";
    case Step::PreviousToTrash: return "previous-to-trash";
    case Step::CurrentToPrevious: return "current-to-previous";
    case Step::StagingToCurrent: return "staging-to-current";
    case Step::CurrentToSwap: return "current-to-swap";
    case Step::PreviousToCurrent: return "previous-to-current";
    case Step::SwapToCurrent: return "swap-to-current";
    case Step::SwapToPrevious: return "swap-to-previous";
    case Step::CurrentToTrash: return "current-to-trash";
    case Step::ClearSwap: return "clear-swap";
    }
    return "?";
}

std::vector<Step> installPlan(bool hasCurrent, bool hasPrevious) {
    std::vector<Step> plan;
    plan.push_back(Step::ClearTrash);
    if (hasCurrent) {
        if (hasPrevious) plan.push_back(Step::PreviousToTrash);
        plan.push_back(Step::CurrentToPrevious);
    }
    plan.push_back(Step::StagingToCurrent);
    plan.push_back(Step::ClearTrash);
    return plan;
}

std::vector<Step> rollbackPlan(bool hasCurrent, bool hasPrevious) {
    std::vector<Step> plan;
    if (!hasPrevious) return plan;
    if (hasCurrent) {
        plan.push_back(Step::ClearSwap);
        plan.push_back(Step::CurrentToSwap);
        plan.push_back(Step::PreviousToCurrent);
        plan.push_back(Step::SwapToPrevious);
    } else {
        plan.push_back(Step::PreviousToCurrent);
    }
    return plan;
}

std::vector<Step> revertPlan(bool hasCurrent) {
    std::vector<Step> plan;
    if (!hasCurrent) return plan;
    plan.push_back(Step::ClearTrash);
    plan.push_back(Step::CurrentToTrash);
    plan.push_back(Step::ClearTrash);
    return plan;
}

std::vector<Step> recoveryPlan(bool hasCurrent, bool hasPrevious, bool hasSwap) {
    std::vector<Step> plan;
    if (hasSwap && !hasCurrent) {
        plan.push_back(Step::SwapToCurrent);
        hasCurrent = true;
        hasSwap = false;
    }
    if (hasSwap && !hasPrevious) {
        plan.push_back(Step::SwapToPrevious);
        hasPrevious = true;
        hasSwap = false;
    }
    if (hasSwap) plan.push_back(Step::ClearSwap);
    if (!hasCurrent && hasPrevious) plan.push_back(Step::PreviousToCurrent);
    plan.push_back(Step::ClearStaging);
    plan.push_back(Step::ClearTrash);
    return plan;
}

}}}
