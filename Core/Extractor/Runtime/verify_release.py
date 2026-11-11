# SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
"""[FULL] Extractor update verifier (NFR-MNT-01, NFR-SEC-04). Python 3.9+ standard library only.

Decides whether a downloaded yt-dlp release archive may replace the working extractor. It reads files
and never imports, extracts or runs anything from the archive. In order:

1. The pinned release key: exactly one OpenPGP v4 RSA public-key packet whose fingerprint equals
   PINNED_FINGERPRINT (or the --fingerprint given to the command line in tests).
2. The detached signature over SHA2-256SUMS: exactly one v4 signature packet (binary or ASCII
   armoured), type 0x00 (binary document), RSA (algorithm 1), SHA-256 or SHA-512, issuer key id and/or
   issuer fingerprint naming the pinned key, a creation time, no unknown critical subpacket, the left
   16 bits of the digest matching, and EMSA-PKCS1-v1_5 with the algorithm's DigestInfo.
3. SHA2-256SUMS: exactly one line for the asset name (exact match), 64 hex digits.
4. The archive: within the size cap, its SHA-256 equal to that line.
5. The archive is a zip (a shebang prefix is fine) whose entries are all plain relative names (no
   absolute path, no backslash, no drive letter or colon, no '.' or '..' component, no NUL, no
   symlink, no encryption, no duplicates), within entry-count and expanded-size caps, containing
   yt_dlp/__init__.py and yt_dlp/version.py.
6. __version__ is read from yt_dlp/version.py with ast.parse (parsing never executes), must look like
   a yt-dlp release (YYYY.MM.DD[.N]) and, if --expect-version is given, equal it.

Command line (tests, and the app's worker task through verify()):
  python3 -I verify_release.py --key KEY --sums SHA2-256SUMS --sig SHA2-256SUMS.sig --archive FILE
                               --asset yt-dlp [--expect-version V] [--fingerprint HEX] [--max-size N]
prints one JSON object {"ok", "version", "sha256", "size", "error"} and exits 0 (ok) or 1.
`error` is a fixed label (see ERRORS), never text from the files.
"""
import argparse
import ast
import base64
import binascii
import hashlib
import json
import re
import stat
import sys
import time
import zipfile

# yt-dlp's release key (Simon Sawicki, yt-dlp signing key), RSA-4096.
PINNED_FINGERPRINT = "AC0CBBE6848D6A873464AF4E57CF65933B5A7581"

MAX_ARCHIVE_BYTES = 64 << 20
MIN_ARCHIVE_BYTES = 1024
MAX_SUMS_BYTES = 64 << 10
MAX_SIG_BYTES = 16 << 10
MAX_KEY_BYTES = 64 << 10
MAX_ENTRIES = 20000
MAX_EXPANDED_BYTES = 512 << 20
MAX_VERSION_FILE_BYTES = 64 << 10
MIN_RSA_BITS = 2048

ERRORS = (
    "usage", "io", "key", "key-pin", "signature-missing", "signature-format", "signature-algorithm",
    "signature-issuer", "signature-expired", "signature", "sums-format", "sums-missing-asset",
    "archive-size", "checksum", "archive-format", "archive-entry", "archive-content", "version",
    "version-mismatch",
)

VERSION_RE = re.compile(r"^[0-9]{4}\.[0-9]{2}\.[0-9]{2}(?:\.[0-9]{1,6})?$")
ASSET_RE = re.compile(r"^[A-Za-z0-9][A-Za-z0-9._-]{0,63}$")
HEX64_RE = re.compile(r"^[0-9a-fA-F]{64}$")

HASHES = {
    8: ("sha256", bytes.fromhex("3031300d060960864801650304020105000420")),
    10: ("sha512", bytes.fromhex("3051300d060960864801650304020305000440")),
}


class Refused(Exception):
    def __init__(self, label):
        Exception.__init__(self, label)
        self.label = label


# ------------------------------------------------------------------ OpenPGP

def _crc24(data):
    crc = 0xB704CE
    for byte in data:
        crc ^= byte << 16
        for _ in range(8):
            crc <<= 1
            if crc & 0x1000000:
                crc ^= 0x1864CFB
    return crc & 0xFFFFFF


