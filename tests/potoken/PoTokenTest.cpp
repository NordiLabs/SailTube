// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Qt-free checks of the RES-14 PO-token policy: the Deno command lines, version and lock pinning,
// the state machine with backoff, the loopback bind verdict over /proc/net/tcp{,6} fixtures and
// the yt-dlp arguments. Build: tests/potoken/run.sh
#include "Core/Resolver/Domain/PoTokenPolicy.h"

#include <algorithm>
#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

using namespace nordtile::video::resolver::potoken;

static int failures = 0;
static int checks = 0;
#define CHECK(c) do { ++checks; if (!(c)) { std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); ++failures; } } while (0)

static std::string fixtures;

static std::string fixture(const char *name) {
    std::ifstream in((fixtures + "/" + name).c_str());
    std::ostringstream s;
    s << in.rdbuf();
    if (!in) std::printf("FAIL missing fixture %s\n", name);
    return s.str();
}

static bool has(const std::vector<std::string> &v, const std::string &x) {
    return std::find(v.begin(), v.end(), x) != v.end();
}

static bool hasPrefix(const std::vector<std::string> &v, const std::string &prefix) {
    for (const std::string &x : v)
        if (x.compare(0, prefix.size(), prefix) == 0) return true;
    return false;
}

static Pin testPin() {
    Pin p;
    p.providerTag = "1.2.3";
    p.providerCommit = std::string(40, 'a');
    p.lockfileSha256 = std::string(64, 'b');
    p.pluginSha256 = std::string(64, 'c');
    p.denoVersion = "2.4.5";
    p.denoSha256 = std::string(64, 'd');
    return p;
}

static Staged stagedFor(const Pin &p) {
    Staged s = parseStamp(formatStamp(p));
    s.present = true;
    s.lockfileSha256 = p.lockfileSha256;
    s.pluginSha256 = p.pluginSha256;
    s.denoSha256 = p.denoSha256;
    return s;
}

static std::vector<SocketEntry> table(const char *v4, const char *v6) {
    std::vector<SocketEntry> e = parseProcNetTcp(fixture(v4), false);
    const std::vector<SocketEntry> e6 = parseProcNetTcp(fixture(v6), true);
    e.insert(e.end(), e6.begin(), e6.end());
    return e;
}

// RES-14: network unrestricted, read only its own directory, deny write/run/FFI, heap ceiling,
// WebGPU stubbed, bound to 127.0.0.1.
static void res14_denoCommandLine() {
    Layout l;
    l.root = "/home/defaultuser/.local/share/org.nordilabs.sailtube/SailTube/potoken";
    const Command c = serveCommand(l, 40123, 384);
    const std::vector<std::string> &a = c.arguments;
    CHECK(c.program == l.root + "/deno/deno");
    CHECK(!a.empty() && a[0] == "run");
    CHECK(has(a, "--allow-net"));                  // bare: unrestricted
    CHECK(!hasPrefix(a, "--allow-net="));
    CHECK(has(a, "--allow-read=" + l.root));       // exactly its own directory
    int reads = 0;
    for (const std::string &x : a) if (x.compare(0, 12, "--allow-read") == 0) ++reads;
    CHECK(reads == 1);
    CHECK(has(a, "--deny-write"));
    CHECK(has(a, "--deny-run"));
    CHECK(has(a, "--deny-ffi"));
    CHECK(!hasPrefix(a, "--allow-write"));
    CHECK(!hasPrefix(a, "--allow-run"));
    CHECK(!hasPrefix(a, "--allow-ffi"));
    CHECK(!hasPrefix(a, "--allow-all"));
    CHECK(!has(a, "-A"));
    CHECK(!hasPrefix(a, "--allow-scripts"));
    CHECK(has(a, "--no-prompt"));
    CHECK(has(a, "--frozen"));
    CHECK(has(a, "--cached-only"));
    CHECK(has(a, "--v8-flags=--max-old-space-size=384"));
    CHECK(has(a, "--preload=" + l.webGpuStub()));
    CHECK(l.webGpuStub().compare(0, l.root.size() + 1, l.root + "/") == 0); // inside the readable root
    // Permission flags come before the module; the server's own options after it.
    const std::size_t module = std::find(a.begin(), a.end(), l.mainModule()) - a.begin();
    const std::size_t deny = std::find(a.begin(), a.end(), std::string("--deny-ffi")) - a.begin();
    CHECK(module < a.size() && deny < module);
    CHECK(module + 4 < a.size() + 1 && a[module + 1] == "--port" && a[module + 2] == "40123");
    CHECK(a[module + 3] == "--host" && a[module + 4] == "127.0.0.1");
    CHECK(c.workingDirectory == l.serverDir());
    // A complete, minimal environment: the helper's own home and cache; no PATH, no proxy.
    bool home = false, cache = false, path = false;
    for (const auto &kv : c.environment) {
        home = home || (kv.first == "HOME" && kv.second == l.root);
        cache = cache || (kv.first == "DENO_DIR" && kv.second == l.denoDir());
        path = path || kv.first == "PATH" || kv.first.find("PROXY") != std::string::npos
            || kv.first.find("proxy") != std::string::npos;
    }
    CHECK(home && cache && !path);
    // Heap ceiling is clamped.
    CHECK(clampHeapMb(10) == minimumHeapMb);
    CHECK(clampHeapMb(100000) == maximumHeapMb);
    CHECK(clampHeapMb(defaultHeapMb) == 512);
    CHECK(has(serveCommand(l, 1, 1).arguments, "--v8-flags=--max-old-space-size=128"));
    // WebGPU adapter requests answer null.
    const std::string stub = webGpuStubSource();
    CHECK(stub.find("requestAdapter") != std::string::npos);
    CHECK(stub.find("Promise.resolve(null)") != std::string::npos);
}

