#!/usr/bin/env bash
# device-test.sh -- repeatable physical test run for one sonos-lms room.
#
# Runs a fixed set of scenarios against a real Sonos speaker. LMS-side steps are
# driven automatically over the LMS CLI (port 9090); Sonos-app steps are prompted.
# Everything is captured with one clock and bundled into a single tarball:
#
#   steps.log        scenario markers, prompts and your observations
#   journal.log      bridge journal for the room, with the markers interleaved
#   lms-events.log   LMS CLI event stream ("listen 1"): every playlist/pause/play
#   lms-status.log   LMS status every 2 s: mode, track id, position, playlist stamp
#   lms-server.log   LMS server.log (only when LMS_SSH is set)
#   capture.pcap     UPnP (1400) + stream + slimproto (3483) traffic
#
# Usage (as root on the bridge host):
#   scripts/device-test.sh                  # scenarios 1–7
#   SCENARIOS="1 2" scripts/device-test.sh  # a subset
#   LONG_PAUSE=1200 scripts/device-test.sh  # 20-minute pause in scenario 6
#   LMS_SSH=root@192.168.178.23 scripts/device-test.sh   # also tail server.log
#
# Settings (environment):
#   ROOM        Sonos room / systemd instance            (default: Study)
#   LMS         LMS host             (default: config, recent journal, unit --server, full journal)
#   PLAYER      LMS player id (MAC)  (default: looked up as "<ROOM> (Sonos)")
#   TRACK_A     search text, or id:<n> for track A       (default: Just A Little Bit More)
#   TRACK_B     search text, or id:<n> for track B       (default: False Need)
#   SCENARIOS   which scenarios to run                   (default: 1 2 3 4 5 6 7)
#   LONG_PAUSE  seconds paused in scenario 6             (default: 120)
#   LMS_SSH     ssh target for the LMS host; empty skips server.log
#   LMS_LOG     server.log path on the LMS host (default: /var/log/squeezeboxserver/server.log)
#   LMS_DEBUG   1 = raise LMS log categories for the run, restored afterwards (default: 1)
#   NO_PCAP     1 = skip tcpdump
#   AUTO        1 = unattended SOAP commands and measured checks
#   QUICK       1 = shorter manual run (same defaults as AUTO)
#   S2_ROUNDS   app/device pause/resume rounds (default: 3; AUTO/QUICK: 1)

set -uo pipefail

ROOM=${ROOM:-Study}
TRACK_A=${TRACK_A:-Just A Little Bit More}
TRACK_B=${TRACK_B:-False Need}
AUTO=${AUTO:-0}
QUICK=${QUICK:-0}
if [[ $AUTO == 1 || $QUICK == 1 ]]; then
    SCENARIOS=${SCENARIOS:-1 2 5 6 7}
    LONG_PAUSE=${LONG_PAUSE:-30}
    S2_ROUNDS=${S2_ROUNDS:-1}
fi
SCENARIOS=${SCENARIOS:-1 2 3 4 5 6 7}
LONG_PAUSE=${LONG_PAUSE:-120}
S2_ROUNDS=${S2_ROUNDS:-3}
LMS_SSH=${LMS_SSH:-}
LMS_LOG=${LMS_LOG:-/var/log/squeezeboxserver/server.log}
LMS_DEBUG=${LMS_DEBUG:-1}
NO_PCAP=${NO_PCAP:-0}
CLI_PORT=${CLI_PORT:-9090}
SCALE=${SCALE:-1}   # internal: multiplies every wait (used for dry runs)

STAMP=$(date +%Y%m%d-%H%M%S)
OUT=${OUT:-/tmp/sonos-test-$STAMP}
mkdir -p "$OUT"
START_TIME=$(date '+%Y-%m-%d %H:%M:%S')
START_EPOCH=$(date +%s)
JOURNAL_SINCE="@$START_EPOCH"
JOURNAL_CHECKED=0
PIDS=()
DEBUG_RESTORE=()
AUTO_HELPER=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)/device_test_auto.py
AUTO_REASONS=()
AUTO_MEASUREMENTS=()
SUMMARY=()
AUTO_FAILED=0
AUTO_MONITOR_PID=''


say()  { printf '%s\n' "$*" >&2; }
fail() { say "Error: $*"; exit 1; }
have() { command -v "$1" >/dev/null 2>&1; }
now()  { date '+%H:%M:%S.%3N'; }

# ---------------------------------------------------------------- LMS CLI ---

