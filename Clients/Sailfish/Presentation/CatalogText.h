// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#pragma once

// Count and age text for QML in the discovery language (catalog::LocalizedText), exposed as
// `catalogText`. Pages prefer YouTube's own localized text where a row carries it and use
// these for structured values ("1,2 M de suscriptores", "hace 3 días").

#include <QObject>
#include <QString>

namespace nordtile { namespace video { namespace sailfish {

class CatalogText final : public QObject {
    Q_OBJECT
    Q_PROPERTY(QString language READ language CONSTANT)
public:
    explicit CatalogText(const QString &hl, QObject *parent = nullptr) : QObject(parent), hl_(hl) {}
    QString language() const { return hl_; }

    Q_INVOKABLE QString views(double count) const;
    Q_INVOKABLE QString subscribers(double count) const;
    Q_INVOKABLE QString videos(double count) const;
    Q_INVOKABLE QString count(double count) const;
    // `epochSeconds` relative to now; empty for a negative epoch.
    Q_INVOKABLE QString age(double epochSeconds) const;

private:
    QString hl_;
};

}}}
