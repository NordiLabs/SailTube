# SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
# Catalog area "links": owned by the catalog component. Paths relative to the repository root.
# Link classification, the cold-start queue, the router and the D-Bus entry points (LNK-01..04,
# PLT-13), plus the D-Bus session service file that starts the app through invoker.
QT += dbus

HEADERS += $$PWD/../Domain/Links.h \
           $$PWD/../../../Clients/Sailfish/Presentation/LinkRouter.h \
           $$PWD/../../../Clients/Sailfish/Presentation/DBusLinkAdaptor.h
SOURCES += $$PWD/../Domain/Links.cpp \
           $$PWD/../../../Clients/Sailfish/Presentation/LinkRouter.cpp \
           $$PWD/../../../Clients/Sailfish/Presentation/DBusLinkAdaptor.cpp

# E10 variant A (classic X-Maemo-* keys): the session bus starts the app for its name.
links_dbus_service.files = $$PWD/../../../dbus/org.nordilabs.sailtube.SailTube.service
links_dbus_service.path = /usr/share/dbus-1/services
INSTALLS += links_dbus_service
OTHER_FILES += $$PWD/../../../dbus/org.nordilabs.sailtube.SailTube.service
