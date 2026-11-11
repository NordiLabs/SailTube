# Extractor updates (NFR-MNT-01, NFR-SEC-04) [FULL]

The FULL build can replace the bundled yt-dlp archive (`/usr/share/harbour-sailtube/runtime/yt-dlp.pyz`)
with a newer upstream release without an app release. The archive is a **data file**: the warm worker
copies it into its private directory and imports it with zipimport (NFR-ROB-04). It is never executed,
extracted or marked executable. The STORE build has no update code at all: it uses only the bundled
extractor.

| Piece | File | Qt |
|---|---|---|
| Policy (channels, URL allow-list per hop, versions, caps, layout, manifest, active archive, rename plans, confirmation token) | `Core/Extractor/Domain/ReleasePolicy.{h,cpp}` | none |
| Updater (network, staging, verifier call, atomic swap, rollback, QML properties) | `Core/Extractor/Infrastructure/ExtractorUpdater.{h,cpp}` | Core, Network |
| Verifier (OpenPGP signature, checksum, archive structure, version) | `Core/Extractor/Runtime/verify_release.py` | Python 3.9+ stdlib |
| Pinned release key | `Core/Extractor/Runtime/yt-dlp-release-key.asc` | — |
| Verifier host | `Core/Worker/Runtime/worker.py` (`NORDTILE_WORKER_TASK=verify`) | PyOtherSide in the app binary |
| Tests | `tests/extractor_update/` (`run.sh`) | Linux + Qt5Core/Network for the C++ part |

## Flow

1. **Check** (`check("stable" | "nightly")`). The user asks for it; nothing runs in the background.
   - `HEAD https://github.com/<repo>/releases/latest/download/yt-dlp` where `<repo>` is
     `yt-dlp/yt-dlp` (stable) or `yt-dlp/yt-dlp-nightly-builds` (nightly).
   - GitHub answers `302` to `/<repo>/releases/download/<tag>/yt-dlp`; the **tag is the version**
     (`YYYY.MM.DD` or `YYYY.MM.DD.N`). That URL answers `302` to a signed address on
     `release-assets.githubusercontent.com` (or the older `objects.githubusercontent.com`), whose
     `200` carries `Content-Length`: the **size**.
   - `GET .../releases/download/<tag>/SHA2-256SUMS` and `.../SHA2-256SUMS.sig` of that same tag (never
     `latest` again, so all three files belong to one release).
   - If the tag is newer than the active extractor, `offered(version, size, source)` is emitted with
     `source = https://github.com/<repo>/releases/download/<tag>/yt-dlp`; otherwise state `upToDate`.
2. **Confirmation** (UI). The dialog shows exactly `offeredSource`, `offeredVersion` and `offeredSize`.
3. **Install** (`install(version, size, source)` with the values the dialog showed). Refused unless
   they equal the last offer exactly and the offer is under 30 minutes old (`no-offer`,
   `token-mismatch`, `offer-expired`). Then:
   - `<root>/staging/` is recreated (0700) and the archive is downloaded from `source` into
     `staging/yt-dlp.pyz` (0600, `O_EXCL|O_NOFOLLOW`), stopping as soon as it exceeds the offered size;
     the received size must equal the offered size and its SHA-256 must equal the archive's line in
     the fetched `SHA2-256SUMS` (early check).
   - `SHA2-256SUMS` and `.sig` (the bytes the check fetched) are written into staging (0600) and the
     **verifier** runs (below). Its answer must be `ok`, with the offered version and the SHA-256 and
     size computed during the download.
   - `staging/manifest.json` is written (0600, fsync): `version`, `sha256`, `size`, `source`, `channel`,
     `installedAtMs`.
   - Swap, each step one `rename(2)` inside `<root>` followed by an fsync of `<root>`:
     `previous -> trash`, `current -> previous`, `staging -> current`, then `trash` is removed.
     `current` is not touched before this point.
4. The new archive is used from the **next start** of the warm workers (`restartNeeded`,
   `activeArchiveChanged()`).

Every hop of every download goes through `RedirectChain` (Domain): https only, port 443 or none, no
user info, `github.com` only on this channel's `releases/latest/download/<file>` (first request of a
check) or `releases/download/<tag>/<file>` (same tag throughout, valid version), then only an asset
host, which must answer (a redirect from it is refused). Redirects are followed by hand
(`FollowRedirectsAttribute` off), so the final URL is re-checked after each one. Certificates are
verified against the system CAs and errors are never ignored. Requests send a fixed User-Agent, no
cookies (load and save off), no cache, `Accept-Encoding: identity`. Bodies are bounded: archive by the
offered size (and 64 MiB), sums 64 KiB, signature 16 KiB. Per-hop timeout 30 s, reset while bytes arrive.

## Verification (`verify_release.py`)

Standard library only, reads files, imports and runs nothing from the archive.

