#!/bin/sh
# SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
# E8 / PLY-23 / PLT-12 / §6 M3, M4: which media.role and audio-resource use give "pause on call,
# duck on navigation" on SFOS 5.x?
# Runs ON THE DEVICE as defaultuser, started by the maintainer only. See docs/experiments/E8_audio_role_resource.md.
#
#   e8_audio_role.sh [ROLE...]        default roles: x-maemo music media video
#
# Per role: a silent stream (audiotestsrc wave=silence) into pulsesink volume=0 with
# stream-properties media.role=ROLE, on sinks that are already muted at 0 %. It logs the sink-input
# properties once, then polls Corked/Mute/Sink every E8_POLL_S (2) seconds for E8_HOLD_S (90)
# seconds while the operator triggers a call / alarm / navigation prompt (the manual part).
# With E8_RESOURCE=media|game (and python3-gobject + libaudioresource.so.1) the stream is played by
# e8_resource.py instead, which acquires the audio resource before PLAYING and logs its callbacks.
set -u
. "$(dirname "$0")/common.sh"

HOLD=${E8_HOLD_S:-90}
POLL=${E8_POLL_S:-2}
RESOURCE=${E8_RESOURCE:-none}
[ $# -ge 1 ] || set -- x-maemo music media video
exp_begin E8 $((HOLD + 30))
exp_have pactl || exp_die "pactl missing"

LC_ALL=C pactl list modules short > "$EXP_DIR/pa-modules.txt" 2>&1
grep -i -E 'policy|resource|role|duck|cork' "$EXP_DIR/pa-modules.txt" | sed 's/^/policy module: /' >&2

# Prints the sink-input block(s) whose properties mention our client name.
our_inputs() {
    LC_ALL=C pactl list sink-inputs | awk -v tag="$1" '
        /^Sink Input #/ { if (blk != "" && hit) printf "%s", blk; blk = ""; hit = 0 }
        { blk = blk $0 "\n"; if (index($0, tag)) hit = 1 }
        END { if (blk != "" && hit) printf "%s", blk }'
}

for role in "$@"; do
    tag="sailtube-e8-$role"
    exp_wait_window $((HOLD + 20))
    exp_mute_and_verify "$role" || exp_die "mute lost before role $role"
    if [ "$RESOURCE" = none ]; then
        exp_have gst-launch-1.0 || exp_die "gst-launch-1.0 missing"
        gst-launch-1.0 -e audiotestsrc wave=silence is-live=true ! audioconvert ! audioresample ! \
            pulsesink volume=0.0 client-name="$tag" \
            stream-properties="props,media.role=(string)$role,media.name=(string)$tag" \
            > "$EXP_DIR/gst.$role.txt" 2>&1 &
        exp_register_pid $!
    else
        $EXP_PY "$EXP_TOOLS/e8_resource.py" --role "$role" --resource "$RESOURCE" --name "$tag" \
            --hold "$HOLD" > "$EXP_DIR/resource.$role.txt" 2>&1 &
        exp_register_pid $!
    fi
    sleep 3
    # The new stream lands on a muted sink; check again before the hold starts.
    exp_mute_and_verify "$role.playing" || exp_die "mute lost after stream start ($role)"
    our_inputs "$tag" > "$EXP_DIR/sink-input.$role.txt"
    if [ -s "$EXP_DIR/sink-input.$role.txt" ]; then
        exp_log "role $role: $(grep -E 'media.role|policy\.|Sink:|Corked:|application.process.id|module-stream-restore' \
            "$EXP_DIR/sink-input.$role.txt" | tr -s ' \t' ' ' | tr '\n' ';')"
    else
        exp_log "role $role: sink input NOT FOUND (see gst.$role.txt / resource.$role.txt)"
    fi
    exp_log ">>> OPERATOR: role $role is playing silently for $HOLD s. Trigger now, in order:" \
        "incoming call (ring >= 10 s, then reject), alarm, navigation prompt. Note the wall-clock times."
    poll="$EXP_DIR/poll.$role.tsv"
    printf 'utc\tuptime_ms\tsink\tcorked\tmute\tvolume\n' > "$poll"
    _end=$(( $(exp_now_ms) + HOLD * 1000 ))
    while [ "$(exp_now_ms)" -lt "$_end" ]; do
        our_inputs "$tag" | awk -v utc="$(date -u +%H:%M:%S)" -v up="$(exp_now_ms)" '
            $1 == "Sink:" { s = $2 } $1 == "Corked:" { c = $2 } $1 == "Mute:" { m = $2 }
            $1 == "Volume:" { v = $5 }
            END { printf "%s\t%s\t%s\t%s\t%s\t%s\n", utc, up, s, c, m, v }' >> "$poll"
        sleep "$POLL"
    done
    exp_log "role $role: corked samples $(awk -F '\t' 'NR > 1 && $4 == "yes"' "$poll" | wc -l)" \
        "of $(($(wc -l < "$poll") - 1))"
    exp_stop_pids
done
exp_log "write the operator's event times into the dated note next to poll.<role>.tsv"
