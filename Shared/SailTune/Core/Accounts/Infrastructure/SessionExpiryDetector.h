// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Maps one authenticated HTTP exchange to the session taxonomy of docs/ytm_hybrid/06-design.md
// section 3.6. Pure C++11: no Qt, no clock, no I/O, never throws.
//
// HTTP 403 (session.md section 13): three different situations used to share one answer.
//   * challenge wording (unusual traffic, captcha, "not a bot", "sign in to confirm", /sorry/)  -> Blocked, Http403Challenge
//   * consent wording, or a 403 nobody explains (HTML, empty, JSON without a permission marker)  -> Blocked, Http403
//   * a JSON error document that names a permission failure, with none of the wording above     -> Forbidden, Http403Forbidden
// The last one concerns a single operation (an object that is not the account's, content that cannot be rated):
// it must fail that operation and nothing else.
//
// EVERY signal below is INFERENCE until it has been observed with the owner's own throwaway
// account (docs/ytm_hybrid/02 section 7 documents 401 only; the HTML/redirect/shape signals
// come from community reports). The point of the table is that it is ONE place, covered by a
// test matrix, so a wrong guess is a one-line change.
#pragma once

#include "../Domain/SessionTypes.h"
#include <string>

namespace nordtile { namespace music { namespace accounts { namespace infrastructure {

// What the parsers slice learned about a 200 JSON document. NotChecked means the caller did
// not look; such an answer is never "commit safe" (see Classification::commitSafe).
enum class BodyShape { NotChecked, SignedIn, SignedOut, MissingRequired };

enum class ExpiryReason {
    Ok, OkShapeUnchecked,
    Http401, SignInRedirect, HtmlSignIn, HtmlUnknown, SignedOutShape,
    ConsentRedirect, ChallengeRedirect, HtmlConsent, HtmlChallenge, Http403, Http403Challenge,
    Http429,
    NetworkError, Http5xx, Http408,
    UnexpectedRedirect, OversizedBody, BadRequest, UnexpectedStatus, EmptyBody, NotJson, MissingRequiredFields,
    // Appended: a 403 whose JSON error document names a permission failure (verdict Forbidden).
    Http403Forbidden
};

struct ExpiryObservation {
    int httpStatus = 0;               // 0 and networkError: no HTTP answer at all
    bool networkError = false;        // DNS, TLS, connect, timeout, reset
    bool oversized = false;           // the body hit the response size cap
    std::string redirectHost;         // lower-case host of Location for 3xx (never the query)
    std::string redirectPath;         // path of Location for 3xx (never the query)
    std::string contentType;          // header value in any case
    std::string bodyPrefix;           // first bytes of the body; at most 8192 are examined
    BodyShape shape = BodyShape::NotChecked;   // only meaningful for 200 JSON
};

struct Classification {
    domain::SessionVerdict verdict = domain::SessionVerdict::Transient;
    ExpiryReason reason = ExpiryReason::NetworkError;
    // A snapshot may be committed only when the shape was verified as a signed-in document.
    bool commitSafe() const { return verdict == domain::SessionVerdict::Ok && reason == ExpiryReason::Ok; }
};

class SessionExpiryDetector {
public:
    static Classification classify(const ExpiryObservation &observation);
    static const char *reasonName(ExpiryReason reason);
};

}}}}
