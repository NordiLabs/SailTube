// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "BrowserCookieJarSource.h"
#include <ctime>

namespace nordtile { namespace music { namespace ytm { namespace sources { namespace application {

namespace acc = nordtile::music::accounts;

struct BrowserCookieJarSource::Shared {
    bool alive = true;
    bool running = false;
    std::uint64_t generation = 0;
    Report report;
};

// Written on the worker, read on the caller's thread after the worker's completion (the WorkerPort
// contract orders the two). Destruction wipes the selection.
struct BrowserCookieJarSource::Payload {
    domain::ReadStatus read = domain::ReadStatus::NotFound;
    int candidate = -1;
    domain::SelectionResult selection;
};

namespace {

domain::Outcome outcomeForAvailability(domain::Availability availability) {
    switch (availability) {
    case domain::Availability::Available: return domain::Outcome::None;
    case domain::Availability::NotFound: return domain::Outcome::NotFound;
    case domain::Availability::NotReadable: return domain::Outcome::NotReadable;
    case domain::Availability::Unsafe: return domain::Outcome::Unsafe;
    case domain::Availability::NoHome: return domain::Outcome::NotFound;
    }
    return domain::Outcome::NotFound;
}

}  // namespace

BrowserCookieJarSource::BrowserCookieJarSource(const Config &config) : cfg_(config), shared_(new Shared) {}

BrowserCookieJarSource::~BrowserCookieJarSource() {
    shared_->alive = false;
    shared_->running = false;
    ++shared_->generation;      // a completion that arrives later wipes its payload and does nothing else
}

accounts::ports::SessionSource::Status BrowserCookieJarSource::status() const {
    Status s;
    const domain::Availability a = cfg_.reader ? cfg_.reader->probe() : domain::Availability::NotFound;
    s.available = (a == domain::Availability::Available);
    if (!s.available) s.reason = domain::reasonFor(a);
    return s;
}

bool BrowserCookieJarSource::busy() const { return shared_->running; }
domain::Outcome BrowserCookieJarSource::outcome() const { return shared_->report.outcome; }
BrowserCookieJarSource::Report BrowserCookieJarSource::report() const { return shared_->report; }

bool BrowserCookieJarSource::setAccountSelection(const std::string &authUser, const std::string &pageId) {
    if (!authUser.empty() && !domain::CookieJarSelection::validAuthUser(authUser)) return false;
    if (!pageId.empty() && !domain::CookieJarSelection::validPageId(pageId)) return false;
    authUser_ = authUser; pageId_ = pageId;
    return true;
}
void BrowserCookieJarSource::clearAccountSelection() { authUser_.clear(); pageId_.clear(); }

acc::ports::SessionStageResult BrowserCookieJarSource::failureResult(domain::Outcome outcome) {
    acc::ports::SessionStageResult r;
    r.result.message = domain::describe(outcome);
    typedef acc::domain::ErrorCode Code;
    switch (outcome) {
    case domain::Outcome::None:
    case domain::Outcome::Staged: break;
    case domain::Outcome::ConsentMissing: r.result.code = Code::PermissionDenied; r.error = acc::ports::ImportError::ConsentRequired; break;
    case domain::Outcome::NotFound: r.result.code = Code::Unsupported; break;
    case domain::Outcome::NotReadable:
    case domain::Outcome::Unsafe: r.result.code = Code::PermissionDenied; break;
    case domain::Outcome::ChangedWhileReading: r.result.code = Code::Busy; break;
    case domain::Outcome::TooLarge:
    case domain::Outcome::CopyFailed:
    case domain::Outcome::NotADatabase:
    case domain::Outcome::UnexpectedSchema:
    case domain::Outcome::TooManyRows: r.result.code = Code::InvalidResponse; break;
    case domain::Outcome::NoCookies: r.result.code = Code::AuthorizationRequired; r.error = acc::ports::ImportError::NoCookies; break;
    case domain::Outcome::NotSignedIn: r.result.code = Code::AuthorizationRequired; r.error = acc::ports::ImportError::NoSigningCookie; break;
    case domain::Outcome::Rejected: r.result.code = Code::InvalidResponse; break;
    case domain::Outcome::Cancelled: r.result.code = Code::Cancelled; break;
    }
    return r;
}

acc::domain::Result BrowserCookieJarSource::start(acc::ports::SessionMaterialSinkPort &sink, Completion done) {
    acc::domain::Result r;
    if (shared_->running) { r.code = acc::domain::ErrorCode::Busy; r.message = "An import from the browser is already running."; return r; }
    // 1. Consent, before anything in the browser's folder is looked at.
    if (!cfg_.consentGranted || !cfg_.consentGranted()) {
        shared_->report = Report(); shared_->report.outcome = domain::Outcome::ConsentMissing;
        r.code = acc::domain::ErrorCode::PermissionDenied; r.message = domain::describe(domain::Outcome::ConsentMissing);
        return r;
    }
    // 2. Availability: a stat-level probe. Never a permission request.
    const domain::Availability a = cfg_.reader ? cfg_.reader->probe() : domain::Availability::NotFound;
    if (a != domain::Availability::Available) {
        shared_->report = Report(); shared_->report.outcome = outcomeForAvailability(a);
        r.code = (a == domain::Availability::NotReadable || a == domain::Availability::Unsafe)
                     ? acc::domain::ErrorCode::PermissionDenied : acc::domain::ErrorCode::Unsupported;
        r.message = domain::reasonFor(a);
        return r;
    }

    shared_->running = true;
    shared_->report = Report();
    const std::uint64_t generation = ++shared_->generation;

    domain::SelectionOptions options;
    options.nowUtcSeconds = cfg_.nowUtcSeconds ? cfg_.nowUtcSeconds() : static_cast<std::int64_t>(std::time(nullptr));
    options.allowGoogleFallback = cfg_.allowGoogleFallback;
    options.authUser = authUser_;
    options.pageId = pageId_;

    const std::shared_ptr<Payload> payload(new Payload);
    ports::CookieJarReaderPort *reader = cfg_.reader;
    const std::shared_ptr<Shared> shared = shared_;
    acc::ports::SessionMaterialSinkPort *sinkPtr = &sink;
    const Completion completion = done;

    std::function<void()> work = [payload, reader, options]() {
        ports::JarRead jar = reader->read();
        payload->read = jar.status;
        payload->candidate = jar.candidate;
        if (jar.status == domain::ReadStatus::Ok)
            payload->selection = domain::CookieJarSelection::compose(jar.rows, jar.matchingRows, options);
        jar.wipe();
    };
    std::function<void()> fin = [shared, generation, payload, sinkPtr, completion]() {
        BrowserCookieJarSource::finish(shared, generation, payload, sinkPtr, completion);
    };
    if (cfg_.worker) cfg_.worker->run(work, fin);
    else { work(); fin(); }
    return r;
}

void BrowserCookieJarSource::cancel() {
    if (!shared_->running) return;
    shared_->running = false;
    ++shared_->generation;      // the completion in flight wipes its payload and does nothing else
    shared_->report = Report();
    shared_->report.outcome = domain::Outcome::Cancelled;
}

void BrowserCookieJarSource::finish(const std::shared_ptr<Shared> &shared, std::uint64_t generation, const std::shared_ptr<Payload> &payload,
                                    acc::ports::SessionMaterialSinkPort *sink, const Completion &done) {
    if (!shared->alive || shared->generation != generation) { payload->selection.wipe(); return; }
    shared->running = false;

    Report report;
    report.candidate = payload->candidate;
    acc::ports::SessionStageResult result;

    if (payload->read != domain::ReadStatus::Ok) {
        report.outcome = domain::outcomeFor(payload->read);
        result = failureResult(report.outcome);
    } else if (payload->selection.status == domain::SelectionResult::Status::NoCookies) {
        report.outcome = domain::Outcome::NoCookies;
        result = failureResult(report.outcome);
    } else if (payload->selection.status == domain::SelectionResult::Status::NotSignedIn) {
        report.outcome = domain::Outcome::NotSignedIn;
        result = failureResult(report.outcome);
    } else {
        domain::SelectionResult &sel = payload->selection;
        report.cookiesUsed = static_cast<int>(sel.names.size());
        report.fromGoogleHost = sel.fromGoogleHost;
        report.selectionApplied = sel.selectionApplied;
        report.names = sel.names;
        acc::ports::SecretBytes raw;
        raw.assign(reinterpret_cast<const unsigned char *>(sel.text.data()), sel.text.size());
        sel.wipe();                                                     // our copy of the text is zeroed now
        result = sink->stage(std::move(raw), acc::domain::SessionSourceKind::SystemBrowser);   // consumes (wipes) `raw`
        report.outcome = result.ok() ? domain::Outcome::Staged : domain::Outcome::Rejected;
    }
    shared->report = report;
    if (done) done(result);
}

}}}}}
