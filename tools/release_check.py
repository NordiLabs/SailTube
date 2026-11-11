#!/usr/bin/env python3
# SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
"""Release hygiene checks (NFR-MNT-03). Python 3.9 stdlib only; run with `python3 -I`.

    tools/release_check.py [--root DIR] version
    tools/release_check.py tag TAG [--repo DIR] [--main-ref REF]
    tools/release_check.py payload RPM_A RPM_B

Single source of the version: the `Version:` line of rpm/harbour-sailtube.spec. It is what
rpmbuild stamps on the package that ships, so the RPM can never disagree with it; the
`VERSION =` line of sailtube.pro (compiled into the binary as SAILTUBE_VERSION) is a mirror that
`version` requires to be equal.

version  Print the spec version. Exit 1 when sailtube.pro disagrees or a line is missing.
tag      TAG is `v<version>` (final) or `v<version>-<suffix>` (pre-release); a `refs/tags/`
         prefix is accepted. Reads the spec and sailtube.pro at the tagged commit and fails when
         the tag's version (before the first `-`) differs from the spec version, or when the
         tagged commit is not an ancestor of the main ref (default `main`; when it does not
         resolve, `origin/<ref>` is tried, as in a CI clone). Prints `release: prerelease` or
         `release: final` on success.
payload  Compare the payload files of two RPMs: every path, its mode, its symlink target and
         the SHA-256 of its content. Timestamps, inodes and owners are ignored. The payload is
         read in Python (gzip, bzip2, xz/lzma); zstd payloads go through `rpm2cpio` or `zstd`
         when either is on PATH. Exit 1 on any difference.

Exit codes: 0 ok, 1 check failed, 2 usage or environment error.
"""
import argparse
import bz2
import gzip
import hashlib
import io
import lzma
import os
import re
import shutil
import struct
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SPEC_PATH = "rpm/harbour-sailtube.spec"
PRO_PATH = "sailtube.pro"

SPEC_VERSION = re.compile(r"^Version:\s*(\S+)\s*$", re.M)
PRO_VERSION = re.compile(r"^\s*VERSION\s*=\s*(\S+)\s*$", re.M)
VERSION_SHAPE = re.compile(r"^\d+(?:\.\d+)*$")
TAG_SHAPE = re.compile(r"^v?(\d+(?:\.\d+)*)(?:-([0-9A-Za-z][0-9A-Za-z.\-]*))?$")


class CheckError(Exception):
    """A check failed (exit 1)."""


class UsageError(Exception):
    """Bad input or a missing tool (exit 2)."""


# ---------------------------------------------------------------------------- version


def _single(pattern, text, what):
    found = pattern.findall(text)
    if len(found) != 1:
        raise CheckError("%s: expected exactly one version line, found %d" % (what, len(found)))
    value = found[0]
    if not VERSION_SHAPE.match(value):
        raise CheckError("%s: version %r is not a plain dotted number" % (what, value))
    return value


def versions_from_text(spec_text, pro_text):
    """Return the single-source version; raise CheckError when the mirror disagrees."""
    spec = _single(SPEC_VERSION, spec_text, SPEC_PATH)
    pro = _single(PRO_VERSION, pro_text, PRO_PATH)
    if spec != pro:
        raise CheckError("%s VERSION %s != %s Version %s (the spec is the single source)"
                         % (PRO_PATH, pro, SPEC_PATH, spec))
    return spec


def _read(path):
    try:
        with open(path, encoding="utf-8") as fh:
            return fh.read()
    except OSError as exc:
        raise CheckError("cannot read %s: %s" % (path, exc.strerror))


def tree_version(root):
    return versions_from_text(_read(os.path.join(root, SPEC_PATH)), _read(os.path.join(root, PRO_PATH)))


# ---------------------------------------------------------------------------- tag


def parse_tag(tag):
    """Return (version, prerelease) for a tag name; raise CheckError for other shapes."""
    name = tag[len("refs/tags/"):] if tag.startswith("refs/tags/") else tag
    m = TAG_SHAPE.match(name)
    if not m:
        raise CheckError("tag %r is not v<version> or v<version>-<suffix>" % tag)
    return m.group(1), "-" in name


def _git(repo, *args):
    exe = shutil.which("git")
    if not exe:
        raise UsageError("git is not on PATH (the tag check needs the git history)")
    return subprocess.run([exe, "-C", repo] + list(args), stdout=subprocess.PIPE,
                          stderr=subprocess.PIPE, universal_newlines=True)


def _resolve(repo, ref):
    r = _git(repo, "rev-parse", "--verify", "--quiet", ref + "^{commit}")
    return r.stdout.strip() if r.returncode == 0 and r.stdout.strip() else None


