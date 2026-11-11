// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#pragma once

// RES-14 [FULL] optional PO-token provider: the public bgutil HTTP provider on Deno, run as a
// separate, pinned, user-enabled helper. Everything here is Qt-free and I/O-free: the pin and
// its checks, the exact Deno command lines, the loopback bind verdict over /proc/net/tcp{,6}
// text, the yt-dlp arguments and the helper's state machine. Time is passed in (milliseconds).
// The STORE build compiles none of this (PoTokenProvider and its callers are FULL only).

#include <set>
#include <string>
#include <utility>
#include <vector>

namespace nordtile { namespace video { namespace resolver { namespace potoken {

// ---- Pin (RES-14 "tag AND commit hash", frozen lockfile; NFR-SEC-04 pinned integrity) ----

// Every field is required. An empty or malformed field refuses the helper (Reason::NotPinned):
// a tag-only pin is never accepted.
struct Pin {
    std::string providerTag;      // release tag of the provider, e.g. "1.2.3"
    std::string providerCommit;   // 40 lowercase hex: the commit the tag points to
    std::string lockfileSha256;   // SHA-256 of the provider's server/deno.lock
    std::string pluginSha256;     // SHA-256 of the provider's yt-dlp plugin zip
    std::string denoVersion;      // e.g. "2.4.5" (2.4 or newer: --preload)
    std::string denoSha256;       // SHA-256 of the aarch64 deno executable
};

// This build's pin. Empty until the provider has passed the NFR-SEC-06 licence review and the
// lead fills it in, so a build without a reviewed pin refuses with NotPinned.
Pin builtInPin();

// What is on disk: the stamp the staging step wrote plus the hashes computed by the
// infrastructure. present = all helper files exist.
struct Staged {
    bool present = false;
    std::string providerTag, providerCommit, denoVersion;   // from the stamp
    std::string lockfileSha256, pluginSha256, denoSha256;   // computed
};

// Stamp file format ("key=value" lines: tag, commit, deno). Unknown keys are ignored.
Staged parseStamp(const std::string &text);
std::string formatStamp(const Pin &);

bool validSha256(const std::string &);   // 64 lowercase hex
bool validCommit(const std::string &);   // 40 lowercase hex
bool validTag(const std::string &);      // [A-Za-z0-9._-], 1..32 chars, not starting with '-' or '.'
bool validVersion(const std::string &);  // digits and dots, 1..32 chars
bool denoSupportsPreload(const std::string &version); // >= 2.4 (WebGPU stub module)

// ---- Reasons: one fixed text each; never provider output ----

enum class Reason {
    None,                    // ready
    Disabled,                // off
    Installing,              // checking the staged copy / running the frozen install
    Starting,                // helper process starting
    Verifying,               // waiting for the loopback bind check
    UnsupportedArchitecture, // not aarch64 (Deno has no 32-bit build)
    NotPinned,               // this build carries no complete pin
    NotStaged,               // helper files are missing
    TagMismatch, CommitMismatch, LockfileMismatch, PluginMismatch, DenoMismatch,
    DenoTooOld,              // pinned Deno lacks --preload
    InstallFailed,           // frozen install exited non-zero (retried later, still frozen)
    InstallGaveUp,           // frozen install failed maxInstallAttempts times
    LockfileChanged,         // the lockfile hash changed during install
    StartFailed,             // the process did not start
    Exited,                  // the helper exited or crashed
    NotListening,            // no listening socket within the verify window
    StoppedListening,        // the socket went away while ready
    NonLoopbackBind,         // a listening socket outside 127.0.0.1
    VerifyUnavailable        // the socket tables could not be read: fail closed
};
const char *reasonText(Reason);

// RES-14: aarch64 only. Accepts "aarch64" and "arm64"; everything else is refused with
// UnsupportedArchitecture.
Reason checkArchitecture(const std::string &architecture);
// RES-14 pin checks, in order: NotPinned, NotStaged, DenoTooOld, Tag/Commit/Lockfile/Plugin/Deno
// mismatch; None when the staged copy is exactly the pinned one.
Reason checkPin(const Pin &, const Staged &);

// Marker SailTube writes after a successful frozen install of exactly this pin.
std::string formatInstallMarker(const Pin &);
bool installCurrent(const std::string &markerText, const Pin &);

// ---- Layout and command lines (RES-14 Deno permissions) ----

struct Layout {
    std::string root;   // absolute helper root (private data dir); Deno may read only this
    std::string denoBinary() const { return root + "/deno/deno"; }
    std::string denoDir() const { return root + "/deno-cache"; }
    std::string providerDir() const { return root + "/provider"; }
    std::string serverDir() const { return root + "/provider/server"; }
    std::string lockfile() const { return root + "/provider/server/deno.lock"; }
    std::string mainModule() const { return root + "/provider/server/src/main.ts"; }
    std::string pluginDir() const { return root + "/yt-dlp-plugins"; }
    std::string pluginZip() const { return root + "/yt-dlp-plugins/bgutil-ytdlp-pot-provider.zip"; }
    std::string stamp() const { return root + "/provider/sailtube-pin.txt"; }
    std::string installMarker() const { return root + "/provider/sailtube-installed.txt"; }
    std::string webGpuStub() const { return root + "/sailtube/webgpu-stub.js"; }
};
bool validRoot(const std::string &root); // absolute, no "..", no ',' (Deno path lists), no control chars

struct Command {
    std::string program;
    std::vector<std::string> arguments;
    std::string workingDirectory;
    std::vector<std::pair<std::string, std::string>> environment; // the complete environment
};

// Heap ceiling in MiB for V8's old space: clamped to [minimumHeapMb, maximumHeapMb].
const int defaultHeapMb = 512;
const int minimumHeapMb = 128;
const int maximumHeapMb = 2048;
int clampHeapMb(int requested);

// The frozen install: `deno install --frozen` in the server directory. Never an unlocked
// variant and no lifecycle scripts (FFI is denied at run time, so native addons are unused).
Command installCommand(const Layout &);
// The server: network unrestricted, read only the helper root, deny write/run/FFI, no prompts,
// frozen lock and cached modules only, a V8 heap ceiling, the WebGPU stub preloaded, bound to
// 127.0.0.1 on the given port.
Command serveCommand(const Layout &, int port, int heapMb);
// RES-14 "stub WebGPU adapter requests": the preload module SailTube writes to webGpuStub().
const char *webGpuStubSource();

// ---- Loopback bind verification (RES-14, NFR-SEC-05) ----

struct SocketEntry {
    bool v6 = false;
    unsigned char address[16] = {0}; // network order; v4 uses the first 4 bytes
    int port = 0;
    int state = 0;                   // 0x0A = LISTEN
    unsigned long long inode = 0;
};
// Parses the text of /proc/net/tcp (v6 = false) or /proc/net/tcp6 (v6 = true). The kernel prints
// each 32-bit word in host order; littleEndian is the device's byte order (aarch64: true).
// Malformed lines are skipped.
std::vector<SocketEntry> parseProcNetTcp(const std::string &text, bool v6, bool littleEndian = true);
// True for 127.0.0.1 and for ::ffff:127.0.0.1 only (not ::1, not other 127/8 addresses).
bool isLoopbackV4(const SocketEntry &);

enum class BindVerdict { NotListening, LoopbackOnly, NonLoopback, Unavailable };
// LISTEN sockets on the helper's port, or owned by the helper (inode in helperInodes): any one
// not on 127.0.0.1 is NonLoopback. LoopbackOnly needs a 127.0.0.1 listener on the port that is
// the helper's own when helperInodes is known (non-empty).
BindVerdict checkBind(const std::vector<SocketEntry> &, int port, const std::set<unsigned long long> &helperInodes);
// Inode of a /proc/<pid>/fd link target "socket:[12345]"; 0 when it is not a socket.
unsigned long long socketInode(const std::string &linkTarget);

// ---- yt-dlp arguments (RES-08 step 4, RES-14) ----

std::string baseUrl(int port);   // "http://127.0.0.1:<port>"
// Plugin directory and the documented bgutil HTTP provider extractor-args, only when the
// provider is ready AND the request asked for it; otherwise empty. Never a cookie option.
std::vector<std::string> extractorArguments(bool ready, bool requested, const std::string &pluginDir, int port);
// RES-08 step 4: the value to append to the "youtube:" extractor-args when the token is forced.
const char *forcedTokenArgument(); // "fetch_pot=always"

// ---- State machine (RES-14) ----

enum class State { Off, Installing, Starting, Verifying, Ready, Refused, Failed };
const char *stateName(State);

enum class Action { None, CheckFiles, Install, Start, Poll, Stop };

struct Timing {
    long long verifyTimeoutMs = 20000;  // start -> loopback listener
    long long stableMs = 60000;         // ready this long resets the failure count
    int maxInstallAttempts = 3;
};
// Backoff after the n-th consecutive failure (n >= 1): 5 s, 15 s, 60 s, 5 min, then 15 min.
long long backoffMs(int failures);

class Machine {
public:
    explicit Machine(const Timing &timing = Timing()) : timing_(timing) {}
    State state() const { return state_; }
    Reason reason() const { return reason_; }
    bool ready() const { return state_ == State::Ready; }
    int failures() const { return failures_; }
    long long retryAtMs() const { return retryAtMs_; }
    long long verifyDeadlineMs() const { return verifyDeadlineMs_; }

