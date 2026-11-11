<!-- SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0 -->
# E10: D-Bus activation for links (`X-Maemo-*` keys vs `X-DBusActivatable`)

Decides the final form of PLT-13. Requirement rows: PLT-13, LNK-01..LNK-04; device steps §6 L1, L2,
L3. Pass criterion (§7): cold, warm, two links at once and under the sandbox all pass with no crash.

Status: host side done (code, files, host tests). **Device run pending.**

## 1. What the tree contains (host side)

| Piece | File |
|---|---|
| Link rules (hosts, forms, `t=`/`start=`, normalisation, cold-start queue) | `Core/Catalog/Domain/Links.{h,cpp}` |
| Router: `openUrl(QVariant)`, queue until ready, route then raise | `Clients/Sailfish/Presentation/LinkRouter.{h,cpp}` |
| D-Bus objects and name claim | `Clients/Sailfish/Presentation/DBusLinkAdaptor.{h,cpp}` |
| Session service file | `dbus/org.nordilabs.sailtube.SailTube.service` (installed by `Core/Catalog/pri/links.pri` to `/usr/share/dbus-1/services`) |
| Desktop entry | `harbour-sailtube.desktop` |
| QML hooks (page pushes, `window.activate()`, `setReady(true)`) | `Clients/Sailfish/qml/harbour-sailtube.qml` |
| Host tests | `tests/catalog_links/` (real `s`, `as`, `v` calls over a private session bus) |

The app exports, on the session bus, under the name `org.nordilabs.sailtube.SailTube`
(= Sailjail `<OrganizationName>.<ApplicationName>`, the only name the sandbox lets it own):

* object `/`, interface `org.nordilabs.sailtube.SailTube`: `openUrl(s)`, `openUrl(as)`,
  `openUrl(v)`, `activateWindow()`. All three `openUrl` forms take the first valid link.
* object `/org/nordilabs/sailtube/SailTube`, interface `org.freedesktop.Application`:
  `Activate(a{sv})`, `Open(as, a{sv})`, `ActivateAction(s, av, a{sv})` (for variant B).

If the name is already owned (a second instance) or the bus is missing, the app logs one warning
and keeps running without the D-Bus entry points (no crash).

Verified facts used here (from the SDK target image, mapplauncherd 4.2.16, and the public Sailjail
README):