def dearmor(data, label):
    """Binary packets: `data` itself, or the body of one ASCII-armoured block (CRC checked if present)."""
    stripped = data.lstrip()
    if not stripped.startswith(b"-----BEGIN PGP "):
        return data
    try:
        text = stripped.decode("ascii")
    except UnicodeDecodeError:
        raise Refused(label)
    lines = [line.strip() for line in text.splitlines()]
    begin = lines[0]
    if not begin.endswith("-----"):
        raise Refused(label)
    kind = begin[len("-----BEGIN "):-len("-----")]
    try:
        end = lines.index("-----END %s-----" % kind)
    except ValueError:
        raise Refused(label)
    body = lines[1:end]
    # Armour headers ("Key: value") end at the first blank line.
    if "" in body:
        head = body[:body.index("")]
        if all(":" in h for h in head):
            body = body[body.index("") + 1:]
    body = [line for line in body if line]
    checksum = None
    if body and body[-1].startswith("="):
        checksum = body.pop()[1:]
    try:
        raw = base64.b64decode("".join(body), validate=True)
    except (binascii.Error, ValueError):
        raise Refused(label)
    if checksum is not None:
        try:
            expected = base64.b64decode(checksum, validate=True)
        except (binascii.Error, ValueError):
            raise Refused(label)
        if len(expected) != 3 or int.from_bytes(expected, "big") != _crc24(raw):
            raise Refused(label)
    return raw


def packets(data, label):
    """[(tag, body)] of a packet sequence. Indeterminate and partial lengths are refused."""
    out, i, n = [], 0, len(data)
    while i < n:
        ctb = data[i]
        i += 1
        if not ctb & 0x80:
            raise Refused(label)
        if ctb & 0x40:   # new format
            tag = ctb & 0x3F
            if i >= n:
                raise Refused(label)
            first = data[i]
            if first < 192:
                length, i = first, i + 1
            elif first < 224:
                if i + 1 >= n:
                    raise Refused(label)
                length, i = ((first - 192) << 8) + data[i + 1] + 192, i + 2
            elif first == 255:
                if i + 5 > n:
                    raise Refused(label)
                length, i = int.from_bytes(data[i + 1:i + 5], "big"), i + 5
            else:
                raise Refused(label)   # partial body length
        else:            # old format
            tag = (ctb >> 2) & 0x0F
            kind = ctb & 0x03
            if kind == 3:
                raise Refused(label)   # indeterminate length
            size = (1, 2, 4)[kind]
            if i + size > n:
                raise Refused(label)
            length, i = int.from_bytes(data[i:i + size], "big"), i + size
        if i + length > n:
            raise Refused(label)
        out.append((tag, data[i:i + length]))
        i += length
    return out


def _mpi(body, offset, label):
    if offset + 2 > len(body):
        raise Refused(label)
    bits = int.from_bytes(body[offset:offset + 2], "big")
    size = (bits + 7) // 8
    end = offset + 2 + size
    if end > len(body) or bits == 0:
        raise Refused(label)
    value = int.from_bytes(body[offset + 2:end], "big")
    if value.bit_length() != bits:
        raise Refused(label)
    return value, end


class PublicKey(object):
    def __init__(self, n, e, fingerprint):
        self.n, self.e, self.fingerprint = n, e, fingerprint

    @property
    def key_id(self):
        return self.fingerprint[-16:]


def load_key(data, pinned_fingerprint):
    """The pinned RSA key from a transferable public key (binary or armoured)."""
    if len(data) > MAX_KEY_BYTES:
        raise Refused("key")
    keys = [body for tag, body in packets(dearmor(data, "key"), "key") if tag == 6]
    if len(keys) != 1:
        raise Refused("key")
    body = keys[0]
    if len(body) < 6 or body[0] != 4:
        raise Refused("key")
    if body[5] != 1:
        raise Refused("key")    # RSA (encrypt or sign) only
    n, offset = _mpi(body, 6, "key")
    e, offset = _mpi(body, offset, "key")
    if offset != len(body) or n.bit_length() < MIN_RSA_BITS or e < 3 or e % 2 == 0:
        raise Refused("key")
    fingerprint = hashlib.sha1(b"\x99" + len(body).to_bytes(2, "big") + body).hexdigest().upper()
    if fingerprint != pinned_fingerprint.upper().replace(" ", ""):
        raise Refused("key-pin")
    return PublicKey(n, e, fingerprint)


def _subpackets(area):
    out, i, n = [], 0, len(area)
    while i < n:
        first = area[i]
        if first < 192:
            length, i = first, i + 1
        elif first < 255:
            if i + 1 >= n:
                raise Refused("signature-format")
            length, i = ((first - 192) << 8) + area[i + 1] + 192, i + 2
        else:
            if i + 5 > n:
                raise Refused("signature-format")
            length, i = int.from_bytes(area[i + 1:i + 5], "big"), i + 5
        if length < 1 or i + length > n:
            raise Refused("signature-format")
        kind = area[i]
        out.append((kind & 0x7F, bool(kind & 0x80), area[i + 1:i + length]))
        i += length
    return out


