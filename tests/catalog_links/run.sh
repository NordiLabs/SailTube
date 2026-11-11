#!/bin/sh
# SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
# Link handling: classification table, D-Bus normalisation, cold-start queue, routing order,
# the D-Bus adaptor (direct calls, and real s/as/v calls over a private session bus), a real
# D-Bus activation (the call is queued by the bus before the name has an owner, then delivered
# when the service registers; LNK-03 / L1), and static checks of the desktop and service files.
if [ -z "$SAILTUBE_LINKS_BUS" ] && command -v dbus-daemon >/dev/null 2>&1; then
    # A private session bus with an activation entry for our name. Its Exec only waits: the
    # test process itself takes the name, as the activated app would.
    busdir=$(mktemp -d "${TMPDIR:-/tmp}/sailtube-links-bus.XXXXXX")
    mkdir -p "$busdir/services"
    printf '[D-BUS Service]\nName=org.nordilabs.sailtube.SailTube\nExec=/bin/sleep 30\n' > "$busdir/services/org.nordilabs.sailtube.SailTube.service"
    cat > "$busdir/bus.conf" <<CONF
<!DOCTYPE busconfig PUBLIC "-//freedesktop//DTD D-Bus Bus Configuration 1.0//EN"
 "http://www.freedesktop.org/standards/dbus/1.0/busconfig.dtd">
<busconfig>
  <type>session</type>
  <listen>unix:tmpdir=$busdir</listen>
  <servicedir>$busdir/services</servicedir>
  <policy context="default">
    <allow send_destination="*" eavesdrop="true"/>
    <allow eavesdrop="true"/>
    <allow own="*"/>
  </policy>
</busconfig>
CONF
    out=$(dbus-daemon --config-file="$busdir/bus.conf" --fork --print-address=1 --print-pid=1)
    addr=$(printf '%s\n' "$out" | sed -n 1p)
    pid=$(printf '%s\n' "$out" | sed -n 2p)
    if [ -n "$addr" ] && [ -n "$pid" ]; then
        DBUS_SESSION_BUS_ADDRESS="$addr" SAILTUBE_LINKS_BUS=1 SAILTUBE_LINKS_ACTIVATION=1 sh "$0" "$@"
        rc=$?
        kill "$pid" 2>/dev/null
        rm -rf "$busdir"
        exit $rc
    fi
    rm -rf "$busdir"
fi
if [ -z "$DBUS_SESSION_BUS_ADDRESS" ] && [ -z "$SAILTUBE_LINKS_BUS" ] && command -v dbus-run-session >/dev/null 2>&1; then
    SAILTUBE_LINKS_BUS=1 exec dbus-run-session -- sh "$0" "$@"
fi
SAILTUBE_REPO_ROOT=$(cd "$(dirname "$0")/../.." && pwd)
export SAILTUBE_REPO_ROOT
SUITE=catalog_links
MOC="Clients/Sailfish/Presentation/LinkRouter.h Clients/Sailfish/Presentation/DBusLinkAdaptor.h"
SOURCES="tests/catalog_links/LinksTest.cpp Core/Catalog/Domain/Links.cpp \
Clients/Sailfish/Presentation/LinkRouter.cpp Clients/Sailfish/Presentation/DBusLinkAdaptor.cpp"
QT_MODULES="Qt5Core Qt5DBus"
. "$(dirname "$0")/../catalog_common/compile.sh"