* `invoker` flags: `--type=silica-qt5`, `-s/--single-instance` ("the existing application window
  will be activated if already launched"), `-F/--desktop-file`, `-n/--no-wait`, `-d/--delay SECS`,
  `-I/--id` (sandboxing id; guessed from the binary name when absent).
* System apps on the image pair `X-Maemo-Service/-Object-Path/-Method` with a session service file
  whose `Exec` is `/usr/bin/invoker --type=silica-qt5 --desktop-file=<app>.desktop -s -n -d 5 <binary>`
  (jolla-settings, sailfish-share).
* Sailjail: an application is granted ownership of `<OrganizationName>.<ApplicationName>` on the
  session bus; `ExecDBus` in `[X-Sailjail]` is "an optional command line which will be used to auto
  start the application" to provide that name.
* Harbour validator: allowed `[X-Sailjail]` keys are Permissions, OrganizationName,
  ApplicationName, ExecDBus; `ExecDBus` must be `harbour-sailtube` plus at most one word argument;
  `Exec` must start with `harbour-sailtube`; files outside `/usr/bin/harbour-sailtube`,
  `/usr/share/harbour-sailtube/`, the desktop file and icons are rejected, so **the service file
  cannot ship in a STORE (Harbour) RPM**. STORE needs `ExecDBus` (variant A2) or variant B.

Not verified (the device run answers it): what lipstick sends for `X-Maemo-Method` (string or
one-element array; FinTube saw both), whether lipstick honours `X-DBusActivatable`/`DBusActivatable`,
and whether sailjaild produces its own activation entry for `ExecDBus`.

## 2. Variants to build

All variants use the same binary. Only the desktop file and the service file change. Build each
as a FULL RPM with the normal build (`tools/native_build.sh`), version-suffixed so the installed
variant is visible in `rpm -q` (for example `Release: 1.e10a`).

### A. Classic keys + service file (shipped default)

Exactly the tree as committed:

```
[Desktop Entry]
...
Exec=harbour-sailtube
MimeType=x-url-handler/youtube.com;x-url-handler/www.youtube.com;x-url-handler/m.youtube.com;x-url-handler/youtu.be;x-url-handler/www.youtube-nocookie.com;
X-Maemo-Service=org.nordilabs.sailtube.SailTube
X-Maemo-Object-Path=/
X-Maemo-Method=org.nordilabs.sailtube.SailTube.openUrl

[X-Sailjail]
Permissions=Internet;Audio
OrganizationName=org.nordilabs.sailtube
ApplicationName=SailTube
```

`/usr/share/dbus-1/services/org.nordilabs.sailtube.SailTube.service`:

```
[D-BUS Service]
Name=org.nordilabs.sailtube.SailTube
Exec=/usr/bin/invoker --type=silica-qt5 --desktop-file=harbour-sailtube.desktop --single-instance /usr/bin/harbour-sailtube
```

`invoker` waits for the app (no `-n`), so the bus never sees the activation process exit before
the name is claimed. If A fails only on cold start, try **A-n**: the system-app form
`... --single-instance -n -d 5 /usr/bin/harbour-sailtube` and record which one passes.

### A2. Classic keys + `ExecDBus` (the Harbour-compatible form)

Variant A, but delete the service file from the RPM (drop the `INSTALLS += links_dbus_service` line
and the `%files` line) and add to `[X-Sailjail]`:

```
ExecDBus=harbour-sailtube
```

### B. `X-DBusActivatable`

Variant A with the three `X-Maemo-*` lines removed and these added to `[Desktop Entry]`:

```
X-DBusActivatable=true
DBusActivatable=true
```

(`DBusActivatable` is the freedesktop key; the spec text names `X-DBusActivatable`; keep both and
note which one lipstick reads.) Keep the service file (B) and, separately, try with `ExecDBus`
instead of the service file (B2). Note: the freedesktop rule wants the desktop file ID to equal
the bus name (`org.nordilabs.sailtube.SailTube.desktop`), which Harbour does not allow
(`harbour-sailtube.desktop`); record whether lipstick still calls `Open` on
`/org/nordilabs/sailtube/SailTube`.

## 3. Device steps

Phone rules first: only with the phone owner's yes; tell SailTune before and after;
all PulseAudio sinks muted at 0 % for the whole run, never raised; pause the player at the end.

Setup for each variant:

```
devel-su pkcon install-local harbour-sailtube-*.e10<variant>.rpm   # or rpm -U
grep sailtube /usr/share/applications/mimeinfo.cache                # every x-url-handler host listed
ls /usr/share/dbus-1/services/ | grep sailtube                      # A, A-n, B: present; A2, B2: absent
```

Observers, in two ssh shells (keep them running through each case):

```
dbus-monitor --session "destination='org.nordilabs.sailtube.SailTube'"
journalctl --user -f | grep -i -E 'sailtube|invoker|sailjail|booster'
```

Links (in a Notes entry or an SMS to self, tapped from Messages):

```
L-video   https://youtu.be/9zCH37330f8?t=90
L-list    https://www.youtube.com/watch?v=9zCH37330f8&list=<any public PL id>&index=2
L-pl      https://www.youtube.com/playlist?list=<same PL id>
L-handle  https://www.youtube.com/@NASA
L-hosts   https://m.youtube.com/watch?v=9zCH37330f8  https://youtube.com/shorts/<id>
          https://www.youtube-nocookie.com/embed/9zCH37330f8
```

Cases (run every case for every variant):

1. **Cold.** `pkill -f harbour-sailtube`; wait 3 s; tap L-video. Pass: the app starts, the video
   page opens and plays from 1:30 (needs the `startSeconds` hint in PlaybackSession, see §5); the
   process is still alive 30 s later; no `SIGSEGV`/`abort` in the journal. Record from
   dbus-monitor whether the argument was `string` or `array [ string ]`.
2. **Warm.** App running on the search page; tap L-list, then L-pl, then L-handle (back to Notes
   between taps). Pass: video page with the list (queue seeded at index 2 once the queue exists),
   playlist preview page, channel page; the window comes to the front each time.
3. **Two links at once, warm.** From ssh:
   ```
   dbus-send --session --type=method_call --dest=org.nordilabs.sailtube.SailTube / \
     org.nordilabs.sailtube.SailTube.openUrl string:https://youtu.be/9zCH37330f8 &
   dbus-send --session --type=method_call --dest=org.nordilabs.sailtube.SailTube / \
     org.nordilabs.sailtube.SailTube.openUrl array:string:https://www.youtube.com/@NASA &
   ```
   Pass: both route, in that order (video page, then channel page on top); none is lost.
4. **Two links at once, cold.** `pkill -f harbour-sailtube`, then the two `dbus-send` lines above.
   Pass: the bus starts one instance; both links are routed after start-up, in order.
5. **Under the sandbox.** For each variant confirm the activated process is sandboxed: its parent
   chain shows sailjail/firejail (`ps -o pid,ppid,args -e | grep -E 'sailjail|firejail|harbour-sailtube'`)
   and `/proc/<pid>/status` shows `Seccomp`/`NoNewPrivs` set. Repeat cases 1 and 3 in that state.
   Pass: same results as unsandboxed, the journal shows no `org.freedesktop.DBus.Error.AccessDenied`
   for the name.
6. **Second instance.** App running; start `invoker --type=silica-qt5 --single-instance /usr/bin/harbour-sailtube`
   from ssh. Pass: the existing window is raised; no second process stays; no crash.
7. **L3 hosts.** Tap each L-hosts link. Pass: each opens SailTube on the right page. With
   SailTune installed, a `music.youtube.com` link opens SailTune (SailTube does not register it).

Decision rule: adopt B only if B (or B2) passes cases 1-6; otherwise keep A for FULL and A2 for
STORE, provided they pass.

## 4. Host-side coverage already in place

`tests/catalog_links` (run with `tools/catalog_test.sh catalog_links`): 60+ link table rows,
`t=`/`start=` forms, D-Bus normalisation (string, one-element array, invalid first element, empty
array, non-string, variant list), cold-start queue order (two links before ready, both routed in
order after), route-then-raise order, LNK-04 routing, adaptor slots called directly with `s`,
`as`, `v`, and over a private session bus (`dbus-run-session`): `openUrl` with `s`, `as`, `v(as)`,
`v(av)`, `org.freedesktop.Application.Open`, two concurrent calls in order, a second instance
refused cleanly, the name released on unregister; plus static checks of the desktop file (each
host, no music host, Sailjail name = X-Maemo-Service, `Exec=harbour-sailtube`), the service file
and the `%files` line.

## 5. Known dependencies outside this package

* `PlaybackSession::open(id, hints)` ignores `startSeconds`, `playlistId` and `index` today; L1's
  "plays from 1:30" needs `resumePositionMs_ = hints.startSeconds * 1000` (when >= 0) in `open()`,
  taking precedence over a stored resume point (LIB-05), and the queue needs the list.
* `pages/ChannelPage.qml {reference}` and `pages/PlaylistPreviewPage.qml {playlistId}` come from the channel and
  playlist packages.

## 6. Result

(Device run pending. Fill in: date, device, OS version, variant, per case pass/fail, argument type
seen for `X-Maemo-Method`, the decision.)