def check_tag(tag, repo, main_ref):
    """Return 'prerelease' or 'final'; raise CheckError listing every failed condition."""
    tag_version, prerelease = parse_tag(tag)
    name = tag[len("refs/tags/"):] if tag.startswith("refs/tags/") else tag
    if _git(repo, "rev-parse", "--git-dir").returncode != 0:
        raise UsageError("%s is not a git repository" % repo)
    commit = _resolve(repo, "refs/tags/" + name)
    if not commit:
        raise CheckError("tag %s does not exist in %s" % (name, repo))
    main = main_ref
    main_commit = _resolve(repo, main_ref)
    if not main_commit and "/" not in main_ref:
        main = "origin/" + main_ref
        main_commit = _resolve(repo, "refs/remotes/" + main)
    if not main_commit:
        raise CheckError("main ref %s does not resolve (fetch it, or pass --main-ref)" % main_ref)

    errors = []
    spec = _git(repo, "show", "%s:%s" % (commit, SPEC_PATH))
    pro = _git(repo, "show", "%s:%s" % (commit, PRO_PATH))
    if spec.returncode != 0 or pro.returncode != 0:
        errors.append("the tagged commit has no %s or %s" % (SPEC_PATH, PRO_PATH))
    else:
        try:
            spec_version = versions_from_text(spec.stdout, pro.stdout)
            if tag_version != spec_version:
                errors.append("tag %s has version %s but the spec at %s says %s"
                              % (name, tag_version, commit[:12], spec_version))
        except CheckError as exc:
            errors.append(str(exc))
    anc = _git(repo, "merge-base", "--is-ancestor", commit, main_commit)
    if anc.returncode == 1:
        errors.append("tagged commit %s is not on %s (tag only commits on main)" % (commit[:12], main))
    elif anc.returncode != 0:
        errors.append("cannot test ancestry against %s: %s" % (main, anc.stderr.strip()))
    if errors:
        raise CheckError("\n".join(errors))
    print("tag: %s -> %s at %s, on %s" % (name, tag_version, commit[:12], main))
    return "prerelease" if prerelease else "final"


# ---------------------------------------------------------------------------- cpio (newc)

CPIO_HEADER = 110
CPIO_MAGICS = (b"070701", b"070702")
S_IFMT, S_IFREG, S_IFLNK = 0o170000, 0o100000, 0o120000


def _exact(stream, n):
    parts, left = [], n
    while left:
        chunk = stream.read(min(left, 1 << 20))
        if not chunk:
            raise CheckError("truncated cpio archive")
        parts.append(chunk)
        left -= len(chunk)
    return b"".join(parts)


def _skip_pad(stream, consumed):
    pad = (4 - consumed % 4) % 4
    if pad:
        _exact(stream, pad)


def _norm(name):
    while name.startswith("./"):
        name = name[2:]
    return "/" + name.lstrip("/") if name not in (".", "") else "/"


def read_newc(stream):
    """Read an uncompressed newc cpio stream into {path: (mode, digest_or_link)}.

    Regular files map to their SHA-256 hex digest, symlinks to 'link:<target>', anything else
    to ''. Hard links (newc stores the data only on the last member) share one digest.
    """
    entries, by_inode, pending = {}, {}, []
    while True:
        head = _exact(stream, CPIO_HEADER)
        if head[:6] not in CPIO_MAGICS:
            raise CheckError("not a newc cpio archive (magic %r)" % head[:6])
        fields = [int(head[6 + 8 * i:14 + 8 * i], 16) for i in range(13)]
        ino, mode, nlink, size = fields[0], fields[1], fields[4], fields[6]
        dev = (fields[7], fields[8])
        namesize = fields[11]
        name = _exact(stream, namesize).rstrip(b"\0").decode("utf-8", "surrogateescape")
        _skip_pad(stream, CPIO_HEADER + namesize)
        if name == "TRAILER!!!":
            break
        path = _norm(name)
        kind = mode & S_IFMT
        if kind == S_IFLNK:
            value = "link:" + _exact(stream, size).decode("utf-8", "surrogateescape")
        else:
            h = hashlib.sha256()
            left = size
            while left:
                chunk = _exact(stream, min(left, 1 << 20))
                h.update(chunk)
                left -= len(chunk)
            value = h.hexdigest() if kind == S_IFREG else ""
            if kind == S_IFREG and nlink > 1:
                key = (dev, ino)
                if size:
                    by_inode[key] = value
                else:
                    pending.append((path, key))
        if size:
            _skip_pad(stream, size)
        entries[path] = (mode, value)
    for path, key in pending:
        if key in by_inode:
            entries[path] = (entries[path][0], by_inode[key])
    return entries


def read_newc_bytes(data):
    return read_newc(io.BytesIO(data))


# ---------------------------------------------------------------------------- rpm


RPM_LEAD = 96
RPM_LEAD_MAGIC = b"\xed\xab\xee\xdb"
RPM_HEADER_MAGIC = b"\x8e\xad\xe8\x01"
RPMTAG_PAYLOADCOMPRESSOR = 1125


