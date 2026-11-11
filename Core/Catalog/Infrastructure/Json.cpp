// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "Json.h"

namespace nordtile { namespace video { namespace catalog { namespace json {

QJsonValue at(const QJsonValue &root, const QString &dottedPath) {
    QJsonValue cur = root;
    for (const QString &seg : dottedPath.split(QLatin1Char('.'), QString::SkipEmptyParts)) {
        bool isIndex = false;
        const int idx = seg.toInt(&isIndex);
        if (isIndex && cur.isArray()) cur = cur.toArray().at(idx);
        else if (cur.isObject()) cur = cur.toObject().value(seg);
        else return QJsonValue();
    }
    return cur;
}

QString text(const QJsonValue &v) {
    if (v.isString()) return v.toString();
    if (!v.isObject()) return QString();
    const QJsonObject o = v.toObject();
    if (o.contains(QStringLiteral("simpleText"))) return o.value(QStringLiteral("simpleText")).toString();
    if (o.contains(QStringLiteral("runs"))) {
        QString out;
        for (const QJsonValue &r : o.value(QStringLiteral("runs")).toArray()) out += r.toObject().value(QStringLiteral("text")).toString();
        return out;
    }
    if (o.contains(QStringLiteral("content"))) return o.value(QStringLiteral("content")).toString();
    if (o.contains(QStringLiteral("text"))) return text(o.value(QStringLiteral("text")));
    const QString label = at(v, QStringLiteral("accessibility.accessibilityData.label")).toString();
    return label;
}

namespace {
bool walk(const QJsonValue &v, const QString &key, const std::function<bool(const QJsonObject &)> &visit) {
    if (v.isObject()) {
        const QJsonObject o = v.toObject();
        for (auto it = o.begin(); it != o.end(); ++it) {
            if (it.key() == key && it.value().isObject()) {
                if (!visit(it.value().toObject())) return false;
            }
            if (!walk(it.value(), key, visit)) return false;
        }
    } else if (v.isArray()) {
        for (const QJsonValue &x : v.toArray()) if (!walk(x, key, visit)) return false;
    }
    return true;
}

bool hasKey(const QJsonValue &v, const QStringList &keys) {
    if (v.isObject()) {
        const QJsonObject o = v.toObject();
        for (auto it = o.begin(); it != o.end(); ++it) {
            if (keys.contains(it.key())) return true;
            if (hasKey(it.value(), keys)) return true;
        }
    } else if (v.isArray()) {
        for (const QJsonValue &x : v.toArray()) if (hasKey(x, keys)) return true;
    }
    return false;
}
}

void forEachKey(const QJsonValue &root, const QString &key, const std::function<bool(const QJsonObject &)> &visit) {
    walk(root, key, visit);
}

QJsonObject findFirst(const QJsonValue &root, const QString &key) {
    QJsonObject found;
    walk(root, key, [&found](const QJsonObject &o) { found = o; return false; });
    return found;
}

bool containsKey(const QJsonValue &root, const QStringList &keys) { return hasKey(root, keys); }

QString continuationToken(const QJsonObject &holder) {
    QString token;
    walk(holder, QStringLiteral("continuationCommand"), [&token](const QJsonObject &c) {
        token = c.value(QStringLiteral("token")).toString();
        return token.isEmpty();
    });
    if (token.isEmpty()) {
        walk(holder, QStringLiteral("nextContinuationData"), [&token](const QJsonObject &c) {
            token = c.value(QStringLiteral("continuation")).toString();
            return token.isEmpty();
        });
    }
    return token;
}

QString bestImage(const QJsonValue &holder, bool largest) {
    QJsonArray list = holder.toObject().value(QStringLiteral("thumbnails")).toArray();
    if (list.isEmpty()) list = holder.toObject().value(QStringLiteral("sources")).toArray();
    if (list.isEmpty()) list = at(holder, QStringLiteral("image.sources")).toArray();
    QString url; int bestW = 0;
    for (const QJsonValue &t : list) {
        const QJsonObject o = t.toObject();
        const int w = o.value(QStringLiteral("width")).toInt();
        if (url.isEmpty() || (largest ? w > bestW : w < bestW)) {
            url = o.value(QStringLiteral("url")).toString();
            bestW = w;
        }
    }
    if (url.startsWith(QLatin1String("//"))) url.prepend(QLatin1String("https:"));
    return url;
}

QString firstBrowseId(const QJsonValue &v) {
    QString id;
    walk(v, QStringLiteral("browseEndpoint"), [&id](const QJsonObject &b) {
        id = b.value(QStringLiteral("browseId")).toString();
        return id.isEmpty();
    });
    return id;
}

}}}}
