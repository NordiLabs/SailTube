# SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
# Shared preamble and teardown for SailTube device experiments (docs/experiments/README.md).
#
# Sourced, never executed. POSIX sh. Runs ON THE DEVICE as the session user (defaultuser), over
# ssh, started by the maintainer only, after the phone owner agrees.
#
#   . "$(dirname "$0")/common.sh"
#   exp_begin E3 120          # name, expected duration in seconds
#   some-pipeline & exp_register_pid $!
#   ...                       # exp_end runs from the EXIT trap
#
# exp_begin refuses to run as root, inside SailTune's :17/:52 test windows, or when any
# PulseAudio sink is not muted at 0 % after muting. Nothing here ever raises a volume or unmutes.

EXP_UID=$(id -u)
EXP_USER=$(id -un 2>/dev/null || echo "uid$EXP_UID")
# ssh sessions do not always carry the session environment; pactl and dbus-send need it.
: "${XDG_RUNTIME_DIR:=/run/user/$EXP_UID}"
: "${DBUS_SESSION_BUS_ADDRESS:=unix:path=$XDG_RUNTIME_DIR/dbus/user_bus_socket}"
export XDG_RUNTIME_DIR DBUS_SESSION_BUS_ADDRESS
# Inside the Sailjail layout (PLT-03), so E2 can read the same files from the sandbox.
EXP_ROOT=${SAILTUBE_EXP_ROOT:-$HOME/.cache/org.nordilabs.sailtube/SailTube/experiments}
EXP_TOOLS=$(cd "$(dirname "$0")" && pwd)
# shellcheck disable=SC2034 # used, unquoted on purpose, by the experiment scripts
EXP_PY="python3 -I"
EXP_DIR=
EXP_PIDS=
EXP_ENDED=0

exp_have() { command -v "$1" >/dev/null 2>&1; }

exp_log() {
    _line="$(date -u +%Y-%m-%dT%H:%M:%SZ) $*"
    printf '%s\n' "$_line" >&2
    [ -n "$EXP_DIR" ] && printf '%s\n' "$_line" >> "$EXP_DIR/run.log"
    return 0
}

exp_die() { exp_log "ABORT: $*"; exit 3; }

# Milliseconds since boot from /proc/uptime, the clock every sampler line uses.
exp_now_ms() { awk '{ printf "%d\n", $1 * 1000 }' /proc/uptime; }

exp_require_session_user() {
    [ "$EXP_UID" -ne 0 ] || exp_die "run as the session user (defaultuser), not root"
    [ "$EXP_USER" = defaultuser ] || exp_log "WARNING: user is $EXP_USER, expected defaultuser"
}

# Refuses to start when [now, now + duration] touches SailTune's device-test windows:
# from 2 min before to 10 min after :17 and :52. SAILTUBE_EXP_IGNORE_WINDOW=1 overrides, only after
# the maintainer has cleared the slot with SailTune.
# Free slots are therefore :02-:15 and :27-:50; a step longer than 23 min never fits.
exp_window_clash() { # duration_s -> prints the clashing minute, or nothing
    [ "${SAILTUBE_EXP_IGNORE_WINDOW:-0}" = 1 ] && return 0
    _start=$(( $(date +%M | sed 's/^0//') * 60 + $(date +%S | sed 's/^0//') ))
    awk -v s="$_start" -v d="${1:-60}" 'BEGIN {
        for (t = s; t <= s + d; t += 30) {
            m = (t % 3600) / 60
            if ((m >= 15 && m < 27) || m >= 50 || m < 2) { print int(m); exit }
        }
    }'
}

exp_check_window() {
    [ "${SAILTUBE_EXP_IGNORE_WINDOW:-0}" = 1 ] && exp_log "window guard overridden"
    _clash=$(exp_window_clash "${1:-60}")
    [ -z "$_clash" ] || exp_die "run would overlap SailTune's :17/:52 window (minute $_clash); wait"
}

# Between steps of a long experiment: waits (nothing playing) until the next step fits, at most
# 40 min, then gives up.
exp_wait_window() {
    _waited=0
    while _clash=$(exp_window_clash "${1:-60}") && [ -n "$_clash" ]; do
        [ $_waited -ge 2400 ] && exp_die "no free slot for a ${1:-60} s step after 40 min"
        [ $_waited -eq 0 ] && exp_log "waiting for SailTune's window to pass (minute $_clash)"
        sleep 30
        _waited=$((_waited + 30))
    done
}

# Mutes every sink: volume 0 and mute on. Safe to call repeatedly (new sinks such as Bluetooth or
# a call route can appear during a run).
exp_mute_all() {
    exp_have pactl || exp_die "pactl not found; cannot enforce the mute rule"
    LC_ALL=C pactl info >/dev/null 2>&1 || exp_die "cannot reach PulseAudio as $EXP_USER"
    LC_ALL=C pactl list short sinks | while read -r _idx _name _rest; do
        pactl set-sink-volume "$_idx" 0 && pactl set-sink-mute "$_idx" 1 \
            || echo "mute failed for sink $_idx ($_name)" >&2
    done
}

# Prints one line per sink and returns non-zero unless every sink is muted with every channel at
# 0 %. Only the "Volume:" line counts ("Base Volume:" is a hardware reference, not a setting).
exp_verify_muted() {
    LC_ALL=C pactl list sinks | awk '
        /^Sink #/ { if (n) report(); n++; id = $2; mute = "?"; vol = "ok"; name = "" }
        $1 == "Name:" { name = $2 }
        $1 == "Mute:" { mute = $2 }
        $1 == "Volume:" { for (i = 2; i <= NF; i++) if ($i ~ /%$/ && $i != "0%") vol = "nonzero" }
        function report() {
            ok = (mute == "yes" && vol == "ok")
            if (!ok) bad++
            printf "sink %s %s mute=%s volume=%s %s\n", id, name, mute, vol, ok ? "OK" : "NOT-MUTED"
        }
        END { if (n) report(); if (!n) { print "no sinks listed"; exit 2 } exit bad ? 1 : 0 }'
}

