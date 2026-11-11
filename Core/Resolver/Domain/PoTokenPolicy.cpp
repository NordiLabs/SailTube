// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "PoTokenPolicy.h"

#include <cstdlib>
#include <sstream>

namespace nordtile { namespace video { namespace resolver { namespace potoken {

namespace {
bool lowerHex(const std::string &s, std::size_t length) {
    if (s.size() != length) return false;
    for (char c : s)
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) return false;
    return true;
}

int hexValue(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

// Parses exactly 2*n hex digits into n bytes in printed order.
bool hexBytes(const std::string &s, unsigned char *out, std::size_t n) {
    if (s.size() != 2 * n) return false;
    for (std::size_t i = 0; i < n; ++i) {
        const int hi = hexValue(s[2 * i]), lo = hexValue(s[2 * i + 1]);
        if (hi < 0 || lo < 0) return false;
        out[i] = static_cast<unsigned char>(hi * 16 + lo);
    }
    return true;
}

bool parseHexNumber(const std::string &s, long long *out) {
    if (s.empty() || s.size() > 15) return false;
    long long v = 0;
    for (char c : s) {
        const int d = hexValue(c);
        if (d < 0) return false;
        v = v * 16 + d;
    }
    *out = v;
    return true;
}

std::vector<std::string> words(const std::string &line) {
    std::vector<std::string> out;
    std::istringstream in(line);
    std::string w;
    while (in >> w) out.push_back(w);
    return out;
}

std::string trimmed(const std::string &s) {
    const std::size_t a = s.find_first_not_of(" \t\r\n");
    if (a == std::string::npos) return std::string();
    const std::size_t b = s.find_last_not_of(" \t\r\n");
    return s.substr(a, b - a + 1);
}

// Splits "key=value" lines into pairs.
std::vector<std::pair<std::string, std::string>> keyValues(const std::string &text) {
    std::vector<std::pair<std::string, std::string>> out;
    std::istringstream in(text);
    std::string line;
    while (std::getline(in, line)) {
        const std::size_t eq = line.find('=');
        if (eq == std::string::npos) continue;
        out.push_back(std::make_pair(trimmed(line.substr(0, eq)), trimmed(line.substr(eq + 1))));
    }
    return out;
}

bool pinComplete(const Pin &p) {
    return validTag(p.providerTag) && validCommit(p.providerCommit) && validSha256(p.lockfileSha256)
        && validSha256(p.pluginSha256) && validVersion(p.denoVersion) && validSha256(p.denoSha256);
}
}

Pin builtInPin() {
    // Filled in only after the NFR-SEC-06 licence review of the provider (see the playback component
    // report). Until then every field is empty and the helper refuses with NotPinned.
    return Pin();
}

Staged parseStamp(const std::string &text) {
    Staged s;
    for (const auto &kv : keyValues(text)) {
        if (kv.first == "tag") s.providerTag = kv.second;
        else if (kv.first == "commit") s.providerCommit = kv.second;
        else if (kv.first == "deno") s.denoVersion = kv.second;
    }
    return s;
}

std::string formatStamp(const Pin &p) {
    return "tag=" + p.providerTag + "\ncommit=" + p.providerCommit + "\ndeno=" + p.denoVersion + "\n";
}

bool validSha256(const std::string &s) { return lowerHex(s, 64); }
bool validCommit(const std::string &s) { return lowerHex(s, 40); }

bool validTag(const std::string &s) {
    if (s.empty() || s.size() > 32 || s[0] == '-' || s[0] == '.') return false;
    for (char c : s)
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '.' || c == '-' || c == '_'))
            return false;
    return true;
}

bool validVersion(const std::string &s) {
    if (s.empty() || s.size() > 32 || s[0] == '.') return false;
    for (char c : s)
        if (!((c >= '0' && c <= '9') || c == '.')) return false;
    return true;
}

bool denoSupportsPreload(const std::string &version) {
    if (!validVersion(version)) return false;
    const int major = std::atoi(version.c_str());
    const std::size_t dot = version.find('.');
    const int minor = dot == std::string::npos ? 0 : std::atoi(version.c_str() + dot + 1);
    return major > 2 || (major == 2 && minor >= 4);
}

const char *reasonText(Reason r) {
    switch (r) {
    case Reason::None: return "ready";
    case Reason::Disabled: return "off";
    case Reason::Installing: return "checking the pinned helper";
    case Reason::Starting: return "starting the helper";
    case Reason::Verifying: return "checking that the helper listens on 127.0.0.1 only";
    case Reason::UnsupportedArchitecture: return "needs a 64-bit ARM (aarch64) device; Deno has no 32-bit build";
    case Reason::NotPinned: return "this build has no reviewed pin for the helper";
    case Reason::NotStaged: return "the helper is not installed";
    case Reason::TagMismatch: return "installed helper version differs from the pinned tag";
    case Reason::CommitMismatch: return "installed helper commit differs from the pinned commit";
    case Reason::LockfileMismatch: return "helper lockfile differs from the pinned lockfile";
    case Reason::PluginMismatch: return "yt-dlp plugin differs from the pinned plugin";
    case Reason::DenoMismatch: return "Deno runtime differs from the pinned runtime";
    case Reason::DenoTooOld: return "the pinned Deno runtime is too old";
    case Reason::InstallFailed: return "frozen install failed; retrying later";
    case Reason::InstallGaveUp: return "frozen install failed repeatedly";
    case Reason::LockfileChanged: return "the install changed the lockfile";
    case Reason::StartFailed: return "the helper did not start";
    case Reason::Exited: return "the helper stopped";
    case Reason::NotListening: return "the helper did not open its port in time";
    case Reason::StoppedListening: return "the helper closed its port";
    case Reason::NonLoopbackBind: return "the helper listens outside 127.0.0.1; refused";
    case Reason::VerifyUnavailable: return "cannot check the helper's sockets; refused";
    }
    return "unknown";
}

Reason checkArchitecture(const std::string &architecture) {
    return architecture == "aarch64" || architecture == "arm64" ? Reason::None : Reason::UnsupportedArchitecture;
}

Reason checkPin(const Pin &pin, const Staged &staged) {
    if (!pinComplete(pin)) return Reason::NotPinned;
    if (!staged.present) return Reason::NotStaged;
    if (!denoSupportsPreload(pin.denoVersion)) return Reason::DenoTooOld;
    if (staged.providerTag != pin.providerTag) return Reason::TagMismatch;
    if (staged.providerCommit != pin.providerCommit) return Reason::CommitMismatch;
    if (staged.lockfileSha256 != pin.lockfileSha256) return Reason::LockfileMismatch;
    if (staged.pluginSha256 != pin.pluginSha256) return Reason::PluginMismatch;
    if (staged.denoVersion != pin.denoVersion || staged.denoSha256 != pin.denoSha256) return Reason::DenoMismatch;
    return Reason::None;
}

std::string formatInstallMarker(const Pin &p) {
    return "commit=" + p.providerCommit + "\nlock=" + p.lockfileSha256 + "\ndeno=" + p.denoSha256 + "\n";
}

bool installCurrent(const std::string &markerText, const Pin &pin) {
    if (!pinComplete(pin)) return false;
    std::string commit, lock, deno;
    for (const auto &kv : keyValues(markerText)) {
        if (kv.first == "commit") commit = kv.second;
        else if (kv.first == "lock") lock = kv.second;
        else if (kv.first == "deno") deno = kv.second;
    }
    return commit == pin.providerCommit && lock == pin.lockfileSha256 && deno == pin.denoSha256;
}

bool validRoot(const std::string &root) {
    if (root.size() < 2 || root[0] != '/' || root[root.size() - 1] == '/') return false;
    if (root.find("/../") != std::string::npos || root.find("/./") != std::string::npos
        || root.compare(root.size() - 3, 3, "/..") == 0 || root.find("//") != std::string::npos) return false;
    for (char c : root)
        if (static_cast<unsigned char>(c) < 0x20 || c == 0x7f || c == ',') return false;
    return true;
}

int clampHeapMb(int requested) {
    if (requested < minimumHeapMb) return minimumHeapMb;
    if (requested > maximumHeapMb) return maximumHeapMb;
    return requested;
}

namespace {
std::vector<std::pair<std::string, std::string>> helperEnvironment(const Layout &l) {
    // The complete environment: nothing of the app's (no session, no proxy, no display, no
    // PATH: the helper may not run programs).
    std::vector<std::pair<std::string, std::string>> e;
    e.push_back(std::make_pair(std::string("HOME"), l.root));
    e.push_back(std::make_pair(std::string("DENO_DIR"), l.denoDir()));
    e.push_back(std::make_pair(std::string("DENO_NO_UPDATE_CHECK"), std::string("1")));
    e.push_back(std::make_pair(std::string("NO_COLOR"), std::string("1")));
    e.push_back(std::make_pair(std::string("LANG"), std::string("C.UTF-8")));
    return e;
}
}

Command installCommand(const Layout &l) {
    Command c;
    c.program = l.denoBinary();
    c.arguments.push_back("install");
    c.arguments.push_back("--frozen");
    c.workingDirectory = l.serverDir();
    c.environment = helperEnvironment(l);
    return c;
}

Command serveCommand(const Layout &l, int port, int heapMb) {
    Command c;
    c.program = l.denoBinary();
    std::vector<std::string> &a = c.arguments;
    a.push_back("run");
    a.push_back("--no-prompt");
    a.push_back("--frozen");
    a.push_back("--cached-only");
    a.push_back("--allow-net");                    // unrestricted: an allow-list killed minting
    a.push_back("--allow-env");                    // the environment is the fixed one above
    a.push_back("--allow-read=" + l.root);         // its own directory only
    a.push_back("--deny-write");
    a.push_back("--deny-run");
    a.push_back("--deny-ffi");
    std::ostringstream heap;
    heap << "--v8-flags=--max-old-space-size=" << clampHeapMb(heapMb);
    a.push_back(heap.str());
    a.push_back("--preload=" + l.webGpuStub());
    a.push_back(l.mainModule());
    std::ostringstream p;
    p << port;
    a.push_back("--port");
    a.push_back(p.str());
    a.push_back("--host");
    a.push_back("127.0.0.1");
    c.workingDirectory = l.serverDir();
    c.environment = helperEnvironment(l);
    return c;
}

const char *webGpuStubSource() {
    return "// SailTube RES-14: every WebGPU adapter request answers \"no adapter\", so GPU\n"
           "// fingerprinting never reaches the device's GPU driver.\n"
           "(() => {\n"
           "  const none = () => Promise.resolve(null);\n"
           "  let patched = false;\n"
           "  try {\n"
           "    if (typeof GPU === \"function\") {\n"
           "      Object.defineProperty(GPU.prototype, \"requestAdapter\", { value: none, configurable: true, writable: true });\n"
           "      patched = true;\n"
           "    }\n"
           "  } catch (_) {}\n"
           "  if (!patched) {\n"
           "    try {\n"
           "      if (globalThis.navigator && navigator.gpu)\n"
           "        Object.defineProperty(navigator.gpu, \"requestAdapter\", { value: none, configurable: true, writable: true });\n"
           "    } catch (_) {}\n"
           "  }\n"
           "})();\n";
}

std::vector<SocketEntry> parseProcNetTcp(const std::string &text, bool v6, bool littleEndian) {
    std::vector<SocketEntry> out;
    std::istringstream in(text);
    std::string line;
    while (std::getline(in, line)) {
        const std::vector<std::string> w = words(line);
        if (w.size() < 10 || w[0].empty() || w[0][w[0].size() - 1] != ':') continue; // header too
        const std::size_t colon = w[1].find(':');
        if (colon == std::string::npos) continue;
        SocketEntry e;
        e.v6 = v6;
        const std::size_t bytes = v6 ? 16 : 4;
        unsigned char printed[16];
        if (!hexBytes(w[1].substr(0, colon), printed, bytes)) continue;
        // Each 32-bit word is printed as a host-order number: reverse within words on LE.
        for (std::size_t word = 0; word < bytes / 4; ++word)
            for (std::size_t i = 0; i < 4; ++i)
                e.address[word * 4 + i] = littleEndian ? printed[word * 4 + (3 - i)] : printed[word * 4 + i];
        long long port = 0, state = 0;
        if (!parseHexNumber(w[1].substr(colon + 1), &port) || port > 65535) continue;
        if (!parseHexNumber(w[3], &state) || state > 0xff) continue;
        char *end = nullptr;
        const unsigned long long inode = std::strtoull(w[9].c_str(), &end, 10);
        if (!end || *end != '\0') continue;
        e.port = static_cast<int>(port);
        e.state = static_cast<int>(state);
        e.inode = inode;
        out.push_back(e);
    }
    return out;
}

bool isLoopbackV4(const SocketEntry &e) {
    const unsigned char *a = e.address;
    if (!e.v6) return a[0] == 127 && a[1] == 0 && a[2] == 0 && a[3] == 1;
    for (int i = 0; i < 10; ++i)
        if (a[i] != 0) return false;
    return a[10] == 0xff && a[11] == 0xff && a[12] == 127 && a[13] == 0 && a[14] == 0 && a[15] == 1;
}

BindVerdict checkBind(const std::vector<SocketEntry> &entries, int port, const std::set<unsigned long long> &helperInodes) {
    const int listen = 0x0A;
    bool own = false;
    for (const SocketEntry &e : entries) {
        if (e.state != listen) continue;
        const bool helpers = helperInodes.count(e.inode) > 0;
        if (e.port != port && !helpers) continue;
        if (!isLoopbackV4(e)) return BindVerdict::NonLoopback;
        if (e.port == port && (helperInodes.empty() || helpers)) own = true;
    }
    return own ? BindVerdict::LoopbackOnly : BindVerdict::NotListening;
}

unsigned long long socketInode(const std::string &target) {
    const std::string prefix = "socket:[";
    if (target.size() <= prefix.size() + 1 || target.compare(0, prefix.size(), prefix) != 0
        || target[target.size() - 1] != ']') return 0;
    const std::string digits = target.substr(prefix.size(), target.size() - prefix.size() - 1);
    if (digits.empty() || digits.size() > 19) return 0;
    for (char c : digits)
        if (c < '0' || c > '9') return 0;
    return std::strtoull(digits.c_str(), nullptr, 10);
}

std::string baseUrl(int port) {
    std::ostringstream s;
    s << "http://127.0.0.1:" << port;
    return s.str();
}

std::vector<std::string> extractorArguments(bool ready, bool requested, const std::string &pluginDir, int port) {
    std::vector<std::string> a;
    if (!ready || !requested || pluginDir.empty() || port <= 0 || port > 65535) return a;
    // The runner's common set starts with --no-plugin-dirs: only this directory is searched (RES-08).
    a.push_back("--plugin-dirs");
    a.push_back(pluginDir);
    a.push_back("--extractor-args");
    a.push_back("youtubepot-bgutilhttp:base_url=" + baseUrl(port));
    return a;
}

const char *forcedTokenArgument() { return "fetch_pot=always"; }

const char *stateName(State s) {
    switch (s) {
    case State::Off: return "off";
    case State::Installing: return "installing";
    case State::Starting: return "starting";
    case State::Verifying: return "verifying";
    case State::Ready: return "ready";
    case State::Refused: return "refused";
    case State::Failed: return "failed";
    }
    return "unknown";
}

long long backoffMs(int failures) {
    static const long long steps[] = { 5000, 15000, 60000, 300000, 900000 };
    if (failures < 1) return 0;
    if (failures > 5) return steps[4];
    return steps[failures - 1];
}

Action Machine::enable(long long nowMs, const std::string &architecture) {
    if (state_ != State::Off && state_ != State::Failed && state_ != State::Refused) return Action::None;
    (void)nowMs;
    failures_ = 0;
    installAttempts_ = 0;
    retryAtMs_ = 0;
    readySinceMs_ = -1;
    if (checkArchitecture(architecture) != Reason::None) return refuse(Reason::UnsupportedArchitecture, Action::None);
    state_ = State::Installing;
    reason_ = Reason::Installing;
    return Action::CheckFiles;
}

Action Machine::disable() {
    const bool running = state_ == State::Starting || state_ == State::Verifying || state_ == State::Ready;
    state_ = State::Off;
    reason_ = Reason::Disabled;
    failures_ = 0;
    installAttempts_ = 0;
    retryAtMs_ = 0;
    readySinceMs_ = -1;
    return running ? Action::Stop : Action::None;
}

Action Machine::filesChecked(Reason pinVerdict, bool installIsCurrent, long long nowMs) {
    (void)nowMs;
    if (state_ != State::Installing) return Action::None;
    if (pinVerdict != Reason::None) return refuse(pinVerdict, Action::None);
    if (installIsCurrent) {
        state_ = State::Starting;
        reason_ = Reason::Starting;
        return Action::Start;
    }
    if (installAttempts_ >= timing_.maxInstallAttempts) return refuse(Reason::InstallGaveUp, Action::None);
    ++installAttempts_;
    return Action::Install;
}

Action Machine::installFinished(bool ok, bool lockUnchanged, long long nowMs) {
    if (state_ != State::Installing) return Action::None;
    // Never retried unlocked: a lockfile the install touched is a refusal, not a retry.
    if (!lockUnchanged) return refuse(Reason::LockfileChanged, Action::None);
    if (ok) {
        state_ = State::Starting;
        reason_ = Reason::Starting;
        return Action::Start;
    }
    if (installAttempts_ >= timing_.maxInstallAttempts) return refuse(Reason::InstallGaveUp, Action::None);
    return fail(Reason::InstallFailed, nowMs, Action::None);
}

Action Machine::started(bool ok, long long nowMs) {
    if (state_ != State::Starting) return Action::None;
    if (!ok) return fail(Reason::StartFailed, nowMs, Action::Stop);
    state_ = State::Verifying;
    reason_ = Reason::Verifying;
    verifyDeadlineMs_ = nowMs + timing_.verifyTimeoutMs;
    return Action::Poll;
}

Action Machine::bindChecked(BindVerdict verdict, long long nowMs) {
    if (state_ != State::Verifying && state_ != State::Ready) return Action::None;
    switch (verdict) {
    case BindVerdict::NonLoopback: return refuse(Reason::NonLoopbackBind, Action::Stop);
    case BindVerdict::Unavailable: return refuse(Reason::VerifyUnavailable, Action::Stop);
    case BindVerdict::LoopbackOnly:
        if (state_ == State::Verifying) {
            state_ = State::Ready;
            reason_ = Reason::None;
            readySinceMs_ = nowMs;
        }
        return Action::None;
    case BindVerdict::NotListening:
        if (state_ == State::Ready) return fail(Reason::StoppedListening, nowMs, Action::Stop);
        if (nowMs >= verifyDeadlineMs_) return fail(Reason::NotListening, nowMs, Action::Stop);
        return Action::Poll;
    }
    return Action::None;
}

Action Machine::exited(long long nowMs) {
    if (state_ != State::Starting && state_ != State::Verifying && state_ != State::Ready) return Action::None;
    return fail(Reason::Exited, nowMs, Action::None);
}

Action Machine::tick(long long nowMs) {
    if (state_ != State::Failed || nowMs < retryAtMs_) return Action::None;
    // Every retry re-checks the staged copy against the pin before anything runs.
    state_ = State::Installing;
    reason_ = Reason::Installing;
    return Action::CheckFiles;
}

Action Machine::fail(Reason reason, long long nowMs, Action action) {
    if (state_ == State::Ready && readySinceMs_ >= 0 && nowMs - readySinceMs_ >= timing_.stableMs) failures_ = 0;
    ++failures_;
    state_ = State::Failed;
    reason_ = reason;
    readySinceMs_ = -1;
    retryAtMs_ = nowMs + backoffMs(failures_);
    return action;
}

Action Machine::refuse(Reason reason, Action action) {
    state_ = State::Refused;
    reason_ = reason;
    readySinceMs_ = -1;
    retryAtMs_ = 0;
    return action;
}

}}}}
