#!/bin/sh
# SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
# E2 / RES-17 / PLT-03: does an in-process loopback hand-off work under Sailjail?
# Runs ON THE DEVICE as defaultuser, started by the maintainer only. See docs/experiments/E2_loopback_sailjail.md.
#
#   e2_loopback.sh CLIP          (a short H.264 .mp4 or VP9 .webm; it is copied into the app's cache)
#
# Runs e2_loopback.py three or four times:
#   A host         no sandbox (baseline)
#   B internet     sailjail -p harbour-sailtube.desktop (Permissions=Internet;Audio)
#   C nointernet   sailjail -p harbour-sailtube-e2-nonet.desktop (Permissions=Audio), only if the
#                  maintainer installed that profile (needs root; see the doc); skipped otherwise
#   D app          sailjail -p harbour-sailtube.desktop -- /usr/bin/harbour-sailtube --experiment=loopback
#                  only with E2_APP_MODE=1, once the app implements that mode
# Sailjail CLI form from the public sailjail README: `sailjail -p <name>.desktop -- <absolute exe> [args]`.
# The profile file must be in /usr/share/applications or /etc/sailjail/applications.
set -u
. "$(dirname "$0")/common.sh"

[ $# -eq 1 ] && [ -r "$1" ] || { echo "usage: $0 CLIP" >&2; exit 2; }
exp_begin E2 180
exp_have python3 || exp_die "python3 missing"

# Everything the sandboxed process reads must sit inside its whitelisted PLT-03 directories.
work="$EXP_DIR/work"
mkdir -p "$work"
cp "$EXP_TOOLS/e2_loopback.py" "$EXP_TOOLS/range_server.py" "$work/"
cp "$1" "$work/" || exp_die "cannot copy clip"
clip="$work/$(basename "$1")"
case "$work" in
    "$HOME/.cache/org.nordilabs.sailtube/SailTube/"*) ;;
    *) exp_log "WARNING: $work is outside the app's cache dir; the sandbox may not see it" ;;
esac

run_case() { # label command...
    _label=$1; shift
    exp_mute_and_verify "$_label" || exp_die "mute lost before $_label"
    exp_log "case $_label: $*"
    "$@" > "$EXP_DIR/case.$_label.txt" 2>&1
    _rc=$?
    exp_log "case $_label: exit $_rc"
    grep '^e2: ' "$EXP_DIR/case.$_label.txt" >&2 || {
        exp_log "case $_label: no stage lines; the launcher itself failed (exec/profile error):"
        tail -n 20 "$EXP_DIR/case.$_label.txt" >&2
    }
}

PY=/usr/bin/python3
run_case host $PY -I "$work/e2_loopback.py" --clip "$clip" --label host

if exp_have sailjail; then
    SJ=$(command -v sailjail)
    if [ -r /usr/share/applications/harbour-sailtube.desktop ]; then
        cp /usr/share/applications/harbour-sailtube.desktop "$EXP_DIR/"
        run_case internet "$SJ" -p harbour-sailtube.desktop -- $PY -I "$work/e2_loopback.py" \
            --clip "$clip" --label internet
    else
        exp_log "case internet: SKIPPED, /usr/share/applications/harbour-sailtube.desktop missing (install the RPM)"
    fi
    nonet=/etc/sailjail/applications/harbour-sailtube-e2-nonet.desktop
    if [ -r "$nonet" ]; then
        cp "$nonet" "$EXP_DIR/"
        run_case nointernet "$SJ" -p harbour-sailtube-e2-nonet.desktop -- $PY -I "$work/e2_loopback.py" \
            --clip "$clip" --label nointernet
    else
        exp_log "case nointernet: SKIPPED, $nonet not installed (root step, see E2 doc)"
    fi
    if [ "${E2_APP_MODE:-0}" = 1 ]; then
        run_case app "$SJ" -p harbour-sailtube.desktop -- /usr/bin/harbour-sailtube \
            --experiment=loopback --experiment-clip="$clip"
    else
        exp_log "case app: SKIPPED (E2_APP_MODE=1 once harbour-sailtube has --experiment=loopback)"
    fi
else
    exp_log "sailjail not found: only the host baseline ran"
fi

grep -h '^e2: ' "$EXP_DIR"/case.*.txt > "$EXP_DIR/stages.txt" 2>/dev/null
exp_log "pass criterion (E2): case internet (and app) has stage=gst result=ok (frames > 0)"
