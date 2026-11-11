# Platform and invariant checks (PLT-03, PLT-04, NFR-TST-03)

`tools/platform_check.py` runs static checks on the source tree. It needs Python 3.9 and nothing
outside the standard library. `tests/platform/run.sh` runs every check on the tree. It then seeds
one violation per check into a scratch copy of the directories the checks read, and asserts that
the matching named check fails with the expected message.

```sh
sh tests/platform/run.sh                              # the suite; last line "platform: PASS"
python3 -I tools/platform_check.py                    # every check on this tree
python3 -I tools/platform_check.py cache_key paths    # selected checks
python3 -I tools/platform_check.py --root DIR         # another tree (a scratch copy, another checkout)
python3 -I tools/platform_check.py --desktop FILE sailjail_names permissions
python3 -I tools/platform_check.py --write-forced-sandbox-desktop OUT
```

The declarations live next to the suite, so a scratch copy carries its own:
`tests/platform/permissions.json`, `output_settings.json` and `path_exceptions.json`.

## What each check proves

| Check | Requirement | Proves (statically) | Seeded failures in `run.sh` |
|---|---|---|---|
| `sailjail_names` | PLT-03 (a) | Every variant's desktop entry has `[X-Sailjail]` with `OrganizationName=org.nordilabs.sailtube` and `ApplicationName=SailTube`. `main()` calls `QCoreApplication::setOrganizationName`/`setApplicationName` with the same literals before the application object and before the first `QStandardPaths` use. Every other name setter in shipped code agrees. A STORE entry with `Sandboxing=Disabled` fails. | Organization name changed in `main.cpp`; application name changed in the desktop entry; name set after the application object; STORE entry with `Sandboxing=Disabled` |
| `paths` | PLT-03 (b) | Shipped code (`Core`, `Clients`, `Shared`, except `Shared/SailTune/tests`; C++, QML, JS, Python; comments ignored) builds no data, cache or config path outside `~/.local/share`, `~/.cache` and `~/.config` `/org.nordilabs.sailtube/SailTube` (see the rules below). | `QDir::homePath()`; `GenericDataLocation` without the app subdirectory; a `/tmp/...` literal; a `/home/...` literal in QML; `MoviesLocation` in a file outside the Videos feature; `QDir::tempPath()` once `main.cpp` stops redirecting `TMPDIR`; a stale exception; and a positive control: paths in comments and relative suffixes still pass |
| `permissions` | PLT-04, NFR-TST-03 (4) | Permission audit. Every PLT-04 permission has an entry in `permissions.json`, and the file maps no other permission. Each entry names a feature, one or more feature flags, its variants, a requirement, and code globs. Each glob must match a file that uses the permission (a regex). The desktop entry declares no permission outside PLT-04, and none whose mapping is `declared:false`, unmapped, or limited to another variant. Every `declared:true` permission is in `Permissions=`. A declared permission must not rest on a flag of kind `planned`. `ExecDBus` is present exactly when the variant ships D-Bus activation: a `[D-BUS Service]` file, a spec that packages `dbus-1/services`, or `X-DBusActivatable`/`X-Maemo-Service` in the entry. STORE does not count when the spec removes the service. | `Secrets` declared with no feature; `Bluetooth` (outside PLT-04); `Audio` mapped but not declared; `Audio`'s code gone; `Secrets` declared on a planned flag; `ExecDBus` with nothing activated; a D-Bus service without `ExecDBus` |
| `cache_key` | NFR-TST-03 (1) | Every key of the settings inventory is classified in `output_settings.json`, as output-affecting (naming the `Shared/Playback/StreamSource.h` `StreamRequest` field it sets) or not (with a reason). Every `StreamRequest` field is classified too (identity, output, not output). For each cache key implementation listed (today the pinned SailTune `StreamRequest::cacheKey()`), every output field maps to a key field that `cacheKey()` uses, or is declared host-applied, with a reason. Every field of the component's own request struct is part of its key. | A new setting left unclassified; a new output-affecting setting whose request field is not in the key; a field added to the pinned request struct but not to `cacheKey()`; an unclassified `StreamRequest` field |
| `ui_defaults` | NFR-TST-03 (2), SET-03 | Default literals the UI writes are compared with the defaults table. The literals come from `value("group/key", literal)` fallbacks, `def:`/`defaultValue:` next to a `key:` (SettingsInventory.js, ConfigurationValue), and `appSettings.prop \|\| literal`. Once the shared table exists, the engine fallbacks in `AppSettings.cpp` are compared too. A literal for a key outside the table fails. | A changed literal (`appSettings.value("playback/maxHeight", 12345)`); a literal for an unknown key; on a tree with `qml/logic/SettingsInventory.js`, a changed `def:` |
| `sponsorblock` | NFR-TST-03 (3) | Every category the engine fetches has a UI row. The engine's categories are the string literals in a declared list: the body of a `sponsor*Categories()` function, or a `kSponsor*Category` constant, under `Core/` or `Shared/`. A UI row is the quoted category id in a QML/JS file under `Clients/Sailfish/qml` that mentions SponsorBlock. | Engine lists `sponsor` and `selfpromo` and the UI has a row only for `sponsor` (fails on `selfpromo`, not `sponsor`); a positive control with both rows |