// RES-14: install with a frozen lockfile only; there is no unlocked variant at all.
static void res14_frozenInstallOnly() {
    Layout l;
    l.root = "/data/potoken";
    const Command c = installCommand(l);
    CHECK(c.program == "/data/potoken/deno/deno");
    CHECK(c.arguments.size() == 2 && c.arguments[0] == "install" && c.arguments[1] == "--frozen");
    CHECK(!hasPrefix(c.arguments, "--allow-scripts"));
    CHECK(c.workingDirectory == "/data/potoken/provider/server");
    CHECK(validRoot("/data/potoken"));
    CHECK(!validRoot("data/potoken"));
    CHECK(!validRoot("/data/../etc"));
    CHECK(!validRoot("/data/a,b"));          // would split Deno's path list
    CHECK(!validRoot("/data/potoken/"));
    CHECK(!validRoot("/"));
}

// RES-14: pin by tag AND commit, lockfile, plugin and Deno hashes; tag-only refused.
static void res14_pinRefusals() {
    const Pin p = testPin();
    CHECK(checkPin(p, stagedFor(p)) == Reason::None);
    CHECK(checkPin(builtInPin(), stagedFor(p)) == Reason::NotPinned); // this build: not reviewed yet
    Pin tagOnly = p;
    tagOnly.providerCommit.clear();
    CHECK(checkPin(tagOnly, stagedFor(p)) == Reason::NotPinned);
    Pin shortCommit = p;
    shortCommit.providerCommit = "abc123";
    CHECK(checkPin(shortCommit, stagedFor(p)) == Reason::NotPinned);
    Pin upper = p;
    upper.lockfileSha256 = std::string(64, 'B');
    CHECK(checkPin(upper, stagedFor(p)) == Reason::NotPinned);
    Pin noLock = p;
    noLock.lockfileSha256.clear();
    CHECK(checkPin(noLock, stagedFor(p)) == Reason::NotPinned);
    Staged missing = stagedFor(p);
    missing.present = false;
    CHECK(checkPin(p, missing) == Reason::NotStaged);
    Staged s = stagedFor(p);
    s.providerTag = "1.2.4";
    CHECK(checkPin(p, s) == Reason::TagMismatch);
    s = stagedFor(p);
    s.providerCommit = std::string(40, 'e');           // same tag, moved commit
    CHECK(checkPin(p, s) == Reason::CommitMismatch);
    s = stagedFor(p);
    s.lockfileSha256 = std::string(64, 'e');
    CHECK(checkPin(p, s) == Reason::LockfileMismatch);
    s = stagedFor(p);
    s.pluginSha256 = std::string(64, 'e');
    CHECK(checkPin(p, s) == Reason::PluginMismatch);
    s = stagedFor(p);
    s.denoSha256 = std::string(64, 'e');
    CHECK(checkPin(p, s) == Reason::DenoMismatch);
    s = stagedFor(p);
    s.denoVersion = "2.4.6";
    CHECK(checkPin(p, s) == Reason::DenoMismatch);
    Pin old = p;
    old.denoVersion = "2.3.9";
    CHECK(checkPin(old, stagedFor(old)) == Reason::DenoTooOld);
    CHECK(denoSupportsPreload("2.4.0") && denoSupportsPreload("3.0") && !denoSupportsPreload("1.46.3"));
    // Stamp round trip and install marker.
    const Staged st = parseStamp("# staged\ntag = 1.2.3\ncommit=" + std::string(40, 'a') + "\ndeno=2.4.5\nother=x\n");
    CHECK(st.providerTag == "1.2.3" && st.providerCommit == std::string(40, 'a') && st.denoVersion == "2.4.5");
    CHECK(installCurrent(formatInstallMarker(p), p));
    Pin moved = p;
    moved.lockfileSha256 = std::string(64, 'f');
    CHECK(!installCurrent(formatInstallMarker(p), moved));
    CHECK(!installCurrent("", p));
    CHECK(!installCurrent(formatInstallMarker(p), builtInPin()));
}

