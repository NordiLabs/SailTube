// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#pragma once

// Minimal read-only ZIP reader for backup imports (IMP-01, IMP-03): end of
// central directory, central directory, local headers; methods 0 (stored) and
// 8 (deflate, zlib raw inflate). ZIP64, multi-disk and encrypted archives are
// refused. Every entry is CRC-32 checked and capped at maxEntryBytes.
//
// Entry names are never used as paths: the caller names the target file, so a
// hostile name such as "../x" cannot write outside the caller's directory.

#include <QByteArray>
#include <QList>
#include <QString>

namespace nordtile { namespace video { namespace library {

struct ZipEntry {
    QString name;                 // as stored (UTF-8 when flagged, else Latin-1)
    quint16 method = 0;
    quint16 flags = 0;
    quint32 crc32 = 0;
    quint32 compressedSize = 0;
    quint32 uncompressedSize = 0;
    quint32 localHeaderOffset = 0;
    bool isDirectory() const { return name.endsWith(QLatin1Char('/')); }
};

class ZipReader {
public:
    enum class Error {
        None, OpenFailed, NotZip, Zip64Unsupported, MultiDiskUnsupported, Truncated, Encrypted,
        UnsupportedMethod, TooLarge, CrcMismatch, InflateFailed, WriteFailed, NoSuchEntry
    };
    static const qint64 maxEntryBytes = 512LL * 1024 * 1024;

    explicit ZipReader(const QString &path);

    // Reads the central directory. False (and error()) when the file is not a usable ZIP.
    bool open();
    QList<ZipEntry> entries() const { return entries_; }
    int indexOf(const QString &name) const;

    // Writes entry `index` to "<targetPath>.part-import" (mode 0600), checks size
    // and CRC, then renames it to `targetPath`. On failure nothing is left behind.
    bool extract(int index, const QString &targetPath);
    // Small entries (CSV, JSON) straight into memory; refused above `maxBytes`.
    bool read(int index, QByteArray *out, qint64 maxBytes);

    Error error() const { return error_; }
    QString errorString() const;

    class Sink; // internal output target (file or memory)

private:
    bool fail(Error e);
    bool decode(int index, Sink &sink, qint64 maxBytes);

    QString path_;
    QList<ZipEntry> entries_;
    Error error_ = Error::None;
};

// Shared by the importers: "file://…" URLs and "~/…" paths to a local path.
QString importLocalPath(const QString &input);

}}}
