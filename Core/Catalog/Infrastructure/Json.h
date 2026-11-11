// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#pragma once

// Small helpers for walking InnerTube JSON by shape rather than by fixed paths (DSC-03).

#include <QJsonArray>
#include <QJsonObject>
#include <QJsonValue>
#include <QString>
#include <QStringList>
#include <functional>
#include <string>

namespace nordtile { namespace video { namespace catalog { namespace json {

// Follows a dotted path; numeric segments index arrays ("contents.0.tabRenderer").
QJsonValue at(const QJsonValue &root, const QString &dottedPath);

// Display text of any InnerTube text shape: {simpleText}, {runs:[{text}]}, {content} (view
// models), a plain string, or the accessibility label as the last resort.
QString text(const QJsonValue &v);

// Calls `visit(object)` for every object stored under `key` anywhere below `root` (depth
// first, document order). Returning false from visit stops the walk.
void forEachKey(const QJsonValue &root, const QString &key, const std::function<bool(const QJsonObject &)> &visit);

// First object stored under `key` anywhere below `root`, or an empty object.
QJsonObject findFirst(const QJsonValue &root, const QString &key);

// True when any of `keys` occurs anywhere below `root` (container markers, DSC-01).
bool containsKey(const QJsonValue &root, const QStringList &keys);

// The continuation token of a continuationItemRenderer (or any object holding one):
// continuationEndpoint.continuationCommand.token, or the older nextContinuationData.
QString continuationToken(const QJsonObject &holder);

// The largest/smallest thumbnail URL in {thumbnails:[...]} or {sources:[...]}, made absolute
// ("//host" -> "https://host").
QString bestImage(const QJsonValue &thumbnailHolder, bool largest = true);

// The browseId of the first browseEndpoint below `v` (owner channel links).
QString firstBrowseId(const QJsonValue &v);

inline std::string s(const QString &q) { return q.toStdString(); }

}}}}