// RES-14: aarch64 only, fixed reason elsewhere.
static void res14_aarch64Only() {
    CHECK(checkArchitecture("aarch64") == Reason::None);
    CHECK(checkArchitecture("arm64") == Reason::None);
    CHECK(checkArchitecture("arm") == Reason::UnsupportedArchitecture);
    CHECK(checkArchitecture("armv7l") == Reason::UnsupportedArchitecture);
    CHECK(checkArchitecture("i386") == Reason::UnsupportedArchitecture);
    CHECK(checkArchitecture("x86_64") == Reason::UnsupportedArchitecture);
    CHECK(checkArchitecture("") == Reason::UnsupportedArchitecture);
    Machine m;
    CHECK(m.enable(0, "armv7l") == Action::None);
    CHECK(m.state() == State::Refused && m.reason() == Reason::UnsupportedArchitecture);
    CHECK(std::string(reasonText(m.reason())).find("aarch64") != std::string::npos);
}

// RES-14 state machine: off -> installing -> starting -> verifying -> ready / refused / failed.
static void res14_stateMachine() {
    Timing t;
    Machine m(t);
    CHECK(m.state() == State::Off && m.reason() == Reason::Disabled && !m.ready());
    CHECK(m.enable(1000, "aarch64") == Action::CheckFiles);
    CHECK(m.state() == State::Installing && m.reason() == Reason::Installing);
    CHECK(m.enable(1000, "aarch64") == Action::None);            // already active
    CHECK(m.filesChecked(Reason::None, false, 1100) == Action::Install);
    CHECK(m.state() == State::Installing);
    CHECK(m.installFinished(true, true, 2000) == Action::Start);
    CHECK(m.state() == State::Starting && m.reason() == Reason::Starting);
    CHECK(m.started(true, 2100) == Action::Poll);
    CHECK(m.state() == State::Verifying && m.reason() == Reason::Verifying);
    CHECK(m.verifyDeadlineMs() == 2100 + t.verifyTimeoutMs);
    CHECK(m.bindChecked(BindVerdict::NotListening, 2400) == Action::Poll); // keep waiting
    CHECK(m.state() == State::Verifying);
    CHECK(m.bindChecked(BindVerdict::LoopbackOnly, 2600) == Action::None);
    CHECK(m.ready() && m.reason() == Reason::None);
    CHECK(m.bindChecked(BindVerdict::LoopbackOnly, 32600) == Action::None); // re-check while ready
    CHECK(m.ready());
    // A later non-loopback bind while ready: stop and refuse.
    CHECK(m.bindChecked(BindVerdict::NonLoopback, 62600) == Action::Stop);
    CHECK(m.state() == State::Refused && m.reason() == Reason::NonLoopbackBind);
    CHECK(m.tick(10000000) == Action::None);                       // refusals never retry
    CHECK(m.exited(62700) == Action::None);
    // Only the user's toggle clears a refusal.
    CHECK(m.disable() == Action::None);
    CHECK(m.state() == State::Off && m.reason() == Reason::Disabled);
    CHECK(m.enable(70000, "aarch64") == Action::CheckFiles);
    CHECK(m.filesChecked(Reason::None, true, 70100) == Action::Start); // install current: no reinstall
    CHECK(m.started(true, 70200) == Action::Poll);
    CHECK(m.bindChecked(BindVerdict::NonLoopback, 70300) == Action::Stop);
    CHECK(m.state() == State::Refused);
    // Verify window elapses without a listener: failed with backoff, process stopped.
    Machine v(t);
    v.enable(0, "aarch64");
    v.filesChecked(Reason::None, true, 0);
    v.started(true, 0);
    CHECK(v.bindChecked(BindVerdict::NotListening, t.verifyTimeoutMs) == Action::Stop);
    CHECK(v.state() == State::Failed && v.reason() == Reason::NotListening);
    CHECK(v.retryAtMs() == t.verifyTimeoutMs + 5000);
    // Unreadable socket tables fail closed.
    Machine u(t);
    u.enable(0, "aarch64");
    u.filesChecked(Reason::None, true, 0);
    u.started(true, 0);
    CHECK(u.bindChecked(BindVerdict::Unavailable, 10) == Action::Stop);
    CHECK(u.state() == State::Refused && u.reason() == Reason::VerifyUnavailable);
    // Pin refusal in installing: nothing runs.
    Machine r(t);
    r.enable(0, "aarch64");
    CHECK(r.filesChecked(Reason::CommitMismatch, false, 1) == Action::None);
    CHECK(r.state() == State::Refused && r.reason() == Reason::CommitMismatch);
    // Disabling a running helper stops it.
    Machine d(t);
    d.enable(0, "aarch64");
    d.filesChecked(Reason::None, true, 0);
    d.started(true, 0);
    d.bindChecked(BindVerdict::LoopbackOnly, 1);
    CHECK(d.disable() == Action::Stop);
    CHECK(d.state() == State::Off);
    // Every state has its fixed name and reason text.
    const State states[] = { State::Off, State::Installing, State::Starting, State::Verifying, State::Ready,
                             State::Refused, State::Failed };
    for (State s : states) CHECK(std::string(stateName(s)) != "unknown");
    for (int i = 0; i <= static_cast<int>(Reason::VerifyUnavailable); ++i)
        CHECK(std::string(reasonText(static_cast<Reason>(i))) != "unknown");
}

