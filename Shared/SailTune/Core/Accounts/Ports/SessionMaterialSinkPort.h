// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#pragma once
#include "CredentialVaultPort.h"
#include "../Domain/SessionTypes.h"
#include <string>
#include <vector>

namespace nordtile { namespace music { namespace accounts { namespace ports {

// Why a pasted/imported text was refused. Messages for these live in YtmSessionMessages
// (translated); the enum is the stable contract. No value of the text ever appears in them.
enum class ImportError {
    None, Empty, TooLarge, LineTooLong, TooManyLines, BadEncoding, Unrecognized,
    NoCookies, NoSigningCookie, NoSessionCookie, CookiesExpired, OtherSitesOnly,
    TooManyCookies, BadCookieValue, BadAuthUser, BadPageId, Disabled, ConsentRequired, Busy
};

enum class ImportFormat { Unknown, CookieHeader, HeaderBlock, NetscapeFile, Curl };

// Non-secret description of what was staged. Safe to show and to log.
struct SessionStageSummary {
    ImportFormat format = ImportFormat::Unknown;
    int cookiesKept = 0;
    int cookiesSkipped = 0;                    // not on the allow-list / other domain / expired
    std::vector<std::string> keptNames;        // cookie NAMES only
    bool hasSigningCookie = false;
    bool hasSessionCookie = false;
    std::string authUser;                      // "0".."99" or empty
    bool hasPageId = false;
    bool sawAuthorizationHeader = false;       // a pasted Authorization header was discarded
    // Short non-reversible tag of the signing cookie so the user can recognise a paste
    // (first 8 hex digits of SHA-1 over a domain-separated value). Empty if none.
    std::string fingerprint;
};

struct SessionStageResult {
    domain::Result result;
    ImportError error = ImportError::None;
    int errorLine = 0;                         // 1-based line of the problem, 0 if not line-specific
    SessionStageSummary summary;
    bool ok() const { return result.ok() && error == ImportError::None; }
};

// How a SessionSource hands a session to the account adapter without a dependency on
// infrastructure (AuthorizationPort::authorize() has no input). Flow: source -> stage() ->
// AccountService::authorize() (or refresh() for "sign in again") consumes the staged material.
class SessionMaterialSinkPort {
public:
    virtual ~SessionMaterialSinkPort() = default;
    // `raw` is the pasted/imported text (Cookie header, request-header block, cURL command or
    // Netscape cookies.txt). It is consumed (wiped) whatever the outcome. On success only the
    // allow-listed cookies are held in memory, for at most the adapter's staging TTL.
    virtual SessionStageResult stage(SecretBytes &&raw, domain::SessionSourceKind source) = 0;
    virtual bool hasStaged() const = 0;
    virtual void discardStaged() = 0;
};

}}}}
