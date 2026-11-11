# SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
# Standard package: includes the shared Python-archive resolver runtime (yt-dlp zipimport
# release + private QuickJS) prepared exactly as for SailTune into vendor/python-runtime.
# `--without python_runtime` builds a package that cannot reach YouTube (packaging checks only).
%bcond_without python_runtime
# `--with store` builds the STORE (Harbour) variant: no Nemo.KeepAlive (PLT-11, DL-09), and
# only the package requirements Harbour allows (PLT-01; checked by tools/harbour_validate.sh).
%bcond_with store
# Time-limited public beta (docs/BETA.md): FULL only, never for Harbour.
%bcond_with beta
%global beta_expires 2026-12-30
%global beta_release 0.beta3
# Device-only test hooks (the beta date override).
%bcond_with debug_hooks

%if %{with python_runtime}
%ifnarch aarch64
%{error:The prepared private QuickJS runtime is only available for aarch64}
%endif
# The private library must not create global RPM provides or system dependencies.
%global __provides_exclude_from ^%{_datadir}/harbour-sailtube/lib/.*$
%global __requires_exclude ^libsailtune-quickjs[.]so[.]1.*$
%endif

%global sailtube_license PolyForm-Noncommercial-1.0.0
%if %{with python_runtime}
%global sailtube_license %{sailtube_license} AND Unlicense AND ISC AND MIT
%endif

Name:       harbour-sailtube
Summary:    Native YouTube video client for Sailfish OS
Version:    0.1.0
%if %{with beta}
Release:    %{beta_release}
%else
Release:    1
%endif
Group:      Applications/Multimedia
License:    %{sailtube_license}
URL:        https://github.com/NordiLabs/SailTube
Vendor:     NordSail
Packager:   NordSail
Source0:    %{name}-%{version}.tar.bz2
BuildRequires:  pkgconfig(sailfishapp)
BuildRequires:  qt5-qttools-linguist
BuildRequires:  pkgconfig(Qt5Multimedia)
BuildRequires:  pkgconfig(Qt5Network)
BuildRequires:  pkgconfig(Qt5DBus)
BuildRequires:  pkgconfig(Qt5Quick)
BuildRequires:  pkgconfig(audioresource-qt)
BuildRequires:  pkgconfig(gstreamer-1.0)
BuildRequires:  pkgconfig(gstreamer-app-1.0)
BuildRequires:  pkgconfig(gstreamer-video-1.0)
BuildRequires:  pkgconfig(nemo-gstreamer-interfaces-1.0)
BuildRequires:  pkgconfig(egl)
BuildRequires:  pkgconfig(glesv2)
BuildRequires:  pkgconfig(Qt5Sql)
BuildRequires:  pkgconfig(zlib)
Requires:   sailfishsilica-qt5
Requires:   qt5-plugin-imageformat-jpeg
# Library component: download notifications.
Requires:   nemo-qml-plugin-notifications-qt5
Requires:   qml(Amber.Mpris)
%if %{without store}
# FULL only. On STORE these come with the base system and Harbour does not allow naming them.
Requires:   qt5-qtmultimedia-plugin-mediaservice-gstmediaplayer
Requires:   qt5-plugin-sqldriver-sqlite
Requires:   qml(Nemo.KeepAlive)
%endif
%if %{with python_runtime}
Requires:   pyotherside-qml-plugin-python3-qt5
%endif

%description
A native Sailfish Silica client for YouTube videos: search, video pages and
playback through the device's multimedia pipeline. Stream resolution runs on the
device with the packaged upstream yt-dlp. Availability depends on the service.

%prep
%setup -q

%build
%if %{with python_runtime}
test -s vendor/python-runtime/runtime/yt-dlp.pyz || exit 1
test -s vendor/python-runtime/lib/libsailtune-quickjs.so.1 || exit 1
test -s vendor/python-runtime/licenses/source-manifest.json || exit 1
test -s vendor/python-runtime/python-runtime-sources.tar.gz || exit 1
(cd vendor/python-runtime && sha256sum --strict -c SHA256SUMS) || exit 1
test -s Shared/Runtime/Python/resolver.py || exit 1
test -s Shared/Runtime/Python/Resolver.qml || exit 1
%endif
%if %{with beta} && %{with store}
%{error:The beta profile is FULL only; it is not for Harbour}
%endif
%if %{with store}
qmake sailtube.pro CONFIG+=sailtube_store %{?with_debug_hooks:CONFIG+=sailtube_debug_hooks}
%else
qmake sailtube.pro %{?with_beta:CONFIG+=sailtube_beta SAILTUBE_BETA_EXPIRES=%{beta_expires} SAILTUBE_BETA_RELEASE=%{beta_release}} %{?with_debug_hooks:CONFIG+=sailtube_debug_hooks}
%endif
make %{?_smp_mflags}

