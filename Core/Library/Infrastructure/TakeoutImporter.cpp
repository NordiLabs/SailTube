// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "TakeoutImporter.h"

#include "Core/Catalog/Domain/Video.h"
#include "Core/Library/Domain/LibraryPolicy.h"
#include "ZipReader.h"

#include <QDateTime>
#include <QFile>
#include <QFileInfo>
#include <QStringList>
#include <QUrl>
#include <QUrlQuery>
#include <QVector>
#include <QXmlStreamReader>

#include <set>

namespace nordtile { namespace video { namespace library {

namespace {

TextImportResult failure(const char *source, TextImportError error, const QString &message) {
    TextImportResult r;
    r.batch.source = source;
    r.error = error;
    r.errorString = message;
    return r;
}

qint64 nowOr(qint64 nowMs) { return nowMs > 0 ? nowMs : QDateTime::currentMSecsSinceEpoch(); }

bool isChannelId(const QString &s) { return catalog::validChannelId(s.trimmed().toStdString()); }

bool isUrl(const QString &s) {
    const QString t = s.trimmed();
    return t.startsWith(QLatin1String("http://"), Qt::CaseInsensitive)
           || t.startsWith(QLatin1String("https://"), Qt::CaseInsensitive);
}

// RFC 4180: quoted fields may hold commas, line breaks and doubled quotes.
QList<QStringList> parseCsvRows(const QString &text) {
    QList<QStringList> rows;
    QStringList row;
    QString field;
    bool quoted = false;
    bool rowHasData = false;
    const int n = text.size();
    for (int i = 0; i < n; ++i) {
        const QChar c = text.at(i);
        if (quoted) {
            if (c == QLatin1Char('"')) {
                if (i + 1 < n && text.at(i + 1) == QLatin1Char('"')) { field += c; ++i; }
                else quoted = false;
            } else {
                field += c;
            }
            continue;
        }
        if (c == QLatin1Char('"') && field.isEmpty()) {
            quoted = true;
            rowHasData = true;
        } else if (c == QLatin1Char(',')) {
            row << field;
            field.clear();
            rowHasData = true;
        } else if (c == QLatin1Char('\r') || c == QLatin1Char('\n')) {
            if (c == QLatin1Char('\r') && i + 1 < n && text.at(i + 1) == QLatin1Char('\n')) ++i;
            row << field;
            if (rowHasData || !field.isEmpty()) rows << row;
            row.clear();
            field.clear();
            rowHasData = false;
        } else {
            field += c;
        }
    }
    if (rowHasData || !field.isEmpty()) {
        row << field;
        rows << row;
    }
    return rows;
}

int bestColumn(const QVector<int> &counts, int exclude) {
    int best = -1;
    for (int i = 0; i < counts.size(); ++i)
        if (i != exclude && counts.at(i) > 0 && (best < 0 || counts.at(i) > counts.at(best))) best = i;
    return best;
}

QString cell(const QStringList &row, int column) {
    return column >= 0 && column < row.size() ? row.at(column).trimmed() : QString();
}

bool readCapped(const QString &path, QByteArray *out, TextImportError *error) {
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) { *error = TextImportError::FileNotFound; return false; }
    if (f.size() > maxTextImportBytes) { *error = TextImportError::TooLarge; return false; }
    *out = f.read(maxTextImportBytes + 1);
    if (out->size() > maxTextImportBytes) { *error = TextImportError::TooLarge; return false; }
    return true;
}

QString errorText(TextImportError e) {
    switch (e) {
    case TextImportError::FileNotFound: return QStringLiteral("the file does not exist or cannot be read");
    case TextImportError::TooLarge: return QStringLiteral("the file is too large");
    default: return QString();
    }
}

} // namespace

TextImportResult parseTakeoutCsv(const QByteArray &data, qint64 nowMs) {
    nowMs = nowOr(nowMs);
    QByteArray bytes = data;
    if (bytes.startsWith("\xEF\xBB\xBF")) bytes.remove(0, 3);
    const QList<QStringList> rows = parseCsvRows(QString::fromUtf8(bytes));

    int idCol = -1, urlCol = -1, titleCol = -1;
    bool englishHeader = false;
    if (!rows.isEmpty() && rows.first().size() >= 3
        && rows.first().at(0).trimmed().compare(QLatin1String("Channel Id"), Qt::CaseInsensitive) == 0
        && rows.first().at(1).trimmed().compare(QLatin1String("Channel Url"), Qt::CaseInsensitive) == 0
        && rows.first().at(2).trimmed().compare(QLatin1String("Channel Title"), Qt::CaseInsensitive) == 0) {
        englishHeader = true;
        idCol = 0; urlCol = 1; titleCol = 2;
    } else {
        int width = 0;
        for (const QStringList &r : rows) width = qMax(width, r.size());
        QVector<int> ids(width, 0), urls(width, 0);
        for (const QStringList &r : rows)
            for (int i = 0; i < r.size(); ++i) {
                if (isChannelId(r.at(i))) ++ids[i];
                else if (isUrl(r.at(i))) ++urls[i];
            }
        idCol = bestColumn(ids, -1);
        urlCol = bestColumn(urls, idCol);
        if (idCol < 0 && urlCol < 0)
            return failure("takeout", TextImportError::UnrecognizedFormat,
                           QStringLiteral("no channel ids found; is this a Takeout subscriptions.csv?"));
        for (int i = 0; i < width && titleCol < 0; ++i)
            if (i != idCol && i != urlCol) titleCol = i;
    }

    TextImportResult result;
    result.batch.source = "takeout";
    std::set<std::string> seen;
    for (int r = englishHeader ? 1 : 0; r < rows.size(); ++r) {
        const QStringList &row = rows.at(r);
        const QString url = cell(row, urlCol);
        std::string id = isChannelId(cell(row, idCol)) ? cell(row, idCol).toStdString() : std::string();
        if (id.empty()) id = channelIdFromUrl(url.toStdString());
        if (!id.empty()) {
            if (!seen.insert(id).second) continue;
            ChannelRecord ch;
            ch.channelId = id;
            ch.name = cell(row, titleCol).toStdString();
            ch.addedMs = nowMs;
            result.batch.subscriptions.push_back(ch);
        } else if (isHandleUrl(url.toStdString())) {
            if (!seen.insert(url.toStdString()).second) continue;
            PendingHandle h;
            h.url = url.toStdString();
            h.name = cell(row, titleCol).toStdString();
            h.addedMs = nowMs;
            result.batch.pendingHandles.push_back(h);
        } else if (r > 0 || isUrl(url)) {
            ++result.batch.skippedRows; // a first row without an id is a localized header
        }
    }
    return result;
}

TextImportResult parseOpml(const QByteArray &data, qint64 nowMs) {
    nowMs = nowOr(nowMs);
    TextImportResult result;
    result.batch.source = "opml";
    QXmlStreamReader xml(data);
    bool root = true;
    std::set<std::string> seen;
    while (!xml.atEnd()) {
        const QXmlStreamReader::TokenType t = xml.readNext();
        if (t == QXmlStreamReader::DTD)
            return failure("opml", TextImportError::MalformedXml, QStringLiteral("OPML with a DTD is not accepted"));
        if (t != QXmlStreamReader::StartElement) continue;
        if (root) {
            if (xml.name().compare(QLatin1String("opml"), Qt::CaseInsensitive) != 0)
                return failure("opml", TextImportError::UnrecognizedFormat, QStringLiteral("not an OPML document"));
            root = false;
            continue;
        }
        if (xml.name() != QLatin1String("outline")) continue;
        const QXmlStreamAttributes a = xml.attributes();
        const QString xmlUrl = a.value(QLatin1String("xmlUrl")).toString().trimmed();
        const QString htmlUrl = a.value(QLatin1String("htmlUrl")).toString().trimmed();
        QString name = a.value(QLatin1String("title")).toString();
        if (name.isEmpty()) name = a.value(QLatin1String("text")).toString();
        if (xmlUrl.isEmpty() && htmlUrl.isEmpty()) continue; // a folder outline

        std::string id;
        if (!xmlUrl.isEmpty()) {
            const QString q = QUrlQuery(QUrl(xmlUrl)).queryItemValue(QStringLiteral("channel_id"));
            if (isChannelId(q)) id = q.trimmed().toStdString();
        }
        if (id.empty()) id = channelIdFromUrl(htmlUrl.toStdString());
        if (!id.empty()) {
            if (!seen.insert(id).second) continue;
            ChannelRecord ch;
            ch.channelId = id;
            ch.name = name.toStdString();
            ch.addedMs = nowMs;
            result.batch.subscriptions.push_back(ch);
        } else if (isHandleUrl(htmlUrl.toStdString())) {
            if (!seen.insert(htmlUrl.toStdString()).second) continue;
            PendingHandle h;
            h.url = htmlUrl.toStdString();
            h.name = name.toStdString();
            h.addedMs = nowMs;
            result.batch.pendingHandles.push_back(h);
        } else {
            ++result.batch.skippedRows;
        }
    }
    if (xml.hasError())
        return failure("opml", TextImportError::MalformedXml, xml.errorString());
    if (root) return failure("opml", TextImportError::UnrecognizedFormat, QStringLiteral("not an OPML document"));
    return result;
}

TextImportResult importTakeoutFile(const QString &input, qint64 nowMs) {
    const QString path = importLocalPath(input);
    if (path.isEmpty() || !QFileInfo(path).isFile())
        return failure("takeout", TextImportError::FileNotFound, errorText(TextImportError::FileNotFound));
    QByteArray head;
    {
        QFile f(path);
        if (!f.open(QIODevice::ReadOnly))
            return failure("takeout", TextImportError::FileNotFound, errorText(TextImportError::FileNotFound));
        head = f.read(4);
    }
    QByteArray data;
    if (head.startsWith("PK\x03\x04") || head.startsWith("PK\x05\x06")) {
        ZipReader zip(path);
        if (!zip.open()) return failure("takeout", TextImportError::ZipError, zip.errorString());
        const QList<ZipEntry> entries = zip.entries();
        int pick = -1;
        for (int i = 0; i < entries.size() && pick < 0; ++i) {
            const QString n = entries.at(i).name;
            if (n.compare(QLatin1String("subscriptions.csv"), Qt::CaseInsensitive) == 0
                || n.endsWith(QLatin1String("/subscriptions.csv"), Qt::CaseInsensitive))
                pick = i;
        }
        if (pick < 0)
            return failure("takeout", TextImportError::NoSubscriptionsCsv,
                           QStringLiteral("the archive holds no subscriptions.csv"));
        if (!zip.read(pick, &data, maxTextImportBytes)) {
            const TextImportError e = zip.error() == ZipReader::Error::TooLarge ? TextImportError::TooLarge
                                                                               : TextImportError::ZipError;
            return failure("takeout", e, zip.errorString());
        }
    } else {
        TextImportError e = TextImportError::None;
        if (!readCapped(path, &data, &e)) return failure("takeout", e, errorText(e));
    }
    return parseTakeoutCsv(data, nowMs);
}

TextImportResult importOpmlFile(const QString &input, qint64 nowMs) {
    const QString path = importLocalPath(input);
    QByteArray data;
    TextImportError e = TextImportError::None;
    if (path.isEmpty() || !QFileInfo(path).isFile()) e = TextImportError::FileNotFound;
    if (e != TextImportError::None || !readCapped(path, &data, &e)) return failure("opml", e, errorText(e));
    return parseOpml(data, nowMs);
}

}}}