// RES-14: failures back off; a crash loop does not reset the count; a stable run does.
static void res14_backoff() {
    CHECK(backoffMs(0) == 0);
    CHECK(backoffMs(1) == 5000 && backoffMs(2) == 15000 && backoffMs(3) == 60000);
    CHECK(backoffMs(4) == 300000 && backoffMs(5) == 900000 && backoffMs(50) == 900000);
    Timing t;
    Machine m(t);
    m.enable(0, "aarch64");
    m.filesChecked(Reason::None, true, 0);
    CHECK(m.started(false, 100) == Action::Stop);
    CHECK(m.state() == State::Failed && m.reason() == Reason::StartFailed && m.failures() == 1);
    CHECK(m.retryAtMs() == 5100);
    CHECK(m.tick(5000) == Action::None);
    CHECK(m.tick(5100) == Action::CheckFiles);                     // retry re-checks the pin first
    CHECK(m.state() == State::Installing);
    m.filesChecked(Reason::None, true, 5100);
    m.started(true, 5200);
    m.bindChecked(BindVerdict::LoopbackOnly, 5300);
    CHECK(m.exited(6000) == Action::None);                         // crash soon after ready
    CHECK(m.state() == State::Failed && m.reason() == Reason::Exited && m.failures() == 2);
    CHECK(m.retryAtMs() == 6000 + 15000);
    m.tick(21000);
    m.filesChecked(Reason::None, true, 21000);
    m.started(true, 21000);
    m.bindChecked(BindVerdict::LoopbackOnly, 21000);
    CHECK(m.exited(21000 + t.stableMs) == Action::None);           // after a stable run
    CHECK(m.failures() == 1 && m.retryAtMs() == 21000 + t.stableMs + 5000);
    // Ready helper that stops listening: failed, stopped.
    Machine s(t);
    s.enable(0, "aarch64");
    s.filesChecked(Reason::None, true, 0);
    s.started(true, 0);
    s.bindChecked(BindVerdict::LoopbackOnly, 0);
    CHECK(s.bindChecked(BindVerdict::NotListening, 100) == Action::Stop);
    CHECK(s.state() == State::Failed && s.reason() == Reason::StoppedListening);
}

