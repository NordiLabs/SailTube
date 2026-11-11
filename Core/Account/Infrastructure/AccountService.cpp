// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "AccountService.h"

#include <QCryptographicHash>
#include <QDateTime>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QNetworkCookie>
#include <QPointer>
#include <algorithm>

namespace nordtile { namespace video { namespace account {

namespace {

const char fallbackWebVersion[] = "2.20241105.01.00";

std::string sha1Hex(const std::string &input) {
    return QCryptographicHash::hash(QByteArray::fromStdString(input), QCryptographicHash::Sha1).toHex().toStdString();
}

QString text(const QJsonValue &v) {
    if (v.isString()) return v.toString();
    const QJsonObject o = v.toObject();
    if (o.contains(QStringLiteral("simpleText"))) return o.value(QStringLiteral("simpleText")).toString();
    if (o.contains(QStringLiteral("content"))) return o.value(QStringLiteral("content")).toString();
    QString out;
    for (const QJsonValue &run : o.value(QStringLiteral("runs")).toArray())
        out += run.toObject().value(QStringLiteral("text")).toString();
    return out;
}

// First object found under `key`, depth first.
QJsonObject findObject(const QJsonValue &v, const QString &key) {
    if (v.isArray()) {
        for (const QJsonValue &e : v.toArray()) { const QJsonObject o = findObject(e, key); if (!o.isEmpty()) return o; }
        return QJsonObject();
    }
    if (!v.isObject()) return QJsonObject();
    const QJsonObject o = v.toObject();
    if (o.value(key).isObject()) return o.value(key).toObject();
    for (auto it = o.constBegin(); it != o.constEnd(); ++it) {
        const QJsonObject r = findObject(it.value(), key);
        if (!r.isEmpty()) return r;
    }
    return QJsonObject();
}

void collectObjects(const QJsonValue &v, const QString &key, const QString &fallback, QList<QJsonObject> *out) {
    if (v.isArray()) { for (const QJsonValue &e : v.toArray()) collectObjects(e, key, fallback, out); return; }
    if (!v.isObject()) return;
    const QJsonObject o = v.toObject();
    if (o.value(key).isObject()) { out->append(o.value(key).toObject()); return; }
    if (o.value(fallback).isObject()) { out->append(o.value(fallback).toObject()); return; }
    for (auto it = o.constBegin(); it != o.constEnd(); ++it) collectObjects(it.value(), key, fallback, out);
}

QString lastThumbnail(const QJsonObject &o) {
    const QJsonArray a = findObject(o, QStringLiteral("accountPhoto")).value(QStringLiteral("thumbnails")).toArray();
    QString url = a.isEmpty() ? QString() : a.last().toObject().value(QStringLiteral("url")).toString();
    if (url.startsWith(QLatin1String("//"))) url.prepend(QStringLiteral("https:"));
    return url;
}

} // namespace

AccountService::AccountService(AccountStore *store, AuthTransport *transport, QObject *parent)
    : QObject(parent), store_(store), transport_(transport),
      clock_([] { return QDateTime::currentMSecsSinceEpoch(); }) {
    timer_.setInterval(60 * 1000);
    connect(&timer_, &QTimer::timeout, this, &AccountService::pollSchedule);
}

void AccountService::setClockForTests(Clock clock) { clock_ = clock; }
void AccountService::setClientVersionProvider(std::function<QString()> provider) { clientVersion_ = provider; }
void AccountService::setRequestLocale(const QString &hl, const QString &gl) { hl_ = hl; gl_ = gl; }

void AccountService::start() {
    timer_.start();
    if (store_->signedIn()) {
        setState(SessionState::Checking);
        verifyNow();          // ACC-07 "at start"
    } else {
        setState(SessionState::SignedOut);
    }
}

void AccountService::setState(SessionState s) {
    if (s == state_) return;
    state_ = s;
    emit stateChanged();
}

std::vector<std::pair<QByteArray, QByteArray>> AccountService::headersFor(const AccountSelection &selection,
                                                                           bool withPageId) const {
    AccountSelection sel = selection;
    if (!withPageId) sel.pageId.clear();
    const RequestHeaders h = authenticatedHeaders(store_->session(), sel, clock_() / 1000, sha1Hex);
    std::vector<std::pair<QByteArray, QByteArray>> out;
    for (const auto &p : h.headers) out.push_back(std::make_pair(QByteArray::fromStdString(p.first), QByteArray::fromStdString(p.second)));
    return out;
}

std::vector<std::pair<QByteArray, QByteArray>> AccountService::sign() const {
    return headersFor(store_->selection(), true);
}

void AccountService::sendJson(const QString &endpoint, const QJsonObject &body,
                              const std::vector<std::pair<QByteArray, QByteArray>> &headers,
                              std::function<void(const AuthReply &)> done) {
    if (headers.empty()) { AuthReply none; done(none); return; } // signed out: nothing is sent
    QJsonObject client;
    client.insert(QStringLiteral("clientName"), QStringLiteral("WEB"));
    client.insert(QStringLiteral("clientVersion"), clientVersion_ ? clientVersion_() : QString::fromLatin1(fallbackWebVersion));
    client.insert(QStringLiteral("hl"), hl_);
    client.insert(QStringLiteral("gl"), gl_);
    QJsonObject context;
    context.insert(QStringLiteral("client"), client);
    QJsonObject payload = body;
    payload.insert(QStringLiteral("context"), context);
    AuthRequest req;
    req.url = QUrl(QStringLiteral("https://www.youtube.com/youtubei/v1/") + endpoint + QStringLiteral("?prettyPrint=false"));
    req.body = QJsonDocument(payload).toJson(QJsonDocument::Compact);
    req.headers.append(qMakePair(QByteArray("Content-Type"), QByteArray("application/json")));
    req.headers.append(qMakePair(QByteArray("X-YouTube-Client-Name"), QByteArray("1")));
    req.headers.append(qMakePair(QByteArray("X-YouTube-Client-Version"),
                                 (clientVersion_ ? clientVersion_() : QString::fromLatin1(fallbackWebVersion)).toLatin1()));
    for (const auto &h : headers) req.headers.append(qMakePair(h.first, h.second));
    const std::uint64_t generation = store_->generation();
    QPointer<AccountService> self(this);
    transport_->send(req, this, [self, generation, done](const AuthReply &reply) {
        if (!self) return;
        if (!reply.setCookies.empty())   // ACC-08: fold rotations; a stale generation is ignored by the store
            self->store_->applyRotation(parseSetCookies(reply.setCookies, self->clock_() / 1000), generation);
        done(reply);
    });
}

void AccountService::post(const QString &endpoint, const QJsonObject &body,
                          std::function<void(bool ok, int status, const QJsonObject &json)> done) {
    sendJson(endpoint, body, sign(), [done](const AuthReply &reply) {
        const QJsonDocument doc = QJsonDocument::fromJson(reply.body);
        done(reply.status == 200 && doc.isObject(), reply.status, doc.object());
    });
}

void AccountService::browse(const QJsonObject &body, const QMap<QByteArray, QByteArray> &headers, Done done) {
    // IMP-02 sends its own identity headers; they replace the stored ones.
    AccountSelection sel = store_->selection();
    if (headers.contains("X-Goog-AuthUser")) sel.authUser = headers.value("X-Goog-AuthUser").toInt();
    sel.pageId = headers.value("X-Goog-PageId").toStdString();
    sendJson(QStringLiteral("browse"), body, headersFor(sel, true), [done](const AuthReply &reply) {
        const QJsonDocument doc = QJsonDocument::fromJson(reply.body);
        done(reply.status == 200 && doc.isObject(), doc.object());
    });
}

Verification AccountService::parseAccountMenu(int status, const QJsonObject &json, QString *accountName) {
    if (status != 200 || json.isEmpty()) return Verification::Inconclusive;
    const QJsonObject header = findObject(json, QStringLiteral("activeAccountHeaderRenderer"));
    const QString name = text(header.value(QStringLiteral("accountName")));
    if (name.isEmpty()) return Verification::Rejected; // 200 with anonymous content
    if (accountName) *accountName = name;
    return Verification::Accepted;
}

std::vector<AccountEntry> AccountService::parseAccountsList(const QJsonObject &json, int authUser, bool *loggedOut) {
    const QJsonObject ctx = findObject(json, QStringLiteral("mainAppWebResponseContext"));
    if (loggedOut) *loggedOut = ctx.value(QStringLiteral("loggedOut")).toBool();
    QList<QJsonObject> items;
    collectObjects(json, QStringLiteral("accountItem"), QStringLiteral("accountItemRenderer"), &items);
    std::vector<AccountEntry> out;
    for (const QJsonObject &item : items) {
        AccountEntry e;
        e.authUser = authUser;
        e.name = text(item.value(QStringLiteral("accountName"))).toStdString();
        e.handle = text(item.value(QStringLiteral("channelHandle"))).toStdString();
        e.photoUrl = lastThumbnail(item).toStdString();
        e.hasChannel = !item.contains(QStringLiteral("hasChannel")) || item.value(QStringLiteral("hasChannel")).toBool();
        e.pageId = findObject(item, QStringLiteral("pageIdToken")).value(QStringLiteral("pageId")).toString().toStdString();
        e.datasyncId = findObject(item, QStringLiteral("datasyncIdToken")).value(QStringLiteral("datasyncId")).toString().toStdString();
        e.gaiaId = findObject(item, QStringLiteral("accountStateToken")).value(QStringLiteral("obfuscatedGaiaId")).toString().toStdString();
        if (e.name.empty() && e.gaiaId.empty() && e.datasyncId.empty()) continue;
        out.push_back(e);
    }
    return out;
}

std::vector<SetCookieValue> AccountService::parseSetCookies(const std::vector<QByteArray> &headerValues, qint64 nowSec) {
    std::vector<SetCookieValue> out;
    for (const QByteArray &line : headerValues) {
        for (const QNetworkCookie &c : QNetworkCookie::parseCookies(line)) {
            SetCookieValue v;
            v.name = c.name().toStdString();
            v.value = c.value().toStdString();
            v.domain = c.domain().toStdString();
            if (c.expirationDate().isValid()) {
                v.expiresSec = c.expirationDate().toMSecsSinceEpoch() / 1000;
                v.expired = v.expiresSec <= nowSec;
            }
            if (!v.name.empty()) out.push_back(v);
        }
    }
    return out;
}

void AccountService::verifyNow() {
    if (!store_->signedIn()) { setState(SessionState::SignedOut); return; }
    if (verifying_) return;
    verifying_ = true;
    ++verificationsSent_;
    lastCheckMs_ = clock_();
    const std::uint64_t generation = store_->generation();
    QPointer<AccountService> self(this);
    sendJson(QStringLiteral("account/account_menu"), QJsonObject(), sign(), [self, generation](const AuthReply &reply) {
        if (!self) return;
        self->verifying_ = false;
        QString name;
        const QJsonDocument doc = QJsonDocument::fromJson(reply.body);
        const Verification v = parseAccountMenu(reply.status, doc.object(), &name);
        self->onVerified(v, name, generation);
    });
}

void AccountService::onVerified(Verification v, const QString &name, std::uint64_t generation) {
    if (generation != store_->generation() || !store_->signedIn()) return; // signed out or re-imported meanwhile
    if (v == Verification::Accepted && !name.isEmpty() && name != accountName_) accountName_ = name;
    const SessionState next = stateAfter(state_, v);
    if (next != state_) setState(next);
    else if (v == Verification::Accepted) emit stateChanged(); // name may have changed
}

void AccountService::pollSchedule() {
    if (!store_->signedIn() || verifying_) return;
    // A session that could not be reached is retried on the same cadence.
    const SessionState s = state_ == SessionState::Unreachable ? SessionState::SignedIn : state_;
    const std::int64_t due = nextVerificationMs(s, lastCheckMs_);
    if (due >= 0 && clock_() >= due) verifyNow();
}

void AccountService::sessionImported() {
    accounts_.clear();
    accountName_.clear();
    emit accountsChanged();
    emit cachesInvalidated();
    setState(SessionState::Checking);
    verifyNow();
    probeAccounts();
}

void AccountService::probeAccounts() {
    if (!store_->signedIn() || probing_) return;
    probing_ = true;
    probeIndex(0, std::vector<AccountEntry>(), std::vector<std::string>());
}

void AccountService::probeIndex(int index, std::vector<AccountEntry> found, std::vector<std::string> keys) {
    AccountSelection sel;
    sel.authUser = index;
    const std::uint64_t generation = store_->generation();
    QPointer<AccountService> self(this);
    sendJson(QStringLiteral("account/accounts_list"), QJsonObject(), headersFor(sel, false),
             [self, index, found, keys, generation](const AuthReply &reply) mutable {
        if (!self) return;
        if (generation != self->store_->generation()) { self->probing_ = false; return; }
        const QJsonDocument doc = QJsonDocument::fromJson(reply.body);
        if (reply.status != 200 || !doc.isObject()) {
            // ACC-09: a network failure on probe 0 is "unreachable", never "signed out".
            if (index == 0 && self->state_ != SessionState::Expired) self->setState(SessionState::Unreachable);
            self->probing_ = false;
            if (index > 0) { self->accounts_ = found; emit self->accountsChanged(); }
            return;
        }
        bool loggedOut = false;
        const std::vector<AccountEntry> entries = parseAccountsList(doc.object(), index, &loggedOut);
        if (probeShouldStop(loggedOut, entries, keys) || index >= limits::maxAuthUserProbe) {
            if (!probeShouldStop(loggedOut, entries, keys)) {
                for (const AccountEntry &e : entries) {
                    const std::string k = accountKey(e);
                    if (std::find(keys.begin(), keys.end(), k) == keys.end()) { keys.push_back(k); found.push_back(e); }
                }
            }
            self->probing_ = false;
            self->accounts_ = found;
            emit self->accountsChanged();
            return;
        }
        for (const AccountEntry &e : entries) {
            const std::string k = accountKey(e);
            if (std::find(keys.begin(), keys.end(), k) == keys.end()) { keys.push_back(k); found.push_back(e); }
        }
        self->probeIndex(index + 1, found, keys);
    });
}

bool AccountService::selectAccount(int index) {
    if (index < 0 || index >= static_cast<int>(accounts_.size())) return false;
    AccountSelection sel;
    sel.authUser = accounts_[static_cast<std::size_t>(index)].authUser;
    sel.pageId = accounts_[static_cast<std::size_t>(index)].pageId;
    if (!store_->setSelection(sel)) return false;
    emit cachesInvalidated();   // ACC-09: resolve and personalised caches
    emit accountsChanged();
    setState(SessionState::Checking);
    verifyNow();                // ACC-07 "after an account switch"
    return true;
}

void AccountService::signOut() {
    store_->signOut();
    accounts_.clear();
    accountName_.clear();
    lastCheckMs_ = -1;
    verifying_ = false;
    probing_ = false;
    emit accountsChanged();
    emit cachesInvalidated();
    setState(SessionState::SignedOut);
}

bool AccountService::canSign() const {
    return store_->signedIn() && (state_ == SessionState::SignedIn || state_ == SessionState::Unreachable);
}

void AccountService::sendSigned(const AuthRequest &request, QObject *context, std::function<void(const AuthReply &)> done) {
    QPointer<QObject> guard(context);
    const bool hasContext = context != nullptr;
    if (!canSign()) {
        QTimer::singleShot(0, this, [done, guard, hasContext] { if (!hasContext || guard) done(AuthReply()); });
        return;
    }
    AuthRequest req = request;
    QList<QPair<QByteArray, QByteArray>> kept;
    for (const auto &h : req.headers) {
        const QByteArray n = h.first.toLower();
        if (n == "cookie" || n == "authorization" || n.startsWith("x-goog-auth") || n == "x-goog-pageid") continue;
        kept.append(h);
    }
    req.headers = kept;
    for (const auto &h : sign()) req.headers.append(qMakePair(h.first, h.second));
    const std::uint64_t generation = store_->generation();
    QPointer<AccountService> self(this);
    transport_->send(req, this, [self, generation, done, guard, hasContext](const AuthReply &reply) {
        if (self && !reply.setCookies.empty())
            self->store_->applyRotation(parseSetCookies(reply.setCookies, self->clock_() / 1000), generation);
        if (hasContext && !guard) return;
        done(reply);
    });
}

AccountService::ExtractorCall AccountService::beginExtractorCall() const {
    ExtractorCall call;
    if (!canSign()) return call;
    call.generation = store_->generation();
    call.jarPath = store_->writeExtractorJar();
    return call;
}

std::vector<SetCookieValue> AccountService::parseNetscapeJar(const QByteArray &text, qint64 nowSec) {
    std::vector<SetCookieValue> out;
    for (QByteArray line : text.split('\n')) {
        line = line.trimmed();
        if (line.startsWith("#HttpOnly_")) line = line.mid(10);
        else if (line.isEmpty() || line.startsWith('#')) continue;
        const QList<QByteArray> f = line.split('\t');
        if (f.size() < 7) continue;
        SetCookieValue v;
        v.domain = f.at(0).toStdString();
        v.expiresSec = f.at(4).toLongLong();
        v.name = f.at(5).toStdString();
        v.value = f.at(6).toStdString();
        v.expired = v.expiresSec > 0 && v.expiresSec <= nowSec;
        if (!v.name.empty()) out.push_back(v);
    }
    return out;
}

void AccountService::endExtractorCall(const ExtractorCall &call) {
    if (call.jarPath.isEmpty()) return;
    QFile file(call.jarPath);
    if (file.open(QIODevice::ReadOnly)) {
        const std::vector<SetCookieValue> after = parseNetscapeJar(file.readAll(), clock_() / 1000);
        file.close();
        // Only values that differ are rotations; names the extractor dropped are kept.
        const SessionCookies before = store_->session();
        std::vector<SetCookieValue> changed;
        for (const SetCookieValue &v : after) {
            const Cookie *c = before.find(v.name);
            if (!c ? isTrackedCookie(v.name) : (c->value != v.value || v.expired)) changed.push_back(v);
        }
        if (!changed.empty()) store_->applyRotation(changed, call.generation);
    }
    AccountStore::removeExtractorJar(call.jarPath);
}

}}}