1. **Key.** Exactly one OpenPGP v4 public-key packet, RSA, at least 2048 bits, whose v4 fingerprint
   (SHA-1 over `0x99 || len || body`) equals the pin `AC0C BBE6 848D 6A87 3464 AF4E 57CF 6593 3B5A 7581`
   (yt-dlp's release key, RSA-4096). In the app the key is always the installed
   `runtime/yt-dlp-release-key.asc` and the pin is the built-in one; the environment cannot name others.
2. **Signature** over `SHA2-256SUMS` (binary or ASCII-armoured, armour CRC24 checked): exactly one v4
   signature packet; type 0x00; public-key algorithm 1 (RSA) only; hash SHA-256 or SHA-512 only; a
   creation time; no unknown critical hashed subpacket; every issuer key id (subpacket 16) and issuer
   fingerprint (33) must name the pinned key, and at least one must be present; an expired signature is
   refused. The digest covers the file, the hashed part of the packet and the v4 trailer
   (`0x04 0xFF || len32`); the left 16 bits must match; `s < n`; `s^e mod n` must equal the
   EMSA-PKCS1-v1_5 encoding `00 01 FF.. 00 || DigestInfo || H` (at least 8 bytes of `FF`).
3. **Sums.** Every non-empty line is `<64 hex> <name>` (optionally `*name`); exactly one line names the
   asset `yt-dlp` exactly.
4. **Archive.** At most 64 MiB, its SHA-256 equal to that line.
5. **Structure.** A zip (the shebang prefix is fine); every entry name is plain and relative: no leading
   `/`, no `\`, no `:`, no NUL, no empty, `.` or `..` component; no symlink, no encrypted entry, no
   duplicate; at most 20000 entries and 512 MiB expanded; `yt_dlp/__init__.py` and `yt_dlp/version.py`
   present.
6. **Version.** `yt_dlp/version.py` (at most 64 KiB) is parsed with `ast.parse` (parsing never runs
   code); exactly one top-level `__version__ = "<literal>"` of the release form, equal to the offered tag.

Output: one JSON line `{"ok", "version", "sha256", "size", "error"}`; `error` is a fixed label. The
updater shows only fixed labels (`lastError`), never text from the network or the verifier.

**Cross-check.** The real 2026.10.26 `SHA2-256SUMS` and `.sig` are kept in
`tests/extractor_update/fixtures/real/`. `gpgv` with the fetched key reports a good signature by
`AC0CBBE6848D6A873464AF4E57CF65933B5A7581`, and `verify_release.py` accepts the same pair and rejects
it after a one-byte change; its `yt-dlp` line is
`e51e69e89489f199530f13d32a4342da3ddb2cf34842f364603a066db05afbf3`, the SHA-256 of the bundled archive.
In the build container the bundled archive verifies end to end (version 2026.10.26).

**Where it runs.** In the app, the verifier is the app binary in its warm-worker mode
(`NORDTILE_RESOLVER_WORKER=1`, `NORDTILE_WORKER_TASK=verify`, paths in `NORDTILE_VERIFY_SUMS`, `_SIG`,
`_ARCHIVE`, `_ASSET`, `_VERSION`), started as a `HelperProcess` (own process group, parent-death
signal, stdin and stderr closed, stdout bounded to 64 KiB, 120 s timeout). `worker.py` sees the task
before it imports anything else and returns after one JSON line; it never imports yt-dlp. A process
trace during an update therefore shows `execve` of the app binary only; the `.pyz` is opened for
reading (hash, zip directory, `version.py`) and never passed to `execve`.

## Store layout

`<root>` is `<AppDataLocation>/extractor` (0700):

| Path | Meaning |
|---|---|
| `current/yt-dlp.pyz`, `current/manifest.json` (+ `SHA2-256SUMS`, `.sig`) | the installed update |
| `previous/...` | the copy it replaced (kept for rollback) |
| `staging/` | the install in progress; never promoted unless verified; removed at start |
| `swap/` | transient during a rollback |
| `trash/` | transient during an install or revert |

**Active archive** (`activeArchive(bundled)`, decided at start and after each swap): `current/yt-dlp.pyz`
if `current/manifest.json` is valid (all fields well formed, `source` equals the tagged URL of its
channel and version) and the file's size and SHA-256 match it, and it is not older than the bundled
extractor (`bundledVersion`, from the packaged source manifest); otherwise the bundled archive.

**Rollback** (`rollback()`, needs a valid `previous/`): `current -> swap`, `previous -> current`,
`swap -> previous` (so it can be undone the same way). `revertToBundled()`: `current -> trash`
(previous is kept).

**Crash recovery** (constructor): no `current` but `swap` -> `swap -> current`; `swap` and no
`previous` -> `swap -> previous`; `swap` left -> removed; no `current` but `previous` ->
`previous -> current`; then `staging` and `trash` are removed. Whatever was interrupted, the last
working state comes back.

## Failure modes

| What happens | Result |
|---|---|
| http URL, port other than 443, user info in any hop | `insecure-url`, nothing fetched from it |
| Redirect to a host other than github.com / the two asset hosts | `host-not-allowed`, no connection to it |
| Wrong repository, file or tag in a github.com hop, tag changes | `bad-release-path` |
| Redirect from an asset host, or before the tag is known | `unexpected-redirect`; more than 4 redirects: `redirect-limit` |
| Untrusted or invalid certificate | `certificate` (no request sent) |
| 4xx/5xx | `http-status` (`sums` for the sums file, `signature-missing` for the `.sig`) |
| Archive larger than the cap at check, or growing past the offered size | `too-large` |
| Archive shorter than offered | `size-mismatch` (or `network` when the connection drops) |
| Archive bytes differ from the sums line | `checksum` |
| Signature missing / malformed / other algorithm / other key / bad value | `signature-missing` / `signature-format` / `signature-algorithm` / `signature-issuer` / `signature` |
| Shipped key not matching the pin | `key-pin` |
| Archive with `../`, absolute or otherwise unsafe entries | `archive-entry`; missing package files: `archive-content` |
| `__version__` not a literal release version / not the tag | `version` / `version-mismatch` |
| Verifier crashes, hangs (120 s), prints nonsense, or claims a digest that is not the download's | `verifier` / `checksum` |
| Disk full, rename fails | `storage` (recovery restores the previous working state) |
| `cancel()` or app exit during download or verification | `cancelled` / staging removed at next start; `current` untouched |
| `install` without a check, with other values, after 30 min | `no-offer` / `token-mismatch` / `offer-expired` |

In every case the working `current` (or the bundled archive) keeps working.

## UI (ux component)

FULL only (the type does not exist in the STORE build). Context property `extractorUpdater`
(`ExtractorUpdater`):

- Properties: `state` (`idle`, `checking`, `offered`, `upToDate`, `downloading`, `verifying`,
  `installing`, `installed`, `rolledBack`, `reverted`, `failed`), `channel`, `offeredVersion`,
  `offeredSize` (bytes), `offeredSource` (URL), `installedVersion`, `previousVersion`, `activeVersion`,
  `usingBundled`, `restartNeeded`, `bytesReceived` (NOTIFY `progress`), `lastError` (fixed label).
- Calls: `check(channel)`, `install(offeredVersion, offeredSize, offeredSource)`, `cancel()`,
  `rollback()`, `revertToBundled()`.
- Settings page "Extractor": the active version and whether it is bundled; channel choice (stable
  default, nightly with a short warning that nightly builds change daily); "Check for update".
- On `offered`: a confirmation dialog that shows **source** (`offeredSource`, verbatim), **version** and
  **size** (human readable plus exact bytes) and two buttons. Accept calls `install` with exactly the
  three shown values. Nothing installs without this dialog (NFR-SEC-04).
- While `downloading`: progress `bytesReceived / offeredSize` and a Cancel button (`cancel()`);
  `verifying` and `installing`: busy indicator.
- `installed` / `rolledBack` / `reverted`: "Restart SailTube to use yt-dlp <version>" (`restartNeeded`).
- "Roll back to <previousVersion>" when `previousVersion` is not empty; "Use the bundled extractor"
  when `usingBundled` is false.
- `failed`: one translated sentence per label group (network, certificate, refused source, signature,
  checksum or archive, storage, cancelled); never the label alone and never upstream text.
- Diagnostics (DIA): `activeVersion`, `usingBundled`, `installedVersion`, `previousVersion`, `lastError`.

## Device step I4 (FULL build)

1. Settings > Extractor shows the bundled version (2026.10.26) and "bundled".
2. With `strace` available (developer mode): `strace -f -e trace=execve,openat -o /tmp/i4.trace -p <pid>`
   on the running app. Check stable: the dialog shows the source URL, the version and the size. Accept.
3. When it says installed: `grep execve /tmp/i4.trace` lists only the app binary
   (`harbour-sailtube`, the verify task) and no `yt-dlp` program; `grep yt-dlp.pyz /tmp/i4.trace` shows
   only `openat` with `O_RDONLY` (verifier) and the staging write. Relaunch; diagnostics show the new
   version, not bundled. Play a video.
4. Roll back: diagnostics show the previous version after a relaunch (or "bundled" via "Use the
   bundled extractor" on a first update). Play a video.
5. Tampered copy: `printf 'x' | dd of=~/.local/share/org.nordilabs.sailtube/SailTube/extractor/current/yt-dlp.pyz bs=1 seek=4096 conv=notrunc`
   then relaunch: diagnostics show the bundled extractor (the damaged copy is refused at start) and
   playback works. A network-side tamper (wrong checksum, bad signature, `../` entry, redirect to an
   unlisted host, bad certificate) is covered by `tests/extractor_update` against the fake release
   host; there is no debug source in the shipped app.
6. STORE build: no Extractor section; `tests/runtime_downloads/run.sh` and `tests/privacy/run.sh`
   (`--variant store`) find no update code path.

## Limitations

- The one-shot resolver child (fallback when the warm lanes are disabled) and anything else that loads
  the archive from the pinned SailTune `resolver.py` still use the bundled archive.
- The signature is checked at install; at start only the manifest and SHA-256 of `current/` are
  checked (the store is private 0700).
- An update takes effect at the next start of the warm workers.
- No downgrade by check: an older release is never offered (`upToDate`); rollback is the way back.
- The key is pinned; a yt-dlp key rotation needs an app release (by design).