// RES-14: install failures retry with the lock still frozen, give up after the cap, and an
// install that touches the lockfile is refused, never retried unlocked.
static void res14_installRetriesFrozenOnly() {
    Timing t;
    t.maxInstallAttempts = 2;
    Machine m(t);
    m.enable(0, "aarch64");
    CHECK(m.filesChecked(Reason::None, false, 0) == Action::Install);
    CHECK(m.installFinished(false, true, 10) == Action::None);
    CHECK(m.state() == State::Failed && m.reason() == Reason::InstallFailed && m.retryAtMs() == 5010);
    CHECK(m.tick(5010) == Action::CheckFiles);
    CHECK(m.filesChecked(Reason::None, false, 5010) == Action::Install); // the same frozen command
    CHECK(m.installFinished(false, true, 5020) == Action::None);
    CHECK(m.state() == State::Refused && m.reason() == Reason::InstallGaveUp);
    Machine l(t);
    l.enable(0, "aarch64");
    l.filesChecked(Reason::None, false, 0);
    CHECK(l.installFinished(true, false, 10) == Action::None);
    CHECK(l.state() == State::Refused && l.reason() == Reason::LockfileChanged);
    CHECK(l.tick(1000000) == Action::None);
}

// RES-14 / NFR-SEC-05: listening only on 127.0.0.1, checked against /proc/net/tcp{,6} fixtures.
static void res14_bindVerification() {
    const std::set<unsigned long long> none;
    const std::vector<SocketEntry> lo = parseProcNetTcp(fixture("tcp_loopback"), false);
    CHECK(lo.size() == 3);
    CHECK(lo[0].port == 4416 && lo[0].state == 0x0A && lo[0].inode == 5001 && isLoopbackV4(lo[0]));
    CHECK(lo[1].address[0] == 192 && lo[1].address[1] == 168 && lo[1].address[3] == 23 && lo[1].state == 1);
    CHECK(checkBind(table("tcp_loopback", "tcp6_empty"), 4416, none) == BindVerdict::LoopbackOnly);
    std::set<unsigned long long> helper;
    helper.insert(5001);
    helper.insert(5002); // its outbound connection: not a listener, ignored
    CHECK(checkBind(table("tcp_loopback", "tcp6_empty"), 4416, helper) == BindVerdict::LoopbackOnly);
    // Another process's loopback listener on the port is not the helper.
    std::set<unsigned long long> other;
    other.insert(9999);
    CHECK(checkBind(table("tcp_loopback", "tcp6_empty"), 4416, other) == BindVerdict::NotListening);
    CHECK(checkBind(table("tcp_empty", "tcp6_empty"), 4416, none) == BindVerdict::NotListening);
    CHECK(checkBind(table("tcp_loopback", "tcp6_empty"), 4417, none) == BindVerdict::NotListening);
    // 0.0.0.0, ::, a LAN address (v4 and v6): refused.
    CHECK(checkBind(table("tcp_any", "tcp6_empty"), 4416, none) == BindVerdict::NonLoopback);
    CHECK(checkBind(table("tcp_empty", "tcp6_any"), 4416, none) == BindVerdict::NonLoopback);
    CHECK(checkBind(table("tcp_loopback", "tcp6_any"), 4416, none) == BindVerdict::NonLoopback);
    CHECK(checkBind(table("tcp_lan", "tcp6_empty"), 4416, none) == BindVerdict::NonLoopback);
    CHECK(checkBind(table("tcp_empty", "tcp6_lan"), 4416, none) == BindVerdict::NonLoopback);
    // Only 127.0.0.1: not ::1, not 127.1.0.1; the v4-mapped 127.0.0.1 is the same address.
    CHECK(checkBind(table("tcp_loopback", "tcp6_v6loopback"), 4416, none) == BindVerdict::NonLoopback);
    CHECK(checkBind(table("tcp_loopback_other", "tcp6_empty"), 4416, none) == BindVerdict::NonLoopback);
    CHECK(checkBind(table("tcp_empty", "tcp6_mapped_loopback"), 4416, none) == BindVerdict::LoopbackOnly);
    // A second listener of the helper on another port, on all interfaces: refused.
    std::set<unsigned long long> two;
    two.insert(5001);
    two.insert(5003);
    CHECK(checkBind(table("tcp_helper_second_port", "tcp6_empty"), 4416, two) == BindVerdict::NonLoopback);
    CHECK(checkBind(table("tcp_helper_second_port", "tcp6_empty"), 4416, helper) == BindVerdict::LoopbackOnly);
    // Big-endian printing and malformed lines.
    const std::vector<SocketEntry> be = parseProcNetTcp("  0: 7F000001:1140 00000000:0000 0A 0:0 0:0 0 1 0 77\n", false, false);
    CHECK(be.size() == 1 && isLoopbackV4(be[0]) && be[0].inode == 77);
    CHECK(parseProcNetTcp("garbage\n  0: 7F00ZZ01:1140 00000000:0000 0A 0:0 0:0 0 1 0 77\n", false).empty());
    CHECK(parseProcNetTcp("  0: 0100007F:1140 00000000:0000 0A 0:0 0:0 0 1 0\n", false).empty());
    CHECK(socketInode("socket:[12345]") == 12345);
    CHECK(socketInode("pipe:[12345]") == 0);
    CHECK(socketInode("socket:[]") == 0);
    CHECK(socketInode("/dev/null") == 0);
}

