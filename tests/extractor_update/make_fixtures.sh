#!/bin/sh
# SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
# Regenerates tests/extractor_update/fixtures/fake (run on a developer machine with gpg; the tests
# themselves never need gpg). Throwaway keys are generated in a temporary GNUPGHOME that is deleted
# afterwards: only the public keys, the fingerprints and the signatures are kept.
#
#   test key   RSA-2048, signs every good release           -> fake/test-key.asc, fake/test-key.fpr
#   other key  RSA-2048, signs release wrongkey-*            -> fake/other-key.asc, fake/other-key.fpr
#   eddsa key  Ed25519, signs release ed25519-* (algorithm refused)
#
# Each release directory fake/<name>-<tag>/ holds yt-dlp (the archive), SHA2-256SUMS and
# SHA2-256SUMS.sig, as a GitHub release of yt-dlp does.
set -eu
here=$(cd "$(dirname "$0")" && pwd)
out="$here/fixtures/fake"
command -v gpg >/dev/null 2>&1 || { echo "make_fixtures: gpg is required"; exit 2; }
home=$(mktemp -d "${TMPDIR:-/tmp}/xupd-gpg.XXXXXX")
trap 'gpgconf --homedir "$home" --kill all >/dev/null 2>&1 || true; rm -rf "$home"' EXIT
chmod 700 "$home"
g() { gpg --homedir "$home" --batch --quiet --no-tty "$@"; }

genkey() {   # NAME TYPE-LINES
    printf '%%no-protection\n%s\nName-Real: SailTube throwaway %s key\nName-Email: %s@sailtube.invalid\nExpire-Date: 0\n%%commit\n' \
        "$2" "$1" "$1" | g --gen-key
    g --with-colons --list-keys "$1@sailtube.invalid" | awk -F: '/^fpr:/ { print $10; exit }'
}

rm -rf "$out"
mkdir -p "$out"
test_fpr=$(genkey test "Key-Type: RSA
Key-Length: 2048
Key-Usage: sign")
other_fpr=$(genkey other "Key-Type: RSA
Key-Length: 2048
Key-Usage: sign")
eddsa_fpr=$(genkey eddsa "Key-Type: EDDSA
Key-Curve: ed25519
Key-Usage: sign")
g --armor --export "$test_fpr" > "$out/test-key.asc"
g --armor --export "$other_fpr" > "$out/other-key.asc"
printf '%s\n' "$test_fpr" > "$out/test-key.fpr"
printf '%s\n' "$other_fpr" > "$out/other-key.fpr"

python3 -I "$here/make_archives.py" "$out/archives"

release() {   # NAME TAG ARCHIVE SIGNER [extra gpg sign options]
    dir="$out/$1-$2"
    mkdir -p "$dir"
    cp "$out/archives/$3" "$dir/yt-dlp"
    sum=$(shasum -a 256 "$dir/yt-dlp" | cut -d' ' -f1)
    {
        printf '%s  %s\n' "$sum" yt-dlp
        printf '%s  %s\n' 0000000000000000000000000000000000000000000000000000000000000000 yt-dlp.exe
        printf '%s  %s\n' 1111111111111111111111111111111111111111111111111111111111111111 yt-dlp.tar.gz
        printf '%s  %s\n' 2222222222222222222222222222222222222222222222222222222222222222 yt-dlp_linux_aarch64
    } > "$dir/SHA2-256SUMS"
    signer=$4; shift 4
    g --local-user "$signer" --detach-sign "$@" -o "$dir/SHA2-256SUMS.sig" "$dir/SHA2-256SUMS"
    gpg --homedir "$home" --batch --quiet --verify "$dir/SHA2-256SUMS.sig" "$dir/SHA2-256SUMS" 2>/dev/null
}
release stable 2026.11.01 stable.pyz "$test_fpr" --digest-algo SHA256
release stable 2026.11.20 stable-next.pyz "$test_fpr" --digest-algo SHA256
release nightly 2026.11.04.232659 nightly.pyz "$test_fpr" --digest-algo SHA512 --armor
release dotdot 2026.11.02 dotdot.pyz "$test_fpr" --digest-algo SHA256
release absolute 2026.11.02 absolute.pyz "$test_fpr" --digest-algo SHA256
release noversion 2026.11.02 noversion.pyz "$test_fpr" --digest-algo SHA256
release mismatch 2026.11.03 mismatch.pyz "$test_fpr" --digest-algo SHA256
release wrongkey 2026.11.01 stable.pyz "$other_fpr" --digest-algo SHA256
release ed25519 2026.11.01 stable.pyz "$eddsa_fpr"
# The nightly signature is armoured; its file name stays SHA2-256SUMS.sig.
rm -rf "$out/archives"
echo "make_fixtures: wrote $out (test key $test_fpr)"
