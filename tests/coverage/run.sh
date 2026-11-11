#!/bin/sh
# SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
# NFR-TST-06: coverage ratchet (tools/coverage_ratchet.py). Needs only python3; git is optional.
#  1. `check` on this tree against tests/coverage/baseline.txt.
#  2. Scratch copy: a new untested engine function fails; a test that calls it passes; a stale
#     baseline entry (tested now, or gone) fails and `update` removes it.
#  3. Temporary git repository: a baseline entry added after the base commit fails `growth`
#     (even though `check` passes); an unchanged or shrunk baseline passes. Skipped without git.
# The names used below live only in this file; tests/coverage/** is not part of the test corpus.
set -eu
here=$(cd "$(dirname "$0")" && pwd)
root=$(cd "$here/../.." && pwd)
tool="$root/tools/coverage_ratchet.py"

fail() { echo "coverage: FAIL ($1)"; [ -f "$scratch/out.txt" ] && cat "$scratch/out.txt"; exit 1; }

python3 -I "$tool" check

scratch=$(mktemp -d "${TMPDIR:-/tmp}/sailtube-coverage.XXXXXX")
trap 'rm -rf "$scratch"' EXIT
tree="$scratch/tree"
mkdir -p "$tree/tools" "$tree/Shared"
(cd "$root" && tar -cf - Core Shared/Playback tests) | tar -xf - -C "$tree"
cp "$tool" "$tree/tools/coverage_ratchet.py"
st="$tree/tools/coverage_ratchet.py"
baseline="$tree/tests/coverage/baseline.txt"

python3 -I "$st" check > "$scratch/out.txt" 2>&1 || fail "the scratch copy does not pass"

# 2a. A new untested engine function (free function and public method) must fail.
mkdir -p "$tree/Core/RatchetScratch"
cat > "$tree/Core/RatchetScratch/RatchetScratch.h" <<'EOF'
#pragma once
namespace nordtile { namespace video { namespace scratch {
int ratchetScratchFree(int value);
class RatchetScratchBox {
public:
    explicit RatchetScratchBox(int v);
    int ratchetScratchMethod() const { return v_; }
signals:
    void ratchetScratchSignal();
private:
    int ratchetScratchPrivate() const;
    int v_;
};
}}}
EOF
if python3 -I "$st" check > "$scratch/out.txt" 2>&1; then fail "an untested engine function was accepted"; fi
grep -q "Core/RatchetScratch/RatchetScratch.h::ratchetScratchFree: add a test" "$scratch/out.txt" \
    || fail "free function not reported"
grep -q "Core/RatchetScratch/RatchetScratch.h::RatchetScratchBox::ratchetScratchMethod: add a test" \
    "$scratch/out.txt" || fail "method not reported"
if grep -q "ratchetScratchSignal\|ratchetScratchPrivate\|::RatchetScratchBox: " "$scratch/out.txt"; then
    fail "a signal, private member or constructor was counted"
fi
echo "coverage: untested engine function rejected as expected"

# 2b. A test that calls both makes it pass.
mkdir -p "$tree/tests/ratchet_scratch"
cat > "$tree/tests/ratchet_scratch/ScratchTest.cpp" <<'EOF'
// ratchetScratchMethod in a comment alone would not count.
#include "Core/RatchetScratch/RatchetScratch.h"
int main() { return ratchetScratchFree(0) + scratch::RatchetScratchBox(0).ratchetScratchMethod(); }
EOF
python3 -I "$st" check > "$scratch/out.txt" 2>&1 || fail "a tested engine function was rejected"
echo "coverage: tested engine function accepted"

# 2c. A comment is not a test.
printf '%s\n' '// ratchetScratchMethod is mentioned here only' > "$tree/tests/ratchet_scratch/ScratchTest.cpp"
printf '%s\n' 'int main() { return ratchetScratchFree(0); }' >> "$tree/tests/ratchet_scratch/ScratchTest.cpp"
if python3 -I "$st" check > "$scratch/out.txt" 2>&1; then fail "a mention in a comment counted as a test"; fi
printf '%s\n' 'int main() { return ratchetScratchFree(0) + RatchetScratchBox(0).ratchetScratchMethod(); }' \
    > "$tree/tests/ratchet_scratch/ScratchTest.cpp"