// RES-08 step 4, RES-14: provider arguments only when ready AND requested; never cookies.
static void res14_extractorArgsOnlyWhenReadyAndRequested() {
    const std::string dir = "/data/potoken/yt-dlp-plugins";
    CHECK(extractorArguments(false, false, dir, 40123).empty());
    CHECK(extractorArguments(true, false, dir, 40123).empty());
    CHECK(extractorArguments(false, true, dir, 40123).empty());
    CHECK(extractorArguments(true, true, dir, 0).empty());
    const std::vector<std::string> a = extractorArguments(true, true, dir, 40123);
    CHECK(a.size() == 4);
    CHECK(a.size() == 4 && a[0] == "--plugin-dirs" && a[1] == dir);
    CHECK(a.size() == 4 && a[2] == "--extractor-args" && a[3] == "youtubepot-bgutilhttp:base_url=http://127.0.0.1:40123");
    for (const std::string &x : a) CHECK(x.find("cookie") == std::string::npos);
    CHECK(baseUrl(4416) == "http://127.0.0.1:4416");
    CHECK(std::string(forcedTokenArgument()) == "fetch_pot=always");
}

// RES-14 pin fields: tag AND commit hash, lockfile/plugin/binary SHA-256, Deno version, layout.
static void res14_pinFieldValidation() {
    CHECK(validSha256(std::string(64, 'a')) && !validSha256(std::string(63, 'a')) && !validSha256(std::string(64, 'A')));
    CHECK(!validSha256(std::string(63, 'a') + "g"));
    CHECK(validCommit(std::string(40, '0')) && !validCommit(std::string(41, '0')) && !validCommit(std::string(40, 'Z')));
    CHECK(validTag("v1.2.3") && validTag("1.2.3-rc_1") && !validTag("") && !validTag("-v1") && !validTag(".v1"));
    CHECK(!validTag(std::string(33, 'a')) && !validTag("v1 2"));
    CHECK(validVersion("2.4.0") && !validVersion("") && !validVersion("2.4.0a") && !validVersion(std::string(33, '1')));
    Layout l;
    l.root = "/home/u/.local/share/org.nordilabs.sailtube/SailTube/potoken";
    CHECK(l.providerDir() == l.root + "/provider");
    CHECK(l.serverDir().compare(0, l.providerDir().size(), l.providerDir()) == 0);
}

int main(int argc, char **argv) {
    fixtures = argc > 1 ? argv[1] : "tests/potoken/fixtures";
    res14_denoCommandLine();
    res14_frozenInstallOnly();
    res14_pinRefusals();
    res14_aarch64Only();
    res14_stateMachine();
    res14_backoff();
    res14_installRetriesFrozenOnly();
    res14_bindVerification();
    res14_extractorArgsOnlyWhenReadyAndRequested();
    res14_pinFieldValidation();
    std::printf("potoken domain: %d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