%install
# Run extraction and release explicitly: the i18n feature ignores failures, so a
# missing tool or broken catalog would otherwise ship stale or missing translations.
lupdate -noobsolete Clients/Sailfish Core -ts \
    translations/harbour-sailtube-en.ts translations/harbour-sailtube-es.ts \
    translations/harbour-sailtube-pt.ts translations/harbour-sailtube-fr.ts \
    translations/harbour-sailtube-de.ts translations/harbour-sailtube-it.ts \
    translations/harbour-sailtube-ru.ts translations/harbour-sailtube-ja.ts \
    translations/harbour-sailtube-zh_CN.ts translations/harbour-sailtube-ar.ts || exit 1
make INSTALL_ROOT=%{buildroot} install
for locale in en es pt fr de it ru ja zh_CN ar; do
    lrelease -nounfinished "translations/harbour-sailtube-$locale.ts" \
        -qm "%{buildroot}%{_datadir}/%{name}/translations/harbour-sailtube-$locale.qm" || exit 1
    test -s "%{buildroot}%{_datadir}/%{name}/translations/harbour-sailtube-$locale.qm" || exit 1
done
install -D -m 0644 LICENSE %{buildroot}%{_datadir}/%{name}/LICENSE
%if %{with store}
# PLT-11: the STORE package ships no Nemo.KeepAlive QML.
rm -f %{buildroot}%{_datadir}/%{name}/qml/components/DownloadKeepAlive.qml \
      %{buildroot}%{_datadir}/%{name}/qml/components/DisplayKeepOn.qml
! grep -rl 'Nemo.KeepAlive' %{buildroot}%{_datadir}/%{name}/qml || exit 1
# Harbour refuses files under dbus-1/services, so STORE has no D-Bus link activation
# (LNK-03 and E10 are FULL only). Drop the service and the desktop keys that point at it.
rm -f %{buildroot}%{_datadir}/dbus-1/services/org.nordilabs.sailtube.SailTube.service
sed -i -e '/^X-Maemo-/d' -e '/^MimeType=/d' -e '/^ExecDBus=/d' %{buildroot}%{_datadir}/applications/%{name}.desktop
%endif
%if %{with python_runtime}
install -D -m 0644 Shared/Runtime/Python/resolver.py %{buildroot}%{_datadir}/%{name}/runtime/resolver.py
install -D -m 0644 Shared/Runtime/Python/Resolver.qml %{buildroot}%{_datadir}/%{name}/runtime/Resolver.qml
install -D -m 0644 Core/Worker/Runtime/worker.py %{buildroot}%{_datadir}/%{name}/runtime/worker.py
install -D -m 0644 Core/Worker/Runtime/Worker.qml %{buildroot}%{_datadir}/%{name}/runtime/Worker.qml
%if %{without store}
install -D -m 0644 Core/Extractor/Runtime/verify_release.py %{buildroot}%{_datadir}/%{name}/runtime/verify_release.py
install -D -m 0644 Core/Extractor/Runtime/yt-dlp-release-key.asc %{buildroot}%{_datadir}/%{name}/runtime/yt-dlp-release-key.asc
%endif
install -D -m 0644 vendor/python-runtime/runtime/yt-dlp.pyz %{buildroot}%{_datadir}/%{name}/runtime/yt-dlp.pyz
install -D -m 0644 vendor/python-runtime/lib/libsailtune-quickjs.so.1 %{buildroot}%{_datadir}/%{name}/lib/libsailtune-quickjs.so.1
strip --strip-unneeded %{buildroot}%{_datadir}/%{name}/lib/libsailtune-quickjs.so.1
for f in QuickJS-LICENSE yt-dlp-LICENSE yt-dlp-THIRD_PARTY_LICENSES.txt source-manifest.json; do
    install -D -m 0644 "vendor/python-runtime/licenses/$f" "%{buildroot}%{_datadir}/%{name}/licenses/python-runtime/$f"
done
%endif

%files
%license %{_datadir}/%{name}/LICENSE
%{_bindir}/%{name}
%{_datadir}/%{name}/qml
%{_datadir}/%{name}/translations
%{_datadir}/applications/%{name}.desktop
%if %{without store}
%{_datadir}/dbus-1/services/org.nordilabs.sailtube.SailTube.service
%endif
%{_datadir}/icons/hicolor/*/apps/%{name}.png
%if %{with python_runtime}
%{_datadir}/%{name}/runtime/resolver.py
%{_datadir}/%{name}/runtime/Resolver.qml
%{_datadir}/%{name}/runtime/worker.py
%{_datadir}/%{name}/runtime/Worker.qml
%if %{without store}
%{_datadir}/%{name}/runtime/verify_release.py
%{_datadir}/%{name}/runtime/yt-dlp-release-key.asc
%endif
%{_datadir}/%{name}/runtime/yt-dlp.pyz
%{_datadir}/%{name}/lib/libsailtune-quickjs.so.1
%license %{_datadir}/%{name}/licenses/python-runtime/QuickJS-LICENSE
%license %{_datadir}/%{name}/licenses/python-runtime/yt-dlp-LICENSE
%license %{_datadir}/%{name}/licenses/python-runtime/yt-dlp-THIRD_PARTY_LICENSES.txt
%{_datadir}/%{name}/licenses/python-runtime/source-manifest.json
%endif

%changelog
* Wed Nov 11 2026 NordSail <355391039+nordsail@users.noreply.github.com> - 0.1.0
- Initial public release