def _header(stream, pad):
    intro = _exact(stream, 16)
    if intro[:4] != RPM_HEADER_MAGIC:
        raise CheckError("bad RPM header magic")
    nindex, hsize = struct.unpack(">II", intro[8:16])
    index = _exact(stream, 16 * nindex)
    store = _exact(stream, hsize)
    if pad:
        _skip_pad8(stream, 16 + 16 * nindex + hsize)
    tags = {}
    for i in range(nindex):
        tag, typ, off, count = struct.unpack(">IIII", index[16 * i:16 * i + 16])
        if typ == 6 and off < len(store):  # STRING
            tags[tag] = store[off:store.index(b"\0", off)].decode("utf-8", "replace")
    return tags


def _skip_pad8(stream, consumed):
    pad = (8 - consumed % 8) % 8
    if pad:
        _exact(stream, pad)


def rpm_payload_entries(path):
    """Return read_newc() of an RPM's payload."""
    with open(path, "rb") as fh:
        lead = _exact(fh, RPM_LEAD)
        if lead[:4] != RPM_LEAD_MAGIC:
            raise CheckError("%s is not an RPM" % path)
        _header(fh, pad=True)  # signature header
        tags = _header(fh, pad=False)
        compressor = tags.get(RPMTAG_PAYLOADCOMPRESSOR, "gzip")
        if compressor == "gzip":
            return read_newc(gzip.GzipFile(fileobj=fh))
        if compressor == "bzip2":
            return read_newc(bz2.BZ2File(fh))
        if compressor in ("xz", "lzma"):
            return read_newc(lzma.LZMAFile(fh))
        if compressor == "identity":
            return read_newc(fh)
    for tool in (["rpm2cpio", path], ["zstd", "-dc", "--", "-"]):
        exe = shutil.which(tool[0])
        if not exe:
            continue
        # Unbuffered, so the descriptor handed to zstd sits exactly at the payload.
        stdin = open(path, "rb", buffering=0) if tool[0] == "zstd" else None
        try:
            if stdin:
                _skip_to_payload(stdin)
            proc = subprocess.Popen([exe] + tool[1:], stdin=stdin, stdout=subprocess.PIPE)
            try:
                return read_newc(proc.stdout)
            finally:
                proc.stdout.close()
                if proc.wait() != 0:
                    raise CheckError("%s failed on %s" % (tool[0], path))
        finally:
            if stdin:
                stdin.close()
    raise UsageError("%s payload of %s needs rpm2cpio or zstd on PATH" % (compressor, path))


def _skip_to_payload(fh):
    _exact(fh, RPM_LEAD)
    _header(fh, pad=True)
    _header(fh, pad=False)


def compare_entries(a, b):
    """Return a sorted list of difference lines between two read_newc() results."""
    out = []
    for path in sorted(set(a) | set(b)):
        if path not in b:
            out.append("only in A: %s" % path)
        elif path not in a:
            out.append("only in B: %s" % path)
        elif a[path] != b[path]:
            what = []
            if a[path][0] != b[path][0]:
                what.append("mode %o != %o" % (a[path][0], b[path][0]))
            if a[path][1] != b[path][1]:
                what.append("content")
            out.append("differs: %s (%s)" % (path, ", ".join(what)))
    return out


# ---------------------------------------------------------------------------- cli


def main(argv=None):
    ap = argparse.ArgumentParser(prog="release_check.py", description=__doc__.split("\n\n")[0])
    ap.add_argument("--root", default=ROOT, help="source tree for `version` (default: this checkout)")
    sub = ap.add_subparsers(dest="cmd")
    sub.add_parser("version", help="print the single-source version")
    t = sub.add_parser("tag", help="check a release tag")
    t.add_argument("tag")
    t.add_argument("--repo", default=None, help="git repository (default: --root)")
    t.add_argument("--main-ref", default="main")
    p = sub.add_parser("payload", help="compare the payload files of two RPMs")
    p.add_argument("rpm_a")
    p.add_argument("rpm_b")
    args = ap.parse_args(argv)
    if not args.cmd:
        ap.print_usage(sys.stderr)
        return 2
    try:
        if args.cmd == "version":
            print(tree_version(args.root))
        elif args.cmd == "tag":
            print("release: %s" % check_tag(args.tag, args.repo or args.root, args.main_ref))
        else:
            a = rpm_payload_entries(args.rpm_a)
            b = rpm_payload_entries(args.rpm_b)
            diffs = compare_entries(a, b)
            if diffs:
                for line in diffs:
                    print(line)
                raise CheckError("payload: %d difference(s) in %d/%d entries" % (len(diffs), len(a), len(b)))
            print("payload: %d entries identical" % len(a))
    except CheckError as exc:
        print("release_check %s: FAIL: %s" % (args.cmd, exc), file=sys.stderr)
        return 1
    except UsageError as exc:
        print("release_check %s: %s" % (args.cmd, exc), file=sys.stderr)
        return 2
    return 0


if __name__ == "__main__":
    sys.exit(main())
