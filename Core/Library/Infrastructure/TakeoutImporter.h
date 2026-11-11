// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#pragma once

// IMP-03: subscriptions without sign-in.
//
// Google Takeout subscriptions.csv -> ImportBatch "takeout". The header row is
// "Channel Id,Channel Url,Channel Title" in English; localized exports are
// handled by finding the column of UC ids and the column of URLs by content
// (the title is the remaining column). RFC 4180 quoting, CRLF or LF, an
// optional UTF-8 BOM. A Takeout .zip is accepted too: its first entry named
// "subscriptions.csv" (any folder) is read in memory.
//
// OPML -> ImportBatch "opml": every <outline> (nested ones included) whose
// xmlUrl carries channel_id=UC… or whose htmlUrl is a /channel/UC… URL. A
// handle htmlUrl becomes a PendingHandle. Documents with a DTD are refused
// (no entity expansion).

#include "Core/Library/Domain/LibraryTypes.h"
#include <QByteArray>
#include <QString>

namespace nordtile { namespace video { namespace library {

enum class TextImportError {
    None,
    FileNotFound,
    TooLarge,            // above maxTextImportBytes
    ZipError,
    NoSubscriptionsCsv,  // a .zip without subscriptions.csv
    UnrecognizedFormat,  // no channel id column / not OPML
    MalformedXml
};

const qint64 maxTextImportBytes = 32LL * 1024 * 1024;

struct TextImportResult {
    ImportBatch batch;
    TextImportError error = TextImportError::None;
    QString errorString;
    bool ok() const { return error == TextImportError::None; }
};

// `nowMs` stamps addedMs (0 = current time).
TextImportResult parseTakeoutCsv(const QByteArray &data, qint64 nowMs = 0);
TextImportResult parseOpml(const QByteArray &data, qint64 nowMs = 0);
// File entry points: a .csv, a Takeout .zip, or an .opml/.xml file. Accept "file://" and "~/".
TextImportResult importTakeoutFile(const QString &input, qint64 nowMs = 0);
TextImportResult importOpmlFile(const QString &input, qint64 nowMs = 0);

}}}
