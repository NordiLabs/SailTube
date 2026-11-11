// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "ZipReader.h"

#include <QDir>
#include <QFile>
#include <QUrl>

#include <cerrno>
#include <fcntl.h>
#include <unistd.h>
#include <zlib.h>

namespace nordtile { namespace video { namespace library {

namespace {

const quint32 eocdSignature = 0x06054b50;
const quint32 zip64LocatorSignature = 0x07064b50;
const quint32 centralSignature = 0x02014b50;
const quint32 localSignature = 0x04034b50;
const int eocdSize = 22;
const int centralSize = 46;
const int localSize = 30;
const int chunkBytes = 64 * 1024;

quint16 le16(const char *p) {
    const unsigned char *u = reinterpret_cast<const unsigned char *>(p);
    return quint16(u[0] | (u[1] << 8));
}

quint32 le32(const char *p) {
    const unsigned char *u = reinterpret_cast<const unsigned char *>(p);
    return quint32(u[0]) | (quint32(u[1]) << 8) | (quint32(u[2]) << 16) | (quint32(u[3]) << 24);
}

} // namespace

class ZipReader::Sink {
public:
    virtual ~Sink() {}
    virtual bool write(const char *data, int size) = 0;
};

namespace {

class MemorySink : public ZipReader::Sink {
public:
    explicit MemorySink(QByteArray *out) : out_(out) {}
    bool write(const char *data, int size) override { out_->append(data, size); return true; }
private:
    QByteArray *out_;
};

// Private 0600 file created exclusively (no symlink following); removed unless committed.
class FileSink : public ZipReader::Sink {
public:
    explicit FileSink(const QString &path) : path_(QFile::encodeName(path)) {
        ::unlink(path_.constData());
        fd_ = ::open(path_.constData(), O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
    }
    ~FileSink() override {
        if (fd_ >= 0) ::close(fd_);
        if (!committed_) ::unlink(path_.constData());
    }
    bool isOpen() const { return fd_ >= 0; }
    bool write(const char *data, int size) override {
        while (size > 0) {
            ssize_t n = ::write(fd_, data, size_t(size));
            if (n < 0 && errno == EINTR) continue;
            if (n <= 0) return false;
            data += n;
            size -= int(n);
        }
        return true;
    }
    bool commitTo(const QString &target) {
        if (::fsync(fd_) != 0 || ::close(fd_) != 0) { fd_ = -1; return false; }
        fd_ = -1;
        if (::rename(path_.constData(), QFile::encodeName(target).constData()) != 0) return false;
        committed_ = true;
        return true;
    }
private:
    QByteArray path_;
    int fd_ = -1;
    bool committed_ = false;
};

} // namespace

const qint64 ZipReader::maxEntryBytes;

ZipReader::ZipReader(const QString &path) : path_(path) {}

bool ZipReader::fail(Error e) {
    error_ = e;
    return false;
}

QString ZipReader::errorString() const {
    switch (error_) {
    case Error::None: return QString();
    case Error::OpenFailed: return QStringLiteral("cannot open the archive");
    case Error::NotZip: return QStringLiteral("not a ZIP archive");
    case Error::Zip64Unsupported: return QStringLiteral("ZIP64 archives are not supported");
    case Error::MultiDiskUnsupported: return QStringLiteral("multi-disk archives are not supported");
    case Error::Truncated: return QStringLiteral("the archive is truncated or damaged");
    case Error::Encrypted: return QStringLiteral("encrypted entries are not supported");
    case Error::UnsupportedMethod: return QStringLiteral("unsupported compression method");
    case Error::TooLarge: return QStringLiteral("the entry is too large");
    case Error::CrcMismatch: return QStringLiteral("the entry failed its CRC check");
    case Error::InflateFailed: return QStringLiteral("the entry could not be decompressed");
    case Error::WriteFailed: return QStringLiteral("cannot write the extracted file");
    case Error::NoSuchEntry: return QStringLiteral("no such entry");
    }
    return QString();
}

bool ZipReader::open() {
    entries_.clear();
    error_ = Error::None;
    QFile file(path_);
    if (!file.open(QIODevice::ReadOnly)) return fail(Error::OpenFailed);
    const qint64 size = file.size();
    if (size < eocdSize) return fail(Error::NotZip);

    // The EOCD record sits at the end, followed by a comment of up to 65535 bytes.
    const qint64 tailSize = qMin<qint64>(size, eocdSize + 0xFFFF);
    if (!file.seek(size - tailSize)) return fail(Error::OpenFailed);
    const QByteArray tail = file.read(tailSize);
    if (tail.size() != tailSize) return fail(Error::Truncated);
    int at = -1;
    for (int i = tail.size() - eocdSize; i >= 0; --i) {
        if (le32(tail.constData() + i) == eocdSignature
            && i + eocdSize + le16(tail.constData() + i + 20) <= tail.size()) {
            at = i;
            break;
        }
    }
    if (at < 0) return fail(Error::NotZip);
    const char *e = tail.constData() + at;
    const qint64 eocdOffset = size - tailSize + at;
    const quint16 disk = le16(e + 4), cdDisk = le16(e + 6);
    const quint16 diskEntries = le16(e + 8), totalEntries = le16(e + 10);
    const quint32 cdSize = le32(e + 12), cdOffset = le32(e + 16);
    if (totalEntries == 0xFFFF || diskEntries == 0xFFFF || cdSize == 0xFFFFFFFFu || cdOffset == 0xFFFFFFFFu)
        return fail(Error::Zip64Unsupported);
    if (at >= 20 && le32(e - 20) == zip64LocatorSignature) return fail(Error::Zip64Unsupported);
    if (disk != 0 || cdDisk != 0 || diskEntries != totalEntries) return fail(Error::MultiDiskUnsupported);
    if (qint64(cdOffset) + cdSize > eocdOffset) return fail(Error::Truncated);

    if (!file.seek(cdOffset)) return fail(Error::Truncated);
    const QByteArray cd = file.read(cdSize);
    if (cd.size() != int(cdSize)) return fail(Error::Truncated);
    int pos = 0;
    for (int n = 0; n < totalEntries; ++n) {
        if (pos + centralSize > cd.size() || le32(cd.constData() + pos) != centralSignature)
            return fail(Error::Truncated);
        const char *h = cd.constData() + pos;
        ZipEntry entry;
        entry.flags = le16(h + 8);
        entry.method = le16(h + 10);
        entry.crc32 = le32(h + 16);
        entry.compressedSize = le32(h + 20);
        entry.uncompressedSize = le32(h + 24);
        const int nameLen = le16(h + 28), extraLen = le16(h + 30), commentLen = le16(h + 32);
        entry.localHeaderOffset = le32(h + 42);
        if (entry.compressedSize == 0xFFFFFFFFu || entry.uncompressedSize == 0xFFFFFFFFu
            || entry.localHeaderOffset == 0xFFFFFFFFu)
            return fail(Error::Zip64Unsupported);
        if (pos + centralSize + nameLen + extraLen + commentLen > cd.size()) return fail(Error::Truncated);
        const QByteArray rawName = cd.mid(pos + centralSize, nameLen);
        entry.name = (entry.flags & 0x0800) ? QString::fromUtf8(rawName) : QString::fromLatin1(rawName);
        entries_.append(entry);
        pos += centralSize + nameLen + extraLen + commentLen;
    }
    return true;
}

int ZipReader::indexOf(const QString &name) const {
    for (int i = 0; i < entries_.size(); ++i)
        if (entries_.at(i).name == name) return i;
    return -1;
}

bool ZipReader::decode(int index, Sink &sink, qint64 maxBytes) {
    if (index < 0 || index >= entries_.size()) return fail(Error::NoSuchEntry);
    const ZipEntry &entry = entries_.at(index);
    if (entry.flags & 0x0001) return fail(Error::Encrypted);
    if (entry.method != 0 && entry.method != 8) return fail(Error::UnsupportedMethod);
    const qint64 cap = qMin(maxBytes, maxEntryBytes);
    if (qint64(entry.uncompressedSize) > cap) return fail(Error::TooLarge);
    if (entry.method == 0 && entry.compressedSize != entry.uncompressedSize) return fail(Error::Truncated);

    QFile file(path_);
    if (!file.open(QIODevice::ReadOnly)) return fail(Error::OpenFailed);
    if (!file.seek(entry.localHeaderOffset)) return fail(Error::Truncated);
    const QByteArray local = file.read(localSize);
    if (local.size() != localSize || le32(local.constData()) != localSignature) return fail(Error::Truncated);
    const qint64 dataStart = qint64(entry.localHeaderOffset) + localSize + le16(local.constData() + 26)
                             + le16(local.constData() + 28);
    if (dataStart + entry.compressedSize > file.size() || !file.seek(dataStart)) return fail(Error::Truncated);

    // Sizes come from the central directory (local headers may defer them to a
    // data descriptor). The declared size is also the inflate bomb limit.
    const qint64 expected = entry.uncompressedSize;
    qint64 remaining = entry.compressedSize;
    qint64 produced = 0;
    uLong crc = ::crc32(0L, Z_NULL, 0);
    QByteArray in(chunkBytes, Qt::Uninitialized);

    if (entry.method == 0) {
        while (remaining > 0) {
            const int want = int(qMin<qint64>(remaining, chunkBytes));
            if (file.read(in.data(), want) != want) return fail(Error::Truncated);
            crc = ::crc32(crc, reinterpret_cast<const Bytef *>(in.constData()), uInt(want));
            if (!sink.write(in.constData(), want)) return fail(Error::WriteFailed);
            remaining -= want;
            produced += want;
        }
    } else {
        z_stream z;
        z.zalloc = Z_NULL;
        z.zfree = Z_NULL;
        z.opaque = Z_NULL;
        z.next_in = Z_NULL;
        z.avail_in = 0;
        if (inflateInit2(&z, -MAX_WBITS) != Z_OK) return fail(Error::InflateFailed);
        QByteArray out(chunkBytes, Qt::Uninitialized);
        int status = Z_OK;
        Error failure = Error::None;
        while (status != Z_STREAM_END && failure == Error::None) {
            if (z.avail_in == 0) {
                if (remaining <= 0) { failure = Error::Truncated; break; }
                const int want = int(qMin<qint64>(remaining, chunkBytes));
                if (file.read(in.data(), want) != want) { failure = Error::Truncated; break; }
                remaining -= want;
                z.next_in = reinterpret_cast<Bytef *>(in.data());
                z.avail_in = uInt(want);
            }
            z.next_out = reinterpret_cast<Bytef *>(out.data());
            z.avail_out = uInt(out.size());
            status = inflate(&z, Z_NO_FLUSH);
            if (status != Z_OK && status != Z_STREAM_END) {
                failure = (status == Z_BUF_ERROR && remaining <= 0) ? Error::Truncated : Error::InflateFailed;
                break;
            }
            const int got = out.size() - int(z.avail_out);
            produced += got;
            if (produced > expected) { failure = Error::TooLarge; break; }
            crc = ::crc32(crc, reinterpret_cast<const Bytef *>(out.constData()), uInt(got));
            if (got > 0 && !sink.write(out.constData(), got)) failure = Error::WriteFailed;
        }
        inflateEnd(&z);
        if (failure != Error::None) return fail(failure);
    }
    if (produced != expected) return fail(Error::Truncated);
    if (quint32(crc) != entry.crc32) return fail(Error::CrcMismatch);
    return true;
}

bool ZipReader::extract(int index, const QString &targetPath) {
    error_ = Error::None;
    if (index < 0 || index >= entries_.size()) return fail(Error::NoSuchEntry);
    FileSink sink(targetPath + QStringLiteral(".part-import"));
    if (!sink.isOpen()) return fail(Error::WriteFailed);
    if (!decode(index, sink, maxEntryBytes)) return false;
    if (!sink.commitTo(targetPath)) return fail(Error::WriteFailed);
    return true;
}

bool ZipReader::read(int index, QByteArray *out, qint64 maxBytes) {
    error_ = Error::None;
    QByteArray data;
    MemorySink sink(&data);
    if (!decode(index, sink, maxBytes)) return false;
    if (out) *out = data;
    return true;
}

QString importLocalPath(const QString &input) {
    const QString s = input.trimmed();
    if (s.startsWith(QLatin1String("file:"), Qt::CaseInsensitive)) return QUrl(s).toLocalFile();
    if (s == QLatin1String("~")) return QDir::homePath();
    if (s.startsWith(QLatin1String("~/"))) return QDir::homePath() + s.mid(1);
    return s;
}

}}}
