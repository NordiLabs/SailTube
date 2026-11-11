// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#pragma once

// [FULL] NFR-MNT-01 / NFR-SEC-04: in-app update of the yt-dlp archive. Qt-free and I/O-free: the
// release channels and their URLs, the per-hop URL allow-list (https, port 443, github.com then the
// release asset hosts, no user info), the version taken from the redirect path, the size caps, the
// store layout and its manifest rules, which archive is active, the install / rollback / crash-recovery
// rename plans and the confirmation-token rule. Time is passed in (milliseconds).
// The STORE build compiles none of this (only `!sailtube_store { }` in the project lists it).

#ifdef SAILTUBE_STORE
#error "The extractor updater is FULL only (NFR-MNT-01 [STORE] uses only the bundled extractor)"
#endif

#include <string>
#include <vector>

namespace nordtile { namespace video { namespace extractor {

// ---- Channels and release files ----

enum class Channel { Stable, Nightly };
const char *channelName(Channel);                          // "stable", "nightly"
bool parseChannel(const std::string &name, Channel *out);  // exactly "stable" or "nightly"
const char *repository(Channel);                           // "yt-dlp/yt-dlp", "yt-dlp/yt-dlp-nightly-builds"

extern const char kAssetName[];      // "yt-dlp": the zipimport archive with a shebang
extern const char kSumsName[];       // "SHA2-256SUMS"
extern const char kSignatureName[];  // "SHA2-256SUMS.sig"
extern const char kReleaseHost[];    // "github.com"

// https://github.com/<repo>/releases/latest/download/<file>
std::string latestUrl(Channel, const std::string &file);
// https://github.com/<repo>/releases/download/<tag>/<file>
std::string taggedUrl(Channel, const std::string &tag, const std::string &file);

// The two hosts GitHub redirects release downloads to. Nothing else is ever contacted.
bool isAssetHost(const std::string &host);

// ---- Limits ----

const long long kMaxArchiveBytes = 64LL << 20;
const long long kMinArchiveBytes = 2LL << 10;     // the real archive is about 3 MiB; fakes in tests are ~5 KiB
const long long kMaxSumsBytes = 64LL << 10;
const long long kMaxSignatureBytes = 16LL << 10;
const int kMaxRedirects = 4;                           // hops after the first request
const long long kOfferLifetimeMs = 30LL * 60 * 1000;   // a confirmation older than this is refused

// ---- Versions ----

// A yt-dlp release tag / __version__: YYYY.MM.DD or YYYY.MM.DD.N (N up to 6 digits).
bool validVersion(const std::string &);
// <0, 0, >0 by numeric components (both must be valid; invalid sorts lowest).
int compareVersions(const std::string &a, const std::string &b);
bool validSha256(const std::string &);   // 64 lowercase hex

// ---- URLs and redirect hops ----

struct Url {
    std::string scheme, host, path, query;
    int port = -1;            // -1: none given
    bool userInfo = false;
};
// Strict absolute URL parser; lower-cases scheme and host. False for anything it does not understand.
bool parseUrl(const std::string &text, Url *out);
// A Location header resolved against the URL that answered: absolute, or path-absolute ("/...").
// Empty when the location is neither (relative paths, scheme-relative "//host" included).
std::string resolveLocation(const std::string &base, const std::string &location);

enum class Refusal {
    None,
    InsecureUrl,         // not https, a port other than 443, user info, unparsable
    HostNotAllowed,      // a host other than github.com or the two asset hosts
    BadReleasePath,      // github.com path is not this channel's release download of this file, or the tag changed
    UnexpectedRedirect,  // a redirect from an asset host, or before the tag is known
    RedirectLimit,
};
const char *refusalLabel(Refusal);   // fixed label: "insecure-url", "host-not-allowed", ...

// One download's chain of URLs (first request, then each redirect target), checked hop by hop.
// The first URL must be the channel's latest/download or releases/download URL of `file` on github.com;
// github.com may redirect only to the same file under releases/download/<tag>/ (one tag, valid);
// a github.com tagged URL may redirect only to an asset host; an asset host answers, never redirects.
class RedirectChain {
public:
    RedirectChain(Channel channel, const std::string &file, const std::string &expectedTag = std::string());
    Refusal next(const std::string &url);
    // The last accepted URL may answer with the content (a tag is known; github.com tagged or asset host).
    bool finalOk() const;
    const std::string &tag() const { return tag_; }
    int hops() const { return hops_; }

private:
    Channel channel_;
    std::string file_, tag_, expectedTag_;
    int hops_ = 0;
    enum class Last { None, Latest, Tagged, Asset } last_ = Last::None;
};

// ---- Release metadata ----

// The SHA-256 (lowercase) of `asset` in a SHA2-256SUMS text: exactly one exact-name line. Empty otherwise.
// Only an early check; the signature-verified answer comes from the verifier.
std::string sumsEntry(const std::string &sums, const std::string &asset);

// ---- Store layout: <root>/{current,previous,staging,swap,trash} ----

extern const char kArchiveFile[];    // "yt-dlp.pyz"
extern const char kManifestFile[];   // "manifest.json"

struct Layout {
    std::string root, current, previous, staging, swap, trash;
    std::string archive(const std::string &dir) const { return dir + "/" + kArchiveFile; }
    std::string manifest(const std::string &dir) const { return dir + "/" + kManifestFile; }
    std::string sums(const std::string &dir) const { return dir + "/" + kSumsName; }
    std::string signature(const std::string &dir) const { return dir + "/" + kSignatureName; }
};
Layout layoutFor(const std::string &root);

// manifest.json of current/ and previous/: written only after the verifier accepted the staged copy.
struct Manifest {
    std::string version, sha256, source, channel;
    long long size = -1;
    long long installedAtMs = -1;
};
// Every field present and well-formed; size within the caps; channel known; source == taggedUrl of
// that channel, version and kAssetName.
bool manifestValid(const Manifest &);

// ---- Which archive is active ----

struct InstalledCopy {
    bool manifestReadable = false;
    Manifest manifest;
    bool archivePresent = false;
    std::string archiveSha256;   // computed at start
    long long archiveSize = -1;
};
enum class Active { Bundled, Current };
// Current only when its manifest is valid and the archive's size and SHA-256 match it, and it is not
// older than the bundled extractor (bundledVersion empty: unknown, not compared).
Active chooseActive(const InstalledCopy &current, const std::string &bundledVersion);

// ---- Offers ----

// Offer an update only when the release is newer than what runs now (activeVersion empty: unknown).
bool worthOffering(const std::string &releaseVersion, const std::string &activeVersion);

struct Offer {
    std::string version, source, channel, sums, signature, sumsSha256;   // sums/signature: the bytes fetched
    long long size = -1;
    long long offeredAtMs = -1;
};
enum class TokenVerdict { Ok, NoOffer, Mismatch, Expired };
// An install must present exactly the {version, size, source} the last check offered (the values the
// confirmation showed), within kOfferLifetimeMs.
TokenVerdict checkToken(const Offer *offer, const std::string &version, long long size, const std::string &source,
                        long long nowMs);

// ---- Rename plans (each step is one rename(2) or one recursive removal inside <root>) ----

enum class Step {
    ClearStaging,        // remove staging/ if present
    ClearTrash,          // remove trash/ if present
    PreviousToTrash,     // rename previous -> trash
    CurrentToPrevious,   // rename current -> previous
    StagingToCurrent,    // rename staging -> current (the commit point of an install)
    CurrentToSwap,       // rename current -> swap
    PreviousToCurrent,   // rename previous -> current
    SwapToCurrent,       // rename swap -> current
    SwapToPrevious,      // rename swap -> previous
    CurrentToTrash,      // rename current -> trash
    ClearSwap,           // remove swap/
};
const char *stepName(Step);

// Install of a verified staging/: drop the old previous, keep current as previous, promote staging.
// A crash before StagingToCurrent leaves current (or, after CurrentToPrevious, previous) to recovery.
std::vector<Step> installPlan(bool hasCurrent, bool hasPrevious);
// Rollback: previous becomes current and current becomes previous. Empty when there is no previous.
std::vector<Step> rollbackPlan(bool hasCurrent, bool hasPrevious);
// Back to the bundled extractor: current goes to trash, previous is kept. Empty without current.
std::vector<Step> revertPlan(bool hasCurrent);
// At start: undo or finish whatever a crash interrupted, so the previous working state comes back:
//   no current but swap      -> SwapToCurrent      (rollback interrupted after CurrentToSwap)
//   swap and no previous     -> SwapToPrevious     (rollback interrupted after PreviousToCurrent)
//   swap left over           -> ClearSwap
//   no current but previous  -> PreviousToCurrent  (install interrupted after CurrentToPrevious)
//   then ClearStaging, ClearTrash (an unverified or half-written staging is never promoted).
std::vector<Step> recoveryPlan(bool hasCurrent, bool hasPrevious, bool hasSwap);

}}}
