// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "DBusLinkAdaptor.h"
#include "LinkRouter.h"

#include <QDBusArgument>
#include <QDBusConnectionInterface>
#include <QDebug>

namespace nordtile { namespace video { namespace sailfish {

DBusLinkAdaptor::DBusLinkAdaptor(QObject *host, LinkRouter *router) : QDBusAbstractAdaptor(host), router_(router) {}

QVariant DBusLinkAdaptor::unwrap(const QVariant &value) {
    if (value.userType() == qMetaTypeId<QDBusVariant>()) return unwrap(value.value<QDBusVariant>().variant());
    if (value.userType() != qMetaTypeId<QDBusArgument>()) return value;
    const QDBusArgument argument = value.value<QDBusArgument>();
    if (argument.currentType() != QDBusArgument::ArrayType) return QVariant();
    QVariantList list;
    argument.beginArray();
    while (!argument.atEnd()) {
        const QVariant element = unwrap(argument.asVariant());
        if (element.userType() == QMetaType::QStringList) {
            const QStringList inner = element.toStringList();
            if (!inner.isEmpty()) list.append(inner.first());
        } else {
            list.append(element);
        }
    }
    argument.endArray();
    return list;
}

void DBusLinkAdaptor::openUrl(const QString &url) {
    qInfo("sailtube-links call openUrl(s)");
    router_->openUrl(QVariant(url));
}

void DBusLinkAdaptor::openUrl(const QStringList &urls) {
    qInfo("sailtube-links call openUrl(as) n=%d", urls.size());
    router_->openUrl(QVariant(urls));
}

void DBusLinkAdaptor::openUrl(const QDBusVariant &url) {
    qInfo("sailtube-links call openUrl(v)");
    router_->openUrl(unwrap(url.variant()));
}

void DBusLinkAdaptor::activateWindow() {
    qInfo("sailtube-links call activateWindow");
    router_->raise();
}

DBusApplicationAdaptor::DBusApplicationAdaptor(QObject *host, LinkRouter *router)
    : QDBusAbstractAdaptor(host), router_(router) {}

void DBusApplicationAdaptor::Activate(const QVariantMap &) {
    router_->raise();
}

void DBusApplicationAdaptor::Open(const QStringList &uris, const QVariantMap &) {
    qInfo("sailtube-links call Application.Open n=%d", uris.size());
    router_->openUrl(QVariant(uris));
}

void DBusApplicationAdaptor::ActivateAction(const QString &, const QVariantList &, const QVariantMap &) {
    // No desktop actions are declared; an action still brings the window up.
    router_->raise();
}

QString DBusLinkService::serviceName() { return QStringLiteral("org.nordilabs.sailtube.SailTube"); }
QString DBusLinkService::objectPath() { return QStringLiteral("/"); }
QString DBusLinkService::applicationObjectPath() { return QStringLiteral("/org/nordilabs/sailtube/SailTube"); }

DBusLinkService::DBusLinkService(LinkRouter *router, QObject *parent)
    : QObject(parent),
      linkAdaptor_(new DBusLinkAdaptor(&linkHost_, router)),
      applicationAdaptor_(new DBusApplicationAdaptor(&applicationHost_, router)) {}

DBusLinkService::~DBusLinkService() {
    unregister();
}

bool DBusLinkService::registerOn(const QDBusConnection &input) {
    unregister();
    QDBusConnection connection(input);
    errorText_.clear();
    if (!connection.isConnected()) {
        errorText_ = QStringLiteral("session bus not connected");
        qWarning("sailtube: links over D-Bus unavailable: %s", qPrintable(errorText_));
        return false;
    }
    if (!connection.registerObject(objectPath(), &linkHost_, QDBusConnection::ExportAdaptors)
        || !connection.registerObject(applicationObjectPath(), &applicationHost_, QDBusConnection::ExportAdaptors)) {
        connection.unregisterObject(objectPath());
        connection.unregisterObject(applicationObjectPath());
        errorText_ = QStringLiteral("object path already exported");
        qWarning("sailtube: links over D-Bus unavailable: %s", qPrintable(errorText_));
        return false;
    }
    // Objects first, then the name: a call queued by the bus during activation finds them.
    if (!connection.registerService(serviceName())) {
        const QString owner = connection.interface() ? connection.interface()->serviceOwner(serviceName()).value() : QString();
        connection.unregisterObject(objectPath());
        connection.unregisterObject(applicationObjectPath());
        errorText_ = owner.isEmpty() ? connection.lastError().message()
                                     : QStringLiteral("name owned by %1 (another instance)").arg(owner);
        qWarning("sailtube: D-Bus name %s not acquired: %s", qPrintable(serviceName()), qPrintable(errorText_));
        return false;
    }
    connectionName_ = connection.name();
    registered_ = true;
    qInfo("sailtube-links service-registered name=%s unique=%s objects=/,%s", qPrintable(serviceName()),
          qPrintable(connection.baseService()), qPrintable(applicationObjectPath()));
    return true;
}

void DBusLinkService::unregister() {
    if (!registered_) return;
    QDBusConnection connection(connectionName_);
    if (connection.isConnected()) {
        connection.unregisterService(serviceName());
        connection.unregisterObject(objectPath());
        connection.unregisterObject(applicationObjectPath());
    }
    registered_ = false;
    connectionName_.clear();
}

}}}