def verify_signature(key, signed, sig_data, now=None):
    """Raises Refused unless sig_data is one valid v4 RSA signature by `key` over `signed`."""
    if not sig_data:
        raise Refused("signature-missing")
    if len(sig_data) > MAX_SIG_BYTES:
        raise Refused("signature-format")
    found = packets(dearmor(sig_data, "signature-format"), "signature-format")
    if len(found) != 1 or found[0][0] != 2:
        raise Refused("signature-format")
    body = found[0][1]
    if len(body) < 6 or body[0] != 4:
        raise Refused("signature-format")
    sig_type, pub_algo, hash_algo = body[1], body[2], body[3]
    if sig_type != 0x00:
        raise Refused("signature-format")
    if pub_algo != 1 or hash_algo not in HASHES:
        raise Refused("signature-algorithm")
    hashed_len = int.from_bytes(body[4:6], "big")
    hashed_end = 6 + hashed_len
    if hashed_end + 2 > len(body):
        raise Refused("signature-format")
    unhashed_len = int.from_bytes(body[hashed_end:hashed_end + 2], "big")
    unhashed_end = hashed_end + 2 + unhashed_len
    if unhashed_end + 2 > len(body):
        raise Refused("signature-format")
    left16 = body[unhashed_end:unhashed_end + 2]
    s, end = _mpi(body, unhashed_end + 2, "signature-format")
    if end != len(body):
        raise Refused("signature-format")

    created = expires = None
    issuers = []
    for kind, critical, value in _subpackets(body[6:hashed_end]):
        if kind == 2 and len(value) == 4:
            created = int.from_bytes(value, "big")
        elif kind == 3 and len(value) == 4:
            expires = int.from_bytes(value, "big")
        elif kind == 16 and len(value) == 8:
            issuers.append(("id", value.hex().upper()))
        elif kind == 33 and len(value) == 21 and value[0] == 4:
            issuers.append(("fpr", value[1:].hex().upper()))
        elif critical:
            raise Refused("signature-format")   # an unknown critical subpacket must not be ignored
    for kind, _critical, value in _subpackets(body[hashed_end + 2:unhashed_end]):
        if kind == 16 and len(value) == 8:
            issuers.append(("id", value.hex().upper()))
    if created is None:
        raise Refused("signature-format")
    if not issuers:
        raise Refused("signature-issuer")
    for kind, value in issuers:
        if (kind == "id" and value != key.key_id) or (kind == "fpr" and value != key.fingerprint):
            raise Refused("signature-issuer")
    if expires:
        if (time.time() if now is None else now) > created + expires:
            raise Refused("signature-expired")

    name, digest_info = HASHES[hash_algo]
    h = hashlib.new(name)
    h.update(signed)
    h.update(body[:hashed_end])
    h.update(b"\x04\xff" + hashed_end.to_bytes(4, "big"))
    digest = h.digest()
    if digest[:2] != left16:
        raise Refused("signature")
    k = (key.n.bit_length() + 7) // 8
    if s >= key.n:
        raise Refused("signature")
    em = pow(s, key.e, key.n).to_bytes(k, "big")
    tail = b"\x00" + digest_info + digest
    padding = k - 2 - len(tail)
    if padding < 8 or em != b"\x00\x01" + b"\xff" * padding + tail:
        raise Refused("signature")


# ------------------------------------------------------------------ release files

def sums_entry(sums, asset):
    """The lowercase SHA-256 of `asset` in a SHA2-256SUMS text (one exact line required)."""
    try:
        text = sums.decode("utf-8")
    except UnicodeDecodeError:
        raise Refused("sums-format")
    found = []
    for line in text.splitlines():
        if not line.strip():
            continue
        parts = line.split(None, 1)
        if len(parts) != 2 or not HEX64_RE.match(parts[0]):
            raise Refused("sums-format")
        name = parts[1].rstrip("\r")
        if name.startswith("*"):
            name = name[1:]
        if name == asset:
            found.append(parts[0].lower())
    if len(found) != 1:
        raise Refused("sums-missing-asset" if not found else "sums-format")
    return found[0]


def _plain_name(name):
    if not name or "\x00" in name or "\\" in name or ":" in name or name.startswith("/"):
        return False
    parts = name.split("/")
    if parts[-1] == "":
        parts = parts[:-1]   # a directory entry
    return bool(parts) and all(p not in ("", ".", "..") for p in parts)


def check_archive(path, max_size):
    """(sha256, size) of the archive file, read once within the size cap."""
    try:
        with open(path, "rb") as handle:
            size = 0
            h = hashlib.sha256()
            while True:
                block = handle.read(1 << 16)
                if not block:
                    break
                size += len(block)
                if size > max_size:
                    raise Refused("archive-size")
                h.update(block)
    except OSError:
        raise Refused("io")
    if size < MIN_ARCHIVE_BYTES:
        raise Refused("archive-size")
    return h.hexdigest(), size