urlenc() {
    local s=$1 out='' c i
    for (( i = 0; i < ${#s}; i++ )); do
        c=${s:i:1}
        case $c in [a-zA-Z0-9._~-]) out+=$c ;; *) out+=$(printf '%%%02X' "'$c") ;; esac
    done
    printf '%s' "$out"
}
urldec() { local s=${1//+/ }; printf '%b' "${s//%/\\x}"; }

# One request, one reply line, raw (still URL-encoded).
cli_raw() {
    local fd reply=''
    # Braces scope the 2>/dev/null; a bare "exec ... 2>" would silence the whole script.
    { exec {fd}<>"/dev/tcp/$LMS/$CLI_PORT"; } 2>/dev/null || return 1
    printf '%s\n' "$1" >&"$fd"
    IFS= read -r -t 5 reply <&"$fd"
    exec {fd}>&-
    printf '%s' "${reply%$'\r'}"
}

# Value of the first "key:value" token in a raw reply, decoded.
field() {
    local tok
    for tok in $1; do
        tok=$(urldec "$tok")
        [[ $tok == "$2":* ]] && { printf '%s' "${tok#"$2":}"; return 0; }
    done
    return 1
}

lms() {
    if cli_raw "$PLAYER $*" >/dev/null; then return 0; fi
    [[ $AUTO != 1 ]] || auto_fail "LMS command failed: $*"
    return 1
}

status_line() {
    local r
    r=$(cli_raw "$PLAYER status - 1 tags:a") || { printf 'status: no reply'; return; }
    printf 'mode=%s id=%s time=%s duration=%s index=%s playlist_timestamp=%s tracks=%s title=%s' \
        "$(field "$r" mode)" "$(field "$r" id)" "$(field "$r" time)" "$(field "$r" duration)" \
        "$(field "$r" playlist_cur_index)" "$(field "$r" playlist_timestamp)" \
        "$(field "$r" playlist_tracks)" "$(field "$r" title)"
}

# "id:<n>" is used as-is; anything else is a title search, first hit wins.
resolve_track() {
    local spec=$1 r id
    if [[ $spec == id:* ]]; then printf '%s' "${spec#id:}"; return 0; fi
    r=$(cli_raw "titles 0 5 search:$(urlenc "$spec") tags:a") || return 1
    id=$(field "$r" id) || return 1
    say "  '$spec' -> id $id: $(field "$r" title) / $(field "$r" artist)"
    printf '%s' "$(field "$r" artist) - $(field "$r" title)" > "$OUT/.name_$id"
    printf '%s' "$id"
}

resolve_player() {
    local r tok id='' want="$ROOM (Sonos)"
    r=$(cli_raw "players 0 100") || return 1
    for tok in $r; do
        tok=$(urldec "$tok")
        case $tok in
            playerid:*) id=${tok#playerid:} ;;
            name:*) [[ ${tok#name:} == "$want" ]] && { printf '%s' "$id"; return 0; } ;;
        esac
    done
    return 1
}

# --------------------------------------------------------------- markers ---

mark() {
    local line
    local failure='^(SETUP FAILED:|S[1-7]([.][0-9]+|[ab])? (CHECK MISMATCH:|POSITION RESTARTED:|ABORTED:|INVALID:|RESUME FAIL:|AUTO-START FAIL:|PAUSE NOT CONFIRMED:|NOT PLAYING before))'
    if [[ $AUTO == 1 && $* =~ $failure ]]; then
        auto_fail "$*"
    fi
    line="[$(now)] === $*"
    printf '%s\n' "$line" | tee -a "$OUT/steps.log" >&2
    have logger && logger -t sonos-test "=== $*"
}

wait_s() {
    local s
    s=$(awk -v a="$1" -v b="$SCALE" 'BEGIN { printf "%.2f", a * b }')
    sleep "$s"
}

# Ask the tester to do something in the Sonos app; mark the moment they confirm.
prompt() {
    if [[ $AUTO == 1 ]]; then
        local action result
        case "$*" in *'press PAUSE'*) action=Pause ;; *'press PLAY'*) action=Play ;;
            *) auto_fail "unsupported AUTO prompt: $*"; return 1 ;; esac
        if result=$(python3 "$AUTO_HELPER" command --host "$COORDINATOR_IP" --action "$action" 2>&1); then
            mark "AUTO device ${action^^} sent"
            [[ $action != Play ]] || auto_progress
        else
            auto_fail "device $action failed: $result"
        fi
        return 0
    fi
    say ""
    say ">>> $*"
    mark "PROMPT shown: $*"
    read -r -p "    press Enter right after you did it " _ || true
    mark "SONOS APP confirmed (Enter): $*"
}

# Record what the tester heard.
observe() {
    if [[ $AUTO == 1 ]]; then
        # Track-change scenarios have no Play prompt: check their settled playback here.
        case "$*" in S3:*|S4:*|S5:*|S5b*) auto_progress ;; esac
        return 0
    fi
    local answer
    say ""
    read -r -p "??? $* " answer || true
    printf '[%s] OBSERVED: %s -> %s\n' "$(now)" "$*" "${answer:-<no answer>}" | tee -a "$OUT/steps.log" >&2
    have logger && logger -t sonos-test "OBSERVED: $* -> ${answer:-<no answer>}"
}

# Physical Sonos transport state (PLAYING, PAUSED_PLAYBACK, STOPPED, ...), taken
# from the bridge's own status table in the journal. Empty if unavailable.
sonos_state() {
    if [[ $AUTO == 1 ]]; then
        python3 "$AUTO_HELPER" state --host "$COORDINATOR_IP" 2>/dev/null
        return
    fi
    have journalctl || return 0
    journalctl -u "$UNIT" -n 300 -o cat --no-pager 2>/dev/null \
        | awk -F'|' 'NF >= 6 && $2 !~ /Title/ { st = $3 } END { gsub(/ /, "", st); print st }'
}

# Which song the speaker is playing, from the bridge journal:
#   "Creating new stream (N)" + "PlaySqueezeBox: title='...'" map stream N to a song;
#   "speaker URI: stream=N session=TOKEN" reports the actual transport URI.
# Some systemd versions combine --grep and -n incorrectly. Read the run's
# complete journal first, then filter locally; retain session reset records.
journal_plain() {
    journalctl -u "$UNIT" --since "$JOURNAL_SINCE" -o cat --no-pager
}
journal_tail() {
    journal_plain | grep -E '^(Creating new stream|PlaySqueezeBox: title=|speaker URI:|Stream session:)'
}
journal_self_check() {
    [[ $JOURNAL_CHECKED == 0 ]] || return 0
    local plain filtered attempt
    for attempt in 1 2 3; do
        plain=$(journal_plain | sed -n '/^Creating new stream (/p' | tail -n1)
        filtered=$(journal_tail | sed -n '/^Creating new stream (/p' | tail -n1)
        if [[ -n $plain && $plain == "$filtered" ]]; then
            JOURNAL_CHECKED=1
            mark "journal reader self-check OK: $plain"
            return 0
        fi
        sleep .2
    done
    local reason="Journal reader self-check failed: plain journalctl newest '${plain:-missing}', journal_tail newest '${filtered:-missing}'; aborting run"
    if [[ $AUTO == 1 ]]; then auto_fail "$reason"; auto_record "${s:-setup}"; fi
    fail "$reason"
}
bridge_stream() { journal_tail | sed -n 's/^Creating new stream (\([0-9]*\)).*/\1/p' | tail -n1; }
speaker_stream() {
    journal_tail | awk '
        /^Stream session: / { session = $3; stream = "" }
        /^speaker URI: stream=[0-9]+ session=/ {
            split($3, id, "="); split($4, token, "=")
            stream = (session == "" || session == token[2]) ? id[2] : ""
        }
        /^speaker URI: other / { stream = "" }
        END { print stream }'
}
stream_song() {
    [[ -n $1 ]] || return 0
    journal_tail | awk -v n="$1" '
        /^Creating new stream \(/ { cur = $0; sub(/^Creating new stream \(/, "", cur); sub(/\).*/, "", cur) }
        /^PlaySqueezeBox: title=/ && cur == n { t = $0; sub(/^PlaySqueezeBox: title=\047/, "", t); sub(/\047 art=.*/, "", t); song = t }
        END { print song }'
}
# One line: what LMS, the bridge and the speaker each think is playing.
now_playing() {
    local bs ss
    bs=$(bridge_stream); ss=$(speaker_stream)
    printf 'LMS: "%s" | bridge newest stream %s: "%s" | speaker on stream %s: "%s"' \
        "$(field "$(cli_raw "$PLAYER status - 1 tags:a")" title 2>/dev/null)" \
        "${bs:-?}" "$(stream_song "$bs")" "${ss:-?}" "$(stream_song "$ss")"
}
# 0 when the speaker plays the bridge's newest stream AND that stream is the
# song LMS reports as current. Empty journal data counts as "cannot check".
speaker_on_current_song() {
    local bs ss lms_title
    bs=$(bridge_stream); ss=$(speaker_stream)
    [[ -z $bs || -z $ss ]] && { [[ $AUTO != 1 ]]; return; }
    lms_title=$(field "$(cli_raw "$PLAYER status - 1 tags:a")" title 2>/dev/null)
    [[ $ss == "$bs" && ( ( -z $lms_title && $AUTO != 1 ) || ( -n $lms_title && $(stream_song "$ss") == "$lms_title" ) ) ]]
}
# Automatic check after a track change: mark OK or MISMATCH with song names.
check_song() {
    local label=$1 i
    for (( i = 0; i < 10; i++ )); do speaker_on_current_song && break; sleep 1; done
    if speaker_on_current_song; then
        mark "$label CHECK OK: $(now_playing)"
    else
        mark "$label CHECK MISMATCH: $(now_playing)"
    fi
}

lms_mode() { local r; r=$(cli_raw "$PLAYER mode ?"); printf '%s' "${r##* }"; }
lms_time() { field "$(cli_raw "$PLAYER status - 1 tags:a")" time 2>/dev/null || echo 0; }

# position_check <label> <LMS time while paused>: after a resume the LMS
# position must continue from the paused position, not restart near 0.
# The LMS time is what the bridge reports from Sonos's own play position.
position_check() {
    local now verdict
    now=$(lms_time)
    verdict=$(awk -v p="${2:-0}" -v n="${now:-0}" 'BEGIN {
        if (n + 1 < p) print "RESTARTED"; else print "CONTINUED" }')
    mark "$1 POSITION $verdict: paused at ${2%.*}s, now ${now%.*}s"
}

# Wait up to $2 seconds until the speaker reports state $1.
# Speaker states that count as "paused": the bridge may answer a pause with a
# UPnP Stop (SONOS_LMS_PAUSE=stop), so a paused speaker can be STOPPED.
PAUSED_STATES='PAUSED_PLAYBACK|STOPPED'

# wait_sonos <STATE or STATE|STATE...> <seconds>
wait_sonos() {
    local want=$1 i st=''
    for (( i = 0; i < $2 * 2; i++ )); do
        st=$(sonos_state)
        [[ $st =~ ^($want)$ || ( -z $st && $AUTO != 1 ) ]] && return 0   # empty = cannot check
        sleep 0.5
    done
    say "    speaker state is '$st', expected '$want'"
    [[ $AUTO != 1 ]] || auto_fail "speaker state ${st:-unknown}, expected $want"
    return 1
}

# AUTO needs a confirmed state; bound even a blocked SOAP read by the deadline.
# Keep wait_sonos's manual-mode handling of unavailable state unchanged.
auto_confirm_pause() {
    local label=$1 st='' started elapsed remaining reason
    started=$(( $(date +%s%N) / 1000000 ))
    while :; do
        elapsed=$(( $(date +%s%N) / 1000000 - started ))
        (( elapsed < 5000 )) || break
        remaining=$(awk -v ms="$((5000 - elapsed))" 'BEGIN { printf "%.3f", ms / 1000 }')
        st=$(
            export AUTO AUTO_HELPER COORDINATOR_IP
            export -f sonos_state
            timeout "$remaining" bash -c sonos_state
        ) || st=''
        elapsed=$(( $(date +%s%N) / 1000000 - started ))
        if [[ -n $st && $st =~ ^($PAUSED_STATES)$ ]]; then
            elapsed=$(awk -v ms="$elapsed" 'BEGIN { printf "%.1f", ms / 1000 }')
            mark "$label PAUSE OK: speaker $st after ${elapsed}s, lms_mode=$(lms_mode)"
            return 0
        fi
        (( elapsed < 5000 )) || break
        remaining=$(awk -v ms="$((5000 - elapsed))" 'BEGIN { printf "%.3f", ms < 500 ? ms / 1000 : 0.5 }')
        sleep "$remaining"
    done
    reason="$label PAUSE FAIL: speaker ${st:-state unknown} after 5s, lms_mode=$(lms_mode)"
    mark "$reason"
    auto_fail "$reason"
    return 1
}

snapshot() {
    printf '[%s] LMS %s\n' "$(now)" "$(status_line)" | tee -a "$OUT/steps.log" >&2
    printf '[%s]   now playing -> %s\n' "$(now)" "$(now_playing)" | tee -a "$OUT/steps.log" >&2
}

# ------------------------------------------------------------- capturing ---

start_capture() {
    ( exec {fd}<>"/dev/tcp/$LMS/$CLI_PORT" || exit 1
      printf 'listen 1\n' >&"$fd"
      while IFS= read -r line <&"$fd"; do
          printf '%s %s\n' "$(now)" "$(urldec "${line%$'\r'}")"
      done ) > "$OUT/lms-events.log" 2>&1 &
    PIDS+=($!)

    ( while :; do printf '%s %s\n' "$(now)" "$(status_line)"; sleep 2; done ) \
        > "$OUT/lms-status.log" 2>&1 &
    PIDS+=($!)

    if [[ $NO_PCAP != 1 ]]; then
        if have tcpdump; then
            tcpdump -i any -s 0 -U -w "$OUT/capture.pcap" 'port 1400 or port 3483' \
                2> "$OUT/tcpdump.err" &
            PIDS+=($!)
        else
            say "  tcpdump not installed: no packet capture (apt install tcpdump)"
        fi
    fi

    if [[ -n $LMS_SSH ]]; then
        ssh -o BatchMode=yes -o ConnectTimeout=5 "$LMS_SSH" "tail -n0 -F '$LMS_LOG'" \
            > "$OUT/lms-server.log" 2> "$OUT/lms-ssh.err" &
        PIDS+=($!)
    fi

    if [[ $LMS_DEBUG == 1 ]]; then
        local cat old
        for cat in network.protocol.slimproto player.source player.streaming player.playlist; do
            # Reply: "debug <category> <LEVEL>"
            old=$(cli_raw "debug $cat ?" | awk '{print $3}')
            [[ -n $old && $old != '?' ]] && DEBUG_RESTORE+=("$cat $old")
            cli_raw "debug $cat INFO" >/dev/null
        done
    fi
    sleep 1
}

finish() {
    local entry pid
    trap - EXIT INT TERM
    if [[ -n $AUTO_MONITOR_PID ]]; then kill "$AUTO_MONITOR_PID" 2>/dev/null; wait "$AUTO_MONITOR_PID" 2>/dev/null; fi
    say ""
    say "Collecting results ..."
    for entry in "${DEBUG_RESTORE[@]}"; do cli_raw "debug $entry" >/dev/null; done
    for pid in "${PIDS[@]}"; do kill "$pid" 2>/dev/null; done
    wait 2>/dev/null
    if have journalctl; then
        journalctl -o short-precise --since "$START_TIME" \
            _SYSTEMD_UNIT="$UNIT" + SYSLOG_IDENTIFIER=sonos-test > "$OUT/journal.log" 2>&1
    fi
    { echo "room=$ROOM unit=$UNIT player=$PLAYER lms=$LMS"
      echo "track_a=$TRACK_A_ID track_b=$TRACK_B_ID scenarios=$SCENARIOS long_pause=$LONG_PAUSE"
      echo "started=$START_TIME finished=$(date '+%Y-%m-%d %H:%M:%S')"
      [[ -d /opt/sonos-lms/.git ]] && echo "bridge=$(git -C /opt/sonos-lms rev-parse --short HEAD)"
    } > "$OUT/run-info.txt"
    tar -czf "$OUT.tar.gz" -C "$(dirname "$OUT")" "$(basename "$OUT")"
    say ""
    say "Done. Send this file:"
    say "  $OUT.tar.gz"
    [[ $AUTO != 1 ]] || auto_summary
}

# ------------------------------------------------------------- scenarios ---

play_track() { lms playlistcontrol cmd:load "track_id:$1"; }

# Scenario precondition: track A freshly started by LMS and audibly playing.
# Each scenario sets up its own start state instead of inheriting the last one.
setup_playing_a() {
    local t1 t2 i
    mark "setup: LMS loads track A"
    play_track "$TRACK_A_ID"
    for (( i = 0; i < 20; i++ )); do
        [[ $(lms_mode) == play ]] && break
        sleep 0.5
    done
    wait_s 8
    journal_self_check
    # Preconditions: speaker really PLAYING, LMS position really advancing.
    # A stalled start (speaker kept a connection that gets no audio) shows
    # LMS mode=play with a position cycling near zero.
    wait_sonos PLAYING 7
    local sonos_ok=$?
    t1=$(lms_time); sleep 2; t2=$(lms_time)
    snapshot
    local song_ok=0
    speaker_on_current_song || song_ok=1
    if (( sonos_ok != 0 || song_ok != 0 )) || ! awk -v a="$t1" -v b="$t2" 'BEGIN { exit !(b >= a + 1) }'; then
        mark "SETUP FAILED: speaker=$(sonos_state) lms_mode=$(lms_mode) position ${t1}s -> ${t2}s; $(now_playing); scenario skipped"
        observe "setup failed: what does the speaker do? (playing/stopped/silent/error dialog)"
        return 1
    fi
    mark "setup OK: speaker PLAYING, LMS position ${t1}s -> ${t2}s; $(now_playing)"
}

scenario_1() {
    mark "S1 BASELINE: LMS starts track A, LMS pause/resume (expected clean)"
    play_track "$TRACK_A_ID"; wait_s 20; journal_self_check; snapshot
    local tp
    mark "S1 LMS pause";  lms pause 1; wait_s 5; snapshot
    if [[ $AUTO == 1 ]]; then
        wait_sonos "$PAUSED_STATES" 10
        [[ $(lms_mode) == pause ]] || auto_fail "S1 LMS did not pause"
    fi
    tp=$(lms_time)
    mark "S1 LMS resume"; lms pause 0; [[ $AUTO != 1 ]] || auto_progress; wait_s 10; snapshot
    position_check "S1" "$tp"
    observe "S1: did it resume where it paused? (c=continued, r=restarted, s=silent, other)"
}

scenario_2() {
    mark "S2 SONOS APP pause/resume x$S2_ROUNDS on track A"
    setup_playing_a || return 0
    local i st tp
    for (( i = 1; i <= S2_ROUNDS; i++ )); do
        # Precondition: the speaker really plays the current song. A failed
        # previous resume must not turn into "pause the silence".
        if ! wait_sonos PLAYING 5 || ! speaker_on_current_song; then
            mark "S2.$i NOT PLAYING before pause: speaker=$(sonos_state); $(now_playing)"
            observe "S2.$i: before this round nothing plays - what do the speaker and app show?"
            setup_playing_a || { mark "S2 ABORTED: could not restart playback"; return 0; }
        fi
        prompt "S2.$i press PAUSE in the Sonos app"
        if [[ $AUTO == 1 ]]; then
            auto_confirm_pause "S2.$i" || true
        elif wait_sonos "$PAUSED_STATES" 10; then
            mark "S2.$i PAUSE OK: speaker $(sonos_state), lms_mode=$(lms_mode)"
        else
            mark "S2.$i PAUSE NOT CONFIRMED: speaker=$(sonos_state), lms_mode=$(lms_mode)"
        fi
        wait_s 3; snapshot
        tp=$(lms_time)
        prompt "S2.$i press PLAY in the Sonos app"
        if wait_sonos PLAYING 10 && speaker_on_current_song; then
            mark "S2.$i RESUME OK: $(now_playing)"
        else
            st=$(sonos_state)
            mark "S2.$i RESUME FAIL: speaker=${st:-unknown}, lms_mode=$(lms_mode); $(now_playing)"
        fi
        wait_s 5; snapshot
        position_check "S2.$i" "$tp"
        observe "S2.$i: error dialog in the app (e), or none (n)?"
    done
}

scenario_3() {
    mark "S3 LMS track change while playing: A -> B"
    setup_playing_a || return 0
    mark "S3 LMS loads track B"
    play_track "$TRACK_B_ID"; wait_s 15; snapshot
    check_song "S3"
    observe "S3: do you hear $TRACK_B_NAME? (y/n/other)"
}

scenario_4() {
    mark "S4 LMS track change while paused (LMS pause, then LMS loads B)"
    setup_playing_a || return 0
    mark "S4 LMS pause"
    lms pause 1; wait_s 8; snapshot
    mark "S4 LMS loads track B while paused"
    play_track "$TRACK_B_ID"; wait_s 15; snapshot
    check_song "S4"
    observe "S4: do you hear $TRACK_B_NAME? (y/n/other)"
}

scenario_5() {
    mark "S5 Sonos-app pause, then LMS track change, then check"
    setup_playing_a || return 0
    prompt "S5 press PAUSE in the Sonos app"; wait_s 8; snapshot
    local previous_stream
    previous_stream=$(speaker_stream)
    [[ $AUTO != 1 ]] || wait_sonos "$PAUSED_STATES" 10
    mark "S5 LMS loads track B while Sonos-paused"
    play_track "$TRACK_B_ID"; wait_s 15; snapshot
    check_song "S5"
    if [[ $AUTO == 1 ]]; then
        local actual
        [[ -n $(speaker_stream) && $(speaker_stream) != "$previous_stream" ]] || auto_fail "S5 speaker did not switch to a new stream"
        actual=$(field "$(cli_raw "$PLAYER status - 1 tags:a")" title 2>/dev/null)
        [[ -n $TRACK_B_TITLE && $actual == "$TRACK_B_TITLE" ]] || auto_fail "S5 LMS title '$actual', expected track B '$TRACK_B_TITLE'"
    fi
    observe "S5: do you hear $TRACK_B_NAME? (y = yes, n = nothing, a = $TRACK_A_NAME came back, other)"
    # S5a: after a Sonos-app pause, loading a new track in LMS must start it by
    # itself. Decide from the data (speaker PLAYING, on the bridge's newest
    # stream, which is the song LMS reports); ask the tester only if unsure.
    local st auto=0
    st=$(sonos_state)
    if [[ $st == PLAYING ]] && speaker_on_current_song; then
        auto=1
    elif [[ -z $st && $AUTO != 1 ]]; then
        local playing
        say ""
        read -r -p "??? S5: is the speaker playing right now? (y/n) " playing || true
        [[ ${playing,,} == y* ]] && auto=1
    fi
    if (( auto )); then
        mark "S5a AUTO-START PASS: $(now_playing); no Sonos-app action needed"
    else
        mark "S5a AUTO-START FAIL: speaker=${st:-unknown}; $(now_playing)"
        prompt "S5b press PLAY in the Sonos app (recovery test)"
        wait_s 10; snapshot
        mark "S5b after PLAY: speaker=$(sonos_state); $(now_playing)"
        observe "S5b after PLAY: which song plays? (a = $TRACK_A_NAME, b = $TRACK_B_NAME, none, error dialog)"
    fi
}

scenario_6() {
    mark "S6 long pause from the Sonos app (${LONG_PAUSE}s), then Sonos-app play"
    setup_playing_a || return 0
    prompt "S6 press PAUSE in the Sonos app"
    if [[ $AUTO == 1 ]]; then
        auto_confirm_pause S6 || return 0
    fi
    if { [[ $AUTO != 1 ]] && ! wait_sonos "$PAUSED_STATES" 10; } || [[ $(lms_mode) != pause ]]; then
        mark "S6 ABORTED: pause not confirmed (speaker=$(sonos_state) lms_mode=$(lms_mode))"
        observe "S6 aborted: what does the speaker/app show?"
        return 0
    fi
    mark "S6 pause confirmed: speaker $(sonos_state), LMS pause; countdown starts"
    local left=$LONG_PAUSE st tp
    tp=$(lms_time)
    while (( left > 0 )); do
        printf '\r    paused, %4ds left ' "$left" >&2
        wait_s 10; (( left -= 10 ))
        st=$(sonos_state)
        if [[ -n $st && ! $st =~ ^($PAUSED_STATES)$ ]] || [[ $(lms_mode) != pause ]]; then
            printf '\n' >&2
            mark "S6 INVALID: pause interrupted after $(( LONG_PAUSE - left ))s (speaker=$st lms_mode=$(lms_mode))"
            observe "S6 invalid: did you press anything? what happened?"
            return 0
        fi
        (( left % 60 == 0 )) && snapshot 2>/dev/null
    done
    printf '\n' >&2
    snapshot
    prompt "S6 press PLAY in the Sonos app"; wait_s 15; snapshot
    mark "S6 after PLAY: speaker=$(sonos_state) lms_mode=$(lms_mode)"
    position_check "S6" "$tp"
    observe "S6: error dialog in the app (e), silent (s), or fine (n)?"
}

# LMS-stop/app-Play regression; also selectable alone with SCENARIOS=7.
scenario_7() {
    mark "S7 LMS stop, then Sonos-app play"
    setup_playing_a || return 0
    mark "S7 LMS stop"
    lms stop; wait_s 5
    wait_sonos STOPPED 5
    mark "S7 after stop: speaker=$(sonos_state) (expected STOPPED) lms_mode=$(lms_mode) (expected stop)"
    prompt "S7 press PLAY in the Sonos app"
    wait_s 15; snapshot
    wait_sonos PLAYING 5
    mark "S7 after PLAY: speaker=$(sonos_state) lms_mode=$(lms_mode); $(now_playing)"
    observe "S7: plays (p), silent (s), error dialog (e)?"
}

# --------------------------------------------------------- LMS discovery ---

lms_from_exec_start() {
    local command host pattern='(^|[[:space:]])--server(=|[[:space:]]+)([^[:space:];]+)'
    command=$(cat)
    # Parse systemctl's argv[] text as data; never eval a unit command line.
    [[ $command =~ $pattern ]] || return 1
    host=${BASH_REMATCH[3]}
    host=${host#\"}; host=${host%\"}
    host=${host#\'}; host=${host%\'}
    [[ -n $host && $host != --* ]] || return 1
    printf '%s\n' "$host"
}

lms_from_journal() {
    sed -n 's/.*LMS server from [^:]*: \([^ ]*\).*/\1/p' | tail -n1
}

resolve_lms_host() {
    local config=${1:-/etc/sonos-lms/config}
    LMS_SOURCE='LMS override'
    if [[ -z ${LMS:-} ]]; then
        LMS=$(sed -n 's/^[[:space:]]*LMS_SERVER=[[:space:]]*//p' "$config" 2>/dev/null | head -n1)
        LMS_SOURCE='config LMS_SERVER'
    fi
    if [[ -z ${LMS:-} ]] && have journalctl; then
        LMS=$(journalctl -u "$UNIT" -n 500 --no-pager -o cat 2>/dev/null | lms_from_journal)
        LMS_SOURCE='recent unit journal'
    fi
    if [[ -z ${LMS:-} ]] && have systemctl; then
        LMS=$(systemctl show "$UNIT" -p ExecStart 2>/dev/null | lms_from_exec_start)
        LMS_SOURCE='unit ExecStart --server'
    fi
    if [[ -z ${LMS:-} ]] && have journalctl; then
        LMS=$(journalctl -u "$UNIT" --no-pager -o cat 2>/dev/null | lms_from_journal)
        LMS_SOURCE='full unit journal'
    fi
    [[ -n ${LMS:-} ]] || return 1
    mark "LMS host $LMS (source: $LMS_SOURCE)"
    return 0
}

discover_coordinator() {
    local details
    if ! details=$(SONOS_LMS_UPNP=yeney ./sonos-lms --list-rooms --details); then
        say "yeney discovery failed; retrying with noson"
        details=$(SONOS_LMS_UPNP=noson ./sonos-lms --list-rooms --details) || return 1
    fi
    printf '%s\n' "$details" | python3 "$AUTO_HELPER" coordinator --room "$ROOM"
}

# AUTO failures accumulate; recovery can never erase a failed check.
auto_fail() { AUTO_REASONS+=("$*"); }
auto_progress() {
    local result
    if result=$(python3 "$AUTO_HELPER" progress --host "$COORDINATOR_IP" --lms "$LMS" --port "$CLI_PORT" --player "$PLAYER" 2>&1); then
        AUTO_MEASUREMENTS+=("$result")
        mark "AUTO $result"
    else
        auto_fail "$result"
    fi
}
auto_begin() {
    AUTO_REASONS=()
    AUTO_MEASUREMENTS=()
    AUTO_SINCE=$(date '+%Y-%m-%d %H:%M:%S.%6N')
    AUTO_STATUS_FILE="$OUT/auto-status-$1.log"
    python3 "$AUTO_HELPER" monitor --host "$COORDINATOR_IP" > "$AUTO_STATUS_FILE" 2>&1 &
    AUTO_MONITOR_PID=$!
}
auto_end() {
    local errors
    kill "$AUTO_MONITOR_PID" 2>/dev/null; wait "$AUTO_MONITOR_PID" 2>/dev/null
    AUTO_MONITOR_PID=''
    [[ -s $AUTO_STATUS_FILE ]] || auto_fail "cannot check: empty GetTransportInfo sample log ($AUTO_STATUS_FILE)"
    # Retain the final read too, without hiding a monitor that produced no samples.
    python3 "$AUTO_HELPER" sample --host "$COORDINATOR_IP" >> "$AUTO_STATUS_FILE" 2>&1
    if errors=$(python3 "$AUTO_HELPER" status-log --file "$AUTO_STATUS_FILE" 2>&1); then
        mark "AUTO $errors"
        AUTO_MEASUREMENTS+=("$errors")
    else
        auto_fail "$errors"
    fi
    if errors=$(journalctl -u "$UNIT" --since "$AUTO_SINCE" -o cat --no-pager 2>&1); then
        errors=$(printf '%s\n' "$errors" | grep -E 'ERROR_[A-Z_]+' || true)
        [[ -z $errors ]] || auto_fail "$errors"
    else
        auto_fail "scenario journal unavailable: $errors"
    fi
    auto_record "$1"
}
auto_record() {
    local reason
    if (( ${#AUTO_REASONS[@]} )); then
        reason=$(IFS=';'; printf '%s' "${AUTO_REASONS[*]}")
        reason=${reason//$'\n'/; }
        SUMMARY+=("S$1 | FAIL | $reason")
        AUTO_FAILED=1
    else
        SUMMARY+=("S$1 | PASS | progress, position/song and transport checks passed${AUTO_MEASUREMENTS[*]:+; ${AUTO_MEASUREMENTS[*]}}")
    fi
}
auto_summary() {
    printf 'scenario | PASS/FAIL | reason\n'
    printf '%s\n' "${SUMMARY[@]}"
}

# ------------------------------------------------------------------ main ---

[[ $EUID -eq 0 ]] || fail "run as root (tcpdump and journal access)"
have systemd-escape && UNIT=$(systemd-escape --template=sonos-lms@.service -- "$ROOM") \
    || UNIT="sonos-lms@$ROOM.service"

resolve_lms_host || fail "LMS host unknown; run with LMS=<ip>"
cli_raw "version ?" >/dev/null || fail "no LMS CLI at $LMS:$CLI_PORT"

if have systemctl && ! systemctl is-active --quiet "$UNIT"; then
    fail "$UNIT is not running"
fi

PLAYER=${PLAYER:-$(resolve_player)} || true
[[ -n ${PLAYER:-} ]] || fail "no LMS player named '$ROOM (Sonos)'; set PLAYER=<mac>"

say "Room $ROOM ($UNIT), LMS $LMS, player $PLAYER"
say "Tracks:"
TRACK_A_ID=$(resolve_track "$TRACK_A") || fail "track A not found: $TRACK_A"
TRACK_B_ID=$(resolve_track "$TRACK_B") || fail "track B not found: $TRACK_B"
TRACK_A_NAME=$(cat "$OUT/.name_$TRACK_A_ID" 2>/dev/null || echo "track A")
TRACK_B_NAME=$(cat "$OUT/.name_$TRACK_B_ID" 2>/dev/null || echo "track B")
rm -f "$OUT"/.name_*
say "Output: $OUT"
say ""
if [[ $AUTO != 1 ]]; then
say "Scenarios $SCENARIOS. Keep the Sonos app open on room $ROOM."
say "When asked, do the action first, then press Enter immediately."
read -r -p "Press Enter to start " _ || true
else
    have python3 || fail "AUTO requires python3"
    [[ -n ${SCENARIOS// /} && $S2_ROUNDS =~ ^[1-9][0-9]*$ && $LONG_PAUSE =~ ^[0-9]+$ ]] || fail "AUTO needs scenarios, positive S2_ROUNDS and nonnegative LONG_PAUSE"
    COORDINATOR_IP=$(discover_coordinator) || fail "AUTO coordinator discovery failed"
    TRACK_B_TITLE=$(field "$(cli_raw "songinfo 0 100 track_id:$TRACK_B_ID")" title)
    [[ -n $TRACK_B_TITLE ]] || fail "AUTO could not resolve track B title"
    mark "AUTO coordinator $COORDINATOR_IP; scenarios $SCENARIOS"
fi

trap finish EXIT
trap 'exit 130' INT TERM
start_capture
mark "RUN START room=$ROOM player=$PLAYER track_a=$TRACK_A_ID track_b=$TRACK_B_ID"
JOURNAL_SINCE="-5min" snapshot

for s in $SCENARIOS; do
    if [[ $AUTO == 1 ]]; then auto_begin "$s"; fi
    if declare -F "scenario_$s" >/dev/null; then "scenario_$s"; else
        say "unknown scenario $s"
        [[ $AUTO != 1 ]] || auto_fail "unknown scenario $s"
    fi
    if [[ $AUTO == 1 ]]; then auto_end "$s"; fi
done

mark "RUN END (LMS pause)"
lms pause 1

if [[ $AUTO == 1 ]]; then auto_summary > "$OUT/auto-summary.txt"; exit "$AUTO_FAILED"; fi