The four NFR-TST-03 invariants are the named checks `cache_key`, `ui_defaults`, `sponsorblock` and
`permissions`.

### Settings inventory and defaults table

The ux component owns `Core/Settings/Domain/SettingsDefaults.cpp` (SET-03). When that file is on the
tree, it is both the inventory and the defaults table: the check parses its row macros and brace
rows. Until it lands (it is not on `main` yet), the engine accessors in
`Clients/Sailfish/Presentation/AppSettings.cpp` serve as the table, taken from their
`settings_.value(QStringLiteral("key"), default)` reads. The check prints which source it used. On
`main` today, the QML settings pages bind `appSettings.<property>` and contain no default literals.
The check states that and still fails on the first hard-coded default anyone adds. The Qt-based
`tests/settings` suite of the ux component covers SET-01/SET-02 (rows per variant, one control per key).
`ui_defaults` is the stdlib, no-build counterpart for SET-03.

### Cache key honesty

The pinned SailTune key (`Shared/SailTune/.../StreamTypes.h`, read-only) covers `audioOnly`,
`maxVideoHeight`, `maxAudioKbps`, `codecPreference` (audio), `dubLanguage` and `hardwareDecode`.
It has **no frame-rate field and no video codec order**. SailTube's `StreamRequest` carries
`highFrameRate` and `videoCodecs`, and they map to the settings `playback/highFrameRate` and
`playback/codecPreference`. These two are declared `host_applied`. One cached shared answer may
serve both values only if the SailTube adapter re-ranks or filters the rungs after the shared
answer. The component ranks every rung (higher fps first within a height) and filters nothing by
fps or video codec. This is a contract for the playback component's adapter, not something the static
check can prove; its own tests must show it. `playback/adaptivePipeline` is not output: Off makes
the player pick the muxed list of the same `StreamSet`, and it applies at start-up only.

Keys that only the ux table has are pre-classified (`providers/poProvider` and
`providers/clientOverride` as output, mapped to the request fields of the same names). They are
checked as soon as they appear in the inventory. If the table merges before those `StreamRequest`
fields do, `cache_key` fails, and that failure is correct.

## Path rules and the exception list (`paths`)

Allowed without comment:

- `QStandardPaths::AppDataLocation`, `AppLocalDataLocation`, `DataLocation`, `CacheLocation`,
  `AppConfigLocation`, and QML `StandardPaths.data`/`.cache`. With the names above they resolve
  to the Sailjail layout.
- `Generic{Cache,Data,Config}Location` only when the same statement appends
  `/org.nordilabs.sailtube[/SailTube]`.
- Relative suffixes appended to one of those (`cacheRoot + "/tmp"`, `"/library/library.db"`).
- `QDir::tempPath()`, default `QTemporaryDir`/`QTemporaryFile`, `TempLocation`: only while `main()`
  first moves `TMPDIR` to `CacheLocation/tmp` (`useFlashTemporaryDirectory()`, PLT-09), before
  the application object. The check verifies the redirect itself, so removing it makes
  `YtDlpRunner.cpp`'s temp directory fail.

Tied to a permission (`location_permissions` in `permissions.json`): `MoviesLocation` (Videos),
`MusicLocation` (Music), `DownloadLocation` (Downloads), QML `StandardPaths.videos/.music/.download`,
and `/run/media` (RemovableMedia). Each is allowed only in a file listed in that permission's
`code` globs, so it is always attached to the feature that needs the permission (DL-06 download
targets). Any other location (`HomeLocation`, `DocumentsLocation`, `PicturesLocation`,
`RuntimeLocation`, ...) fails. Reads of `HOME`, `USER`, `LOGNAME`, `TMPDIR`, `TMP`, `TEMP` and
`XDG_*` also fail, and so do `QDir::homePath()`/`QDir::home()`, `os.path.expanduser`, and string
literals starting with `/home`, `/tmp`, `/var`, `/usr`, `/opt`, `/etc`, `/media`, `/run`, `/mnt`,
`/sdcard`, `/dev`, `/proc`, `/sys`, `/root`, `/data`, `/storage` or `~/` (or `file:///home/...`).

`tests/platform/path_exceptions.json`:

