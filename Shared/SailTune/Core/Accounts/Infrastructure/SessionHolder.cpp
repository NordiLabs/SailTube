// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "SessionHolder.h"

namespace nordtile { namespace music { namespace accounts { namespace infrastructure {

SessionHolder::SessionHolder(const SignerOptions &signer)
    : signer_(signer), generation_(1), revision_(0), dirty_(false) {}

SessionHolder::~SessionHolder() {
    std::lock_guard<std::mutex> lock(mutex_);
    bundle_.wipe(); domain::wipeString(userSessionId_); domain::wipeString(displayName_);
}

void SessionHolder::setSignerOptions(const SignerOptions &options) {
    std::lock_guard<std::mutex> lock(mutex_); signer_ = options;
}
void SessionHolder::setUserSessionId(const std::string &id) {
    std::lock_guard<std::mutex> lock(mutex_); domain::wipeString(userSessionId_); userSessionId_ = id;
}

std::uint64_t SessionHolder::install(const domain::SessionBundle &bundle, bool dirty) {
    std::lock_guard<std::mutex> lock(mutex_);
    bundle_.wipe();
    bundle_ = bundle;
    ++generation_; ++revision_; dirty_ = dirty;
    return generation_;
}

std::uint64_t SessionHolder::wipe() {
    std::lock_guard<std::mutex> lock(mutex_);
    bundle_.wipe(); domain::wipeString(userSessionId_); domain::wipeString(displayName_);
    ++generation_; ++revision_; dirty_ = false;
    return generation_;
}

bool SessionHolder::empty() const { std::lock_guard<std::mutex> lock(mutex_); return bundle_.empty(); }
std::uint64_t SessionHolder::revision() const { std::lock_guard<std::mutex> lock(mutex_); return revision_; }

bool SessionHolder::snapshot(Snapshot *out) const {
    if (!out) return false;
    std::lock_guard<std::mutex> lock(mutex_);
    out->bundle.wipe();
    if (bundle_.empty()) return false;
    out->bundle = bundle_;
    out->generation = generation_; out->revision = revision_; out->dirty = dirty_;
    return true;
}

bool SessionHolder::markPersisted(std::uint64_t generation, std::uint64_t revision) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (generation != generation_ || revision != revision_ || bundle_.empty()) return false;
    dirty_ = false; return true;
}

void SessionHolder::setDirtyObserver(std::function<void()> observer) {
    std::lock_guard<std::mutex> lock(mutex_); observer_ = std::move(observer);
}
void SessionHolder::setDisplayName(const std::string &name) {
    std::lock_guard<std::mutex> lock(mutex_); domain::wipeString(displayName_); displayName_ = name;
}
std::string SessionHolder::displayName() const { std::lock_guard<std::mutex> lock(mutex_); return displayName_; }

bool SessionHolder::signedIn() const { std::lock_guard<std::mutex> lock(mutex_); return bundle_.usable(); }
std::uint64_t SessionHolder::generation() const { std::lock_guard<std::mutex> lock(mutex_); return generation_; }

bool SessionHolder::requestHeaders(const std::string &origin, std::int64_t nowUtcSeconds, ports::SessionRequestHeaders *out) const {
    if (!out) return false;
    out->wipe();
    std::lock_guard<std::mutex> lock(mutex_);
    if (!bundle_.usable() || !SessionSigner::validOrigin(origin)) return false;

    SigningInput in;
    in.options = signer_; in.origin = origin; in.nowUtcSeconds = nowUtcSeconds;
    in.userSessionId = userSessionId_;
    const domain::SessionCookie *c = bundle_.find("SAPISID"); if (c) in.sapisid = c->value;
    c = bundle_.find("__Secure-1PAPISID"); if (c) in.secure1Papisid = c->value;
    c = bundle_.find("__Secure-3PAPISID"); if (c) in.secure3Papisid = c->value;
    SigningResult signedValue = SessionSigner::sign(in);
    if (!signedValue.ok) return false;

    std::string cookie;
    for (std::size_t i = 0; i < bundle_.cookies.size(); ++i) {
        if (i) cookie += "; ";
        cookie += bundle_.cookies[i].name; cookie += '='; cookie += bundle_.cookies[i].value;
    }
    out->cookie = cookie; domain::wipeString(cookie);
    out->authorization = signedValue.value;
    out->origin = origin;
    out->authUser = bundle_.authUser.empty() ? std::string("0") : bundle_.authUser;
    out->pageId = bundle_.pageId;
    out->generation = generation_;
    return true;
}

ports::RotationOutcome SessionHolder::applyRotation(const std::vector<ports::RotatedCookie> &updates, std::uint64_t requestGeneration) {
    std::function<void()> notify;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (bundle_.empty()) return ports::RotationOutcome::NotSignedIn;
        if (requestGeneration != generation_) return ports::RotationOutcome::StaleGeneration;
        bool changed = false;
        for (std::size_t i = 0; i < updates.size(); ++i) {
            const ports::RotatedCookie &u = updates[i];
            const domain::CookieRole role = domain::cookieRole(u.name);
            if (role == domain::CookieRole::None || !domain::validCookieName(u.name)) continue;
            if (u.deleted || u.value.empty()) {
                // Only client-side cookies may be deleted by a response; signing and identity
                // cookies are never dropped on a server's say-so (the next probe classifies).
                if (role == domain::CookieRole::Client && bundle_.erase(u.name)) changed = true;
                continue;
            }
            if (!domain::validCookieValue(u.value)) continue;
            const domain::SessionCookie *existing = bundle_.find(u.name);
            if (existing && existing->value == u.value) continue;
            if (bundle_.set(u.name, u.value)) changed = true;
        }
        if (!changed) return ports::RotationOutcome::NoChange;
        ++revision_; dirty_ = true;
        notify = observer_;
    }
    if (notify) notify();
    return ports::RotationOutcome::Applied;
}

}}}}