# 2d. A stale baseline entry (tested now, or gone) must fail; `update` removes it and never adds.
cp "$baseline" "$scratch/baseline.orig"
{ grep '^#' "$scratch/baseline.orig"
  { grep -v '^#' "$scratch/baseline.orig"
    echo "Core/RatchetScratch/RatchetScratch.h::ratchetScratchFree"
    echo "Core/RatchetScratch/RatchetScratch.h::ratchetScratchGone"; } | LC_ALL=C sort; } > "$baseline"
if python3 -I "$st" check > "$scratch/out.txt" 2>&1; then fail "a stale baseline entry was accepted"; fi
grep -q "ratchetScratchFree is tested now: remove it from the baseline: it may only shrink" "$scratch/out.txt" \
    || fail "tested baseline entry not reported"
grep -q "ratchetScratchGone no longer exists: remove it from the baseline" "$scratch/out.txt" \
    || fail "missing function not reported"
python3 -I "$st" update > "$scratch/out.txt" 2>&1 || fail "update failed"
cmp -s "$baseline" "$scratch/baseline.orig" || fail "update did not restore the baseline"
python3 -I "$st" check > "$scratch/out.txt" 2>&1 || fail "check after update"
rm "$tree/tests/ratchet_scratch/ScratchTest.cpp"
python3 -I "$st" update > "$scratch/out.txt" 2>&1 || fail "update failed"
cmp -s "$baseline" "$scratch/baseline.orig" || fail "update added entries"
echo "coverage: stale baseline entries rejected; update only shrinks"

# 3. Baseline growth against git history.
if ! command -v git > /dev/null 2>&1; then
    echo "coverage: NOTE git not available, growth check skipped"
    echo "coverage: PASS"
    exit 0
fi
unset GIT_DIR GIT_WORK_TREE GIT_INDEX_FILE 2>/dev/null || true
rm -rf "$tree/Core/RatchetScratch" "$tree/tests/ratchet_scratch"
g() { git -C "$tree" -c user.name=ratchet -c user.email=ratchet@example.invalid \
          -c commit.gpgsign=false -c core.hooksPath=/dev/null "$@"; }
g init -q
g add -A
g commit -q -m base
python3 -I "$st" growth --base HEAD --repo "$tree" > "$scratch/out.txt" 2>&1 || fail "unchanged baseline failed growth"
# A new untested function together with a new baseline entry passes `check` but fails `growth`.
mkdir -p "$tree/Core/RatchetScratch"
printf '%s\n' '#pragma once' 'int ratchetScratchFree(int value);' > "$tree/Core/RatchetScratch/RatchetScratch.h"
{ grep '^#' "$scratch/baseline.orig"
  { grep -v '^#' "$scratch/baseline.orig"
    echo "Core/RatchetScratch/RatchetScratch.h::ratchetScratchFree"; } | LC_ALL=C sort; } > "$baseline"
python3 -I "$st" check > "$scratch/out.txt" 2>&1 || fail "check with a listed untested function"
if python3 -I "$st" growth --base HEAD --repo "$tree" > "$scratch/out.txt" 2>&1; then
    fail "a grown baseline was accepted"
fi
grep -q "new baseline entry Core/RatchetScratch/RatchetScratch.h::ratchetScratchFree" "$scratch/out.txt" \
    || fail "unexpected growth error text"
echo "coverage: grown baseline rejected as expected"
# Shrinking is fine.
grep -v 'DownloadPolicy.h::isTransfer$' "$scratch/baseline.orig" > "$baseline"
python3 -I "$st" growth --base HEAD --repo "$tree" > "$scratch/out.txt" 2>&1 || fail "a shrunk baseline failed growth"
echo "coverage: shrunk baseline accepted"
echo "coverage: PASS"