| Kind | Entry | Why it is safe |
|---|---|---|
| allowed absolute | `/usr/share/harbour-sailtube/...` | The package's read-only runtime root (`NORDTILE_RUNTIME_ROOT`: bundled extractor, resolver, QuickJS, QML, licence notices). |
| allowed absolute | `/usr/share/icons/hicolor/*/apps/harbour-sailtube.{png,svg}` | The package's own icon (About page). |
| allowed absolute | `/proc/self/...`, `/dev/null`, `/dev/urandom` | Kernel and device interfaces with no user data (`/dev/urandom`: the pinned MediaProxy's session token). |
| `user-typed-import-path` (+ `-literal`) | `ZipReader.cpp` `QDir::homePath()` and `"~/"` | Expands a `~/` path the user typed or picked for an import. It reads that one file and writes nothing outside the layout. |
| `account-browser-import-home` | `main.cpp` `QDir::homePath()` | The home directory for the account's ACC-03 browser import (FULL): the three Sailfish Browser cookie databases are probed there, the found one is copied with its -wal/-shm into the private 0700 scratch dir, read and deleted. |
| `removable-media-user-name` (+ `-fallback`) | `DownloadLocation.cpp` `qgetenv("USER")`, `QDir::home().dirName()` | Only the user *name*, for the SD-card root `/run/media/<user>` (RemovableMedia, DL-06). Nothing in the home directory is opened. |
| `ram-filesystem-deny-list` | `DownloadPolicy.cpp` `"/tmp" "/var/tmp" "/dev/shm" "/run/user"` | A deny list (PLT-09): these paths are compared against, never opened. |

An exception names one file, one rule and one regex. An exception that no longer matches anything
fails the check, so the list cannot quietly go stale.

## Sandbox gaps the static checks report (device step)

`permissions` prints `SANDBOX GAP` for each permission that is `declared:false` while compiled code
of an `always` feature needs it. On `main`:

- **Videos, Music, Downloads, RemovableMedia — DL-06.** The download-folder setting offers
  `~/Videos`, `~/Music`, `~/Downloads` and mounted SD cards. In a sandboxed build without these
  permissions, the write probe refuses the user folders, which is a handled error. `/run/media` is
  not visible, so no card is offered.
- **Downloads — IMP-01/IMP-02.** The import file picker and the backup-export folder picker cannot
  reach `~/Downloads`, where Takeout and NewPipe archives usually sit.

None of these are in the PLT-03 "lost" list (cookie import from the browser, extractor updates,
helper executables, direct GStreamer linking, Nemo.KeepAlive). Either declare the permissions,
with their feature, in both variants, or hide these choices when sandboxed. That decision belongs
to the library and core components, and the forced-sandbox device run below confirms it.

## Forced-sandbox FULL test build (PLT-03 c) — device step pending

`--write-forced-sandbox-desktop OUT` writes the desktop entry with every `Sandboxing=Disabled` in
`[X-Sailjail]` removed, plus a leading comment. `run.sh` generates it and audits it with
`sailjail_names` and `permissions`. It also proves that the generator removes exactly that one
line from an entry that disables the sandbox. Nothing is added to `rpm/`, and the real desktop
file is unchanged. The shipped entry has no `Sandboxing=Disabled`
today, so the generated entry differs from it only by the comment.

Still pending, on the phone (needs the user's go-ahead):

1. Build and install a FULL RPM. Replace `/usr/share/applications/harbour-sailtube.desktop` with
   the generated entry, which keeps the same permissions, so Sailjail confines the app.
2. Start the app from its launcher icon (a plain shell start of the binary is not confined). Then cover
   search, playback (video, audio only, background), thumbnails, a download to the default
   folder, each DL-06 folder choice, an import from a file, an export, and a restart. Restart
   checks settings, library and resume state.
3. Collect `journalctl --user` and `journalctl` lines from `sailjail`/`firejail`, plus the app's
   own warnings. Pass means no file-access failure outside the "lost" features and the sandbox
   gaps listed above. Every gap seen is either fixed (a permission added together with its
   `permissions.json` entry) or added to the "lost" list in the requirements.
4. Confirm that no new directory appears under `~/.local/share`, `~/.cache` or `~/.config` other
   than `org.nordilabs.sailtube/SailTube`.

## Editing the declarations

- New setting: classify it in `tests/platform/output_settings.json` (`cache_key` fails until you
  do). If it changes the resolve answer, add its `StreamRequest` field, the field's role, and its
  key mapping (or a `host_applied` reason) for every cache key implementation.
- New permission: add the feature, its flag (`feature_flags`), its variants and its code to
  `tests/platform/permissions.json`. Then declare it in the desktop entry and update
  `tests/privacy/network_allowlist.json` (`build_permissions`) and `docs/PRIVACY_NETWORK.md`.
- New deliberate path outside the layout: add one exception (file, rule, regex, reason,
  requirement) to `tests/platform/path_exceptions.json` and a row to the table above.
