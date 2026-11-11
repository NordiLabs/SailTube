// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace nordtile { namespace music { namespace accounts { namespace domain {

// An opaque provider subject, never an email address or display name. The
// adapter must obtain it from a verified provider response, not user input.
struct AccountKey {
    std::string provider;
    std::string subject;
    bool valid() const { return !provider.empty() && !subject.empty(); }
    bool operator==(const AccountKey &other) const {
        return provider == other.provider && subject == other.subject;
    }
    bool operator!=(const AccountKey &other) const { return !(*this == other); }
};

enum class Capability {
    YouTubePlaylistsRead, YouTubeLikedVideosRead,
    MusicLibrarySongsRead, MusicLibraryAlbumsRead, MusicLibraryArtistsRead,
    MusicLibraryPlaylistsRead, MusicLibraryWrite,
    // Appended: create/delete the user's own YouTube playlists and their items
    // (OAuth scope .../auth/youtube). Never implies any YouTube Music access.
    YouTubePlaylistsWrite,
    // Appended for the cookie-session provider ("ytm-session"): least privilege, so a
    // kill switch can drop playlist writes while likes stay possible. MusicLibraryWrite
    // stays the umbrella for older code: it is granted iff any finer write capability is.
    MusicLibraryPlaylistsWrite, MusicLibraryLikesWrite
};

enum class ErrorCode {
    None, Unsupported, InvalidArgument, Busy, Cancelled, StaleOperation,
    Expired, AuthorizationRequired, PermissionDenied, Network,
    SecureStorageUnavailable, SecureStorageFailure, InvalidResponse,
    // Appended for the cookie-session provider (docs/ytm_hybrid/06-design.md 3.6):
    // RateLimited = "try later", ProtocolChanged = "YouTube changed, sync paused",
    // Blocked = bot check / consent / unexplained 403 (stop, cool down).
    RateLimited, ProtocolChanged, Blocked
};

struct Result {
    ErrorCode code = ErrorCode::None;
    // Fixed, safe application messages only; no raw provider response text.
    std::string message;
    bool ok() const { return code == ErrorCode::None; }
};

enum class SessionState {
    SignedOut, Authorizing, Authorized, Refreshing, Expired,
    SigningOut, CleanupRequired, Unavailable
};

struct Grant {
    AccountKey account;
    std::vector<Capability> capabilities;
    std::int64_t expiresAtUtcSeconds = 0;
    // Non-secret vault lookup handle. Tokens never cross this boundary.
    std::string credentialHandle;
};

struct Session {
    SessionState state = SessionState::SignedOut;
    Grant grant;
    Result lastResult;
    std::uint64_t generation = 0;
    bool can(Capability capability, std::int64_t nowUtcSeconds) const {
        if (state != SessionState::Authorized || nowUtcSeconds <= 0 ||
            grant.expiresAtUtcSeconds <= nowUtcSeconds || !grant.account.valid() ||
            grant.credentialHandle.empty()) return false;
        for (Capability granted : grant.capabilities) if (granted == capability) return true;
        return false;
    }
};

}}}}
