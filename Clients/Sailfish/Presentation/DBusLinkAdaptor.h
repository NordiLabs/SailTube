// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#pragma once

// D-Bus entry points for opened links (PLT-13, LNK-03), all forwarding to LinkRouter.
//  * org.nordilabs.sailtube.SailTube at "/" (the desktop file's X-Maemo-Service/-Object-Path/
//    -Method, and the Sailjail name <OrganizationName>.<ApplicationName>):
//    openUrl(s), openUrl(as), openUrl(v) and activateWindow(). The launcher may send a
//    one-element string array (warm start) or a plain string; all three overloads normalise
//    through LinkRouter::openUrl.
//  * org.freedesktop.Application at /org/nordilabs/sailtube/SailTube: Activate, Open,
//    ActivateAction, for the DBusActivatable variant evaluated in E10.
// DBusLinkService exports both objects, then claims the name. When the bus is not connected or
// another instance owns the name it logs, exports nothing and returns false (no crash).

#include <QDBusAbstractAdaptor>
#include <QDBusConnection>
#include <QDBusVariant>
#include <QObject>
#include <QString>
#include <QStringList>
#include <QVariantList>
#include <QVariantMap>

namespace nordtile { namespace video { namespace sailfish {

class LinkRouter;

class DBusLinkAdaptor final : public QDBusAbstractAdaptor {
    Q_OBJECT
    Q_CLASSINFO("D-Bus Interface", "org.nordilabs.sailtube.SailTube")
public:
    DBusLinkAdaptor(QObject *host, LinkRouter *router);

    // Unwraps a QDBusVariant payload (string, string list, or a demarshalled array) to what
    // LinkRouter::openUrl accepts.
    static QVariant unwrap(const QVariant &value);

public slots:
    void openUrl(const QString &url);
    void openUrl(const QStringList &urls);
    void openUrl(const QDBusVariant &url);
    void activateWindow();

private:
    LinkRouter *router_;
};

class DBusApplicationAdaptor final : public QDBusAbstractAdaptor {
    Q_OBJECT
    Q_CLASSINFO("D-Bus Interface", "org.freedesktop.Application")
public:
    DBusApplicationAdaptor(QObject *host, LinkRouter *router);

public slots:
    void Activate(const QVariantMap &platformData);
    void Open(const QStringList &uris, const QVariantMap &platformData);
    void ActivateAction(const QString &name, const QVariantList &parameter, const QVariantMap &platformData);

private:
    LinkRouter *router_;
};

class DBusLinkService final : public QObject {
    Q_OBJECT
public:
    static QString serviceName();          // org.nordilabs.sailtube.SailTube
    static QString objectPath();           // /
    static QString applicationObjectPath(); // /org/nordilabs/sailtube/SailTube

    explicit DBusLinkService(LinkRouter *router, QObject *parent = nullptr);
    ~DBusLinkService() override;

    bool registerOn(const QDBusConnection &connection);
    bool registered() const { return registered_; }
    QString errorText() const { return errorText_; }
    DBusLinkAdaptor *linkAdaptor() const { return linkAdaptor_; }
    DBusApplicationAdaptor *applicationAdaptor() const { return applicationAdaptor_; }

private:
    void unregister();

    QObject linkHost_;
    QObject applicationHost_;
    DBusLinkAdaptor *linkAdaptor_;
    DBusApplicationAdaptor *applicationAdaptor_;
    QString connectionName_;
    bool registered_ = false;
    QString errorText_;
};

}}}