exp_mute_and_verify() {
    exp_mute_all
    if exp_verify_muted > "$EXP_DIR/mute.$1.txt" 2>&1; then
        exp_log "mute verified ($1): $(tr '\n' ';' < "$EXP_DIR/mute.$1.txt")"
    else
        cat "$EXP_DIR/mute.$1.txt" >&2
        return 1
    fi
}

exp_record_env() {
    {
        echo "date_utc=$(date -u +%Y-%m-%dT%H:%M:%SZ)"
        echo "user=$EXP_USER uid=$EXP_UID"
        echo "experiment=$1 tools=$EXP_TOOLS"
        echo "tools_commit=${SAILTUBE_EXP_COMMIT:-unknown}"
        for f in /etc/sailfish-release /etc/os-release /etc/hw-release; do
            [ -r "$f" ] && sed "s|^|$(basename "$f"): |" "$f"
        done
        echo "uname=$(uname -srm)"
        # 32-bit userland on a 64-bit kernel exists (spec §1.3): record the userland word size.
        echo "userland_bits=$(getconf LONG_BIT 2>/dev/null || echo unknown)"
        if exp_have gst-inspect-1.0; then
            gst-inspect-1.0 --version 2>&1 | sed 's/^/gst: /'
        else
            echo "gst: gst-inspect-1.0 not installed (package gstreamer1.0-tools)"
        fi
        exp_have gst-launch-1.0 || echo "gst: gst-launch-1.0 not installed"
        exp_have pactl && pactl --version 2>&1 | head -n 2 | sed 's/^/pulse: /'
        if exp_have python3; then
            echo "python=$(python3 --version 2>&1)"
            python3 -I -c 'import gi; gi.require_version("Gst", "1.0"); from gi.repository import Gst; Gst.init(None); print("python_gi_gst=" + Gst.version_string())' 2>/dev/null \
                || echo "python_gi_gst=missing"
        else
            echo "python=missing"
        fi
        echo "sinks:"
        LC_ALL=C pactl list short sinks 2>&1 | sed 's/^/  /'
    } > "$EXP_DIR/env.txt" 2>&1
}

exp_register_pid() { EXP_PIDS="$EXP_PIDS $1"; }

# SIGINT first (gst-launch-1.0 -e turns it into EOS), then TERM, then KILL.
exp_stop_pids() {
    for _p in $EXP_PIDS; do kill -0 "$_p" 2>/dev/null && kill -INT "$_p" 2>/dev/null; done
    for _sig in TERM KILL; do
        _alive=
        for _p in $EXP_PIDS; do kill -0 "$_p" 2>/dev/null && _alive="$_alive $_p"; done
        [ -z "$_alive" ] && break
        sleep 2
        for _p in $_alive; do kill -0 "$_p" 2>/dev/null && kill -"$_sig" "$_p" 2>/dev/null; done
    done
    EXP_PIDS=
}

# Pauses every MPRIS player on the session bus (SailTube, SailTune, the system media player).
exp_pause_players() {
    if ! exp_have dbus-send; then
        exp_log "dbus-send missing: pause every player by hand"
        return 0
    fi
    for _n in $(dbus-send --session --print-reply --dest=org.freedesktop.DBus /org/freedesktop/DBus \
                    org.freedesktop.DBus.ListNames 2>/dev/null \
                | sed -n 's/.*string "\(org\.mpris\.MediaPlayer2\.[^"]*\)".*/\1/p'); do
        dbus-send --session --type=method_call --dest="$_n" /org/mpris/MediaPlayer2 \
            org.mpris.MediaPlayer2.Player.Pause 2>/dev/null \
            && exp_log "paused $_n" || exp_log "WARNING: could not pause $_n"
    done
}

exp_end() {
    _rc=$?
    [ "$EXP_ENDED" = 1 ] && return 0
    EXP_ENDED=1
    exp_stop_pids
    if [ "${EXP_NO_PAUSE:-0}" = 1 ]; then
        exp_log "player pause skipped (EXP_NO_PAUSE=1; a later step must pause)"
    else
        exp_pause_players
    fi
    if [ -n "$EXP_DIR" ]; then
        exp_mute_and_verify end || exp_log "WARNING: a sink is not muted at the end; check by hand"
        exp_log "sinks left muted at 0 %; restoring volume is the user's decision"
        exp_log "end rc=$_rc results in $EXP_DIR"
    fi
}

# exp_begin <name> <expected duration s>: identity checks, window guard, result directory,
# environment record, mute + verify (abort on failure), trap.
exp_begin() {
    exp_require_session_user
    exp_check_window "${2:-60}"
    EXP_DIR="$EXP_ROOT/$1-$(date -u +%Y%m%dT%H%M%SZ)"
    mkdir -p "$EXP_DIR" || exp_die "cannot create $EXP_DIR"
    trap exp_end EXIT
    trap 'exit 130' INT TERM HUP
    exp_record_env "$1"
    exp_mute_and_verify start || exp_die "sinks are not all muted at 0 %; not starting"
    exp_log "begin $1 (expected ${2:-60} s) in $EXP_DIR"
}