def archive_version(path):
    try:
        archive = zipfile.ZipFile(path)
    except (zipfile.BadZipFile, OSError, ValueError):
        raise Refused("archive-format")
    with archive:
        infos = archive.infolist()
        if not infos or len(infos) > MAX_ENTRIES:
            raise Refused("archive-format")
        seen = set()
        expanded = 0
        for info in infos:
            for name in {info.filename, info.orig_filename}:
                if not _plain_name(name):
                    raise Refused("archive-entry")
            if info.filename in seen:
                raise Refused("archive-entry")
            seen.add(info.filename)
            mode = (info.external_attr >> 16) & 0xFFFF
            if mode and stat.S_ISLNK(mode):
                raise Refused("archive-entry")
            if info.flag_bits & 0x1:
                raise Refused("archive-entry")   # encrypted
            expanded += info.file_size
            if expanded > MAX_EXPANDED_BYTES:
                raise Refused("archive-format")
        if "yt_dlp/__init__.py" not in seen or "yt_dlp/version.py" not in seen:
            raise Refused("archive-content")
        info = archive.getinfo("yt_dlp/version.py")
        if info.file_size > MAX_VERSION_FILE_BYTES:
            raise Refused("archive-content")
        try:
            source = archive.read(info).decode("utf-8")
        except (zipfile.BadZipFile, OSError, ValueError, UnicodeDecodeError, NotImplementedError, RuntimeError):
            raise Refused("archive-format")
    try:
        tree = ast.parse(source, "version.py")   # parsing only: nothing in the file runs
    except (SyntaxError, ValueError):
        raise Refused("version")
    versions = []
    for node in tree.body:
        if (isinstance(node, ast.Assign) and len(node.targets) == 1 and isinstance(node.targets[0], ast.Name)
                and node.targets[0].id == "__version__"):
            value = node.value
            if isinstance(value, ast.Constant) and isinstance(value.value, str):
                versions.append(value.value)
            else:
                raise Refused("version")
    if len(versions) != 1 or not VERSION_RE.match(versions[0]):
        raise Refused("version")
    return versions[0]


def _read(path, limit, missing_label, label):
    try:
        with open(path, "rb") as handle:
            data = handle.read(limit + 1)
    except FileNotFoundError:
        raise Refused(missing_label)
    except OSError:
        raise Refused("io")
    if len(data) > limit:
        raise Refused(label)
    return data


def verify(key_path, sums_path, sig_path, archive_path, asset, expect_version=None,
           fingerprint=PINNED_FINGERPRINT, max_size=MAX_ARCHIVE_BYTES, now=None):
    """The result object; never raises for bad input."""
    result = {"ok": False, "version": None, "sha256": None, "size": None, "error": None}
    try:
        if not ASSET_RE.match(asset or "") or not re.match(r"^[0-9A-Fa-f ]{40,50}$", fingerprint or ""):
            raise Refused("usage")
        if expect_version is not None and not VERSION_RE.match(expect_version):
            raise Refused("usage")
        key = load_key(_read(key_path, MAX_KEY_BYTES, "key", "key"), fingerprint)
        sums = _read(sums_path, MAX_SUMS_BYTES, "sums-format", "sums-format")
        verify_signature(key, sums, _read(sig_path, MAX_SIG_BYTES, "signature-missing", "signature-format"), now)
        expected = sums_entry(sums, asset)
        digest, size = check_archive(archive_path, max_size)
        result["sha256"], result["size"] = digest, size
        if digest != expected:
            raise Refused("checksum")
        version = archive_version(archive_path)
        result["version"] = version
        if expect_version is not None and version != expect_version:
            raise Refused("version-mismatch")
        result["ok"] = True
    except Refused as refused:
        result["error"] = refused.label
    except (MemoryError, RecursionError):
        result["error"] = "archive-format"
    return result


def main(argv=None):
    parser = argparse.ArgumentParser(description="Verify a yt-dlp release archive (NFR-SEC-04).", add_help=True)
    parser.add_argument("--key", required=True)
    parser.add_argument("--sums", required=True)
    parser.add_argument("--sig", required=True)
    parser.add_argument("--archive", required=True)
    parser.add_argument("--asset", required=True)
    parser.add_argument("--expect-version")
    parser.add_argument("--fingerprint", default=PINNED_FINGERPRINT)
    parser.add_argument("--max-size", type=int, default=MAX_ARCHIVE_BYTES)
    try:
        args = parser.parse_args(argv)
    except SystemExit:
        print(json.dumps({"ok": False, "version": None, "sha256": None, "size": None, "error": "usage"}))
        return 1
    result = verify(args.key, args.sums, args.sig, args.archive, args.asset, args.expect_version,
                    args.fingerprint, min(args.max_size, MAX_ARCHIVE_BYTES))
    sys.stdout.write(json.dumps(result, separators=(",", ":")) + "\n")
    sys.stdout.flush()
    return 0 if result["ok"] else 1


if __name__ == "__main__":
    sys.exit(main())
