// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "SessionBundleCodec.h"

namespace nordtile { namespace music { namespace accounts { namespace infrastructure {
namespace {
const char kHeader[] = "ytm-session-bundle v1";

bool parseInt(const std::string &text, std::int64_t *out) {
    if (text.empty() || text.size() > 18) return false;
    std::int64_t v = 0;
    for (std::size_t i = 0; i < text.size(); ++i) { if (text[i] < '0' || text[i] > '9') return false; v = v * 10 + (text[i] - '0'); }
    *out = v; return true;
}
struct Scrub {
    std::string &a; std::string &b;
    Scrub(std::string &x, std::string &y) : a(x), b(y) {}
    ~Scrub() { domain::wipeString(a); domain::wipeString(b); }
};
std::string decimal(std::int64_t v) {
    if (v <= 0) return "0";
    std::string out;
    while (v) { out.insert(out.begin(), char('0' + (v % 10))); v /= 10; }
    return out;
}
}

bool SessionBundleCodec::encode(const domain::SessionBundle &bundle, std::string *out) {
    if (!out) return false;
    out->clear();
    if (!bundle.usable()) return false;
    std::string text = std::string(kHeader) + "\n";
    text += std::string("source=") + domain::sourceKindName(bundle.source) + "\n";
    text += "imported=" + decimal(bundle.importedAtUtcSeconds) + "\n";
    if (!bundle.authUser.empty()) {
        if (!domain::digitsOnly(bundle.authUser, 1, 2)) { domain::wipeString(text); return false; }
        text += "authuser=" + bundle.authUser + "\n";
    }
    if (!bundle.pageId.empty()) {
        if (!domain::digitsOnly(bundle.pageId, 1, 32)) { domain::wipeString(text); return false; }
        text += "pageid=" + bundle.pageId + "\n";
    }
    for (std::size_t i = 0; i < bundle.cookies.size(); ++i) {
        const domain::SessionCookie &c = bundle.cookies[i];
        if (!domain::validCookieName(c.name) || !domain::isNeededCookie(c.name) || !domain::validCookieValue(c.value)) {
            domain::wipeString(text); return false;
        }
        text += "c." + c.name + "=" + c.value + "\n";
        if (text.size() > std::size_t(kMaxBytes)) { domain::wipeString(text); return false; }
    }
    if (text.size() > std::size_t(kMaxBytes)) { domain::wipeString(text); return false; }
    *out = text;
    domain::wipeString(text);
    return true;
}

BundleDecode SessionBundleCodec::decode(const char *data, std::size_t size, domain::SessionBundle *out) {
    if (!out) return BundleDecode::Malformed;
    out->wipe();
    if (!data || size == 0) return BundleDecode::Empty;
    if (size > std::size_t(kMaxBytes)) return BundleDecode::TooLarge;
    domain::SessionBundle bundle;
    std::size_t pos = 0; int lineNo = 0;
    while (pos < size) {
        std::size_t end = pos;
        while (end < size && data[end] != '\n') ++end;
        if (end == size) { bundle.wipe(); return BundleDecode::Malformed; }     // every line ends with \n
        std::string line(data + pos, data + end);
        std::string value;
        Scrub scrub(line, value);
        pos = end + 1; ++lineNo;
        if (lineNo == 1) {
            if (line != kHeader) { bundle.wipe(); return line.compare(0, 19, "ytm-session-bundle ") == 0 ? BundleDecode::UnsupportedVersion : BundleDecode::Malformed; }
            continue;
        }
        if (lineNo > 128) { bundle.wipe(); return BundleDecode::Malformed; }
        const std::size_t eq = line.find('=');
        if (eq == std::string::npos || eq == 0) { bundle.wipe(); return BundleDecode::Malformed; }
        const std::string key = line.substr(0, eq);
        value = line.substr(eq + 1);
        if (key.compare(0, 2, "c.") == 0) {
            const std::string name = key.substr(2);
            if (!domain::isNeededCookie(name) || !bundle.set(name, value)) { bundle.wipe(); return BundleDecode::Malformed; }
        } else if (key == "source") bundle.source = domain::parseSourceKind(value);
        else if (key == "imported") { std::int64_t t = 0; if (!parseInt(value, &t)) { bundle.wipe(); return BundleDecode::Malformed; } bundle.importedAtUtcSeconds = t; }
        else if (key == "authuser") { if (!domain::digitsOnly(value, 1, 2)) { bundle.wipe(); return BundleDecode::Malformed; } bundle.authUser = value; }
        else if (key == "pageid") { if (!domain::digitsOnly(value, 1, 32)) { bundle.wipe(); return BundleDecode::Malformed; } bundle.pageId = value; }
        // other keys: ignored on purpose
    }
    if (lineNo == 0) return BundleDecode::Empty;
    if (!bundle.usable()) { bundle.wipe(); return BundleDecode::Unusable; }
    *out = bundle;
    bundle.wipe();
    return BundleDecode::Ok;
}

}}}}