    // Off/Failed/Refused -> Installing (CheckFiles), or Refused on another architecture.
    Action enable(long long nowMs, const std::string &architecture);
    // Any state -> Off. Stop when a process may be running.
    Action disable();
    // Installing: the pin verdict of the staged copy and whether the frozen install is current.
    Action filesChecked(Reason pinVerdict, bool installIsCurrent, long long nowMs);
    // Installing: the frozen install finished; lockUnchanged = lockfile hash still the pinned one.
    Action installFinished(bool ok, bool lockUnchanged, long long nowMs);
    // Starting: the process started (ok) or failed to start.
    Action started(bool ok, long long nowMs);
    // Verifying / Ready: a bind check result.
    Action bindChecked(BindVerdict, long long nowMs);
    // Starting / Verifying / Ready: the helper process ended.
    Action exited(long long nowMs);
    // Failed: once nowMs reaches retryAtMs, retry from CheckFiles (the pin is re-checked).
    Action tick(long long nowMs);

private:
    Action fail(Reason, long long nowMs, Action);
    Action refuse(Reason, Action);
    Timing timing_;
    State state_ = State::Off;
    Reason reason_ = Reason::Disabled;
    int failures_ = 0;
    int installAttempts_ = 0;
    long long retryAtMs_ = 0;
    long long verifyDeadlineMs_ = 0;
    long long readySinceMs_ = -1;
};

}}}}
