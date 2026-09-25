#!/usr/bin/env bash
# device-test.sh -- repeatable physical test run for one sonos-squeezebox room.
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
#   scripts/device-test.sh                  # all scenarios
#   SCENARIOS="1 2" scripts/device-test.sh  # a subset
#   LONG_PAUSE=1200 scripts/device-test.sh  # 20-minute pause in scenario 6
#   LMS_SSH=root@192.168.178.23 scripts/device-test.sh   # also tail server.log
#
# Settings (environment):
#   ROOM        Sonos room / systemd instance            (default: Study)
#   LMS         LMS host             (default: LMS_SERVER from config, else discovery log)
#   PLAYER      LMS player id (MAC)  (default: looked up as "<ROOM> (Sonos)")
#   TRACK_A     search text, or id:<n> for track A       (default: Just A Little Bit More)
#   TRACK_B     search text, or id:<n> for track B       (default: False Need)
#   SCENARIOS   which scenarios to run                   (default: 1 2 3 4 5 6)
#   LONG_PAUSE  seconds paused in scenario 6             (default: 120)
#   LMS_SSH     ssh target for the LMS host; empty skips server.log
#   LMS_LOG     server.log path on the LMS host (default: /var/log/squeezeboxserver/server.log)
#   LMS_DEBUG   1 = raise LMS log categories for the run, restored afterwards (default: 1)
#   NO_PCAP     1 = skip tcpdump

set -uo pipefail

ROOM=${ROOM:-Study}
TRACK_A=${TRACK_A:-Just A Little Bit More}
TRACK_B=${TRACK_B:-False Need}
SCENARIOS=${SCENARIOS:-1 2 3 4 5 6}
LONG_PAUSE=${LONG_PAUSE:-120}
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
PIDS=()
DEBUG_RESTORE=()

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

lms() { cli_raw "$PLAYER $*" >/dev/null; }

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
    say ""
    say ">>> $*"
    mark "PROMPT shown: $*"
    read -r -p "    press Enter right after you did it " _ || true
    mark "SONOS APP confirmed (Enter): $*"
}

# Record what the tester heard.
observe() {
    local answer
    say ""
    read -r -p "??? $* " answer || true
    printf '[%s] OBSERVED: %s -> %s\n' "$(now)" "$*" "${answer:-<no answer>}" | tee -a "$OUT/steps.log" >&2
    have logger && logger -t sonos-test "OBSERVED: $* -> ${answer:-<no answer>}"
}

# Physical Sonos transport state (PLAYING, PAUSED_PLAYBACK, STOPPED, ...), taken
# from the bridge's own status table in the journal. Empty if unavailable.
sonos_state() {
    have journalctl || return 0
    journalctl -u "$UNIT" -n 300 -o cat --no-pager 2>/dev/null \
        | awk -F'|' 'NF >= 6 && $2 !~ /Title/ { st = $3 } END { gsub(/ /, "", st); print st }'
}

# Which song the speaker is playing, from the bridge journal:
#   "Creating new stream (N)" + "PlaySqueezeBox: title='...'" map stream N to a song;
#   the status table's "| Title  squeezebox.flac?session=TOKEN&stream=N |" is what the speaker plays.
journal_tail() { have journalctl && journalctl -u "$UNIT" -n 600 -o cat --no-pager 2>/dev/null; }
bridge_stream() { journal_tail | sed -n 's/^Creating new stream (\([0-9]*\)).*/\1/p' | tail -n1; }
speaker_stream() { journal_tail | sed -nE 's/^\| Title  *squeezebox\.flac\?([^ ]*&)?stream=([0-9]+).*/\2/p' | tail -n1; }
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
    [[ -z $bs || -z $ss ]] && return 0
    lms_title=$(field "$(cli_raw "$PLAYER status - 1 tags:a")" title 2>/dev/null)
    [[ $ss == "$bs" && ( -z $lms_title || $(stream_song "$ss") == "$lms_title" ) ]]
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
# UPnP Stop (SONOS_SQUEEZEBOX_PAUSE=stop), so a paused speaker can be STOPPED.
PAUSED_STATES='PAUSED_PLAYBACK|STOPPED'

# wait_sonos <STATE or STATE|STATE...> <seconds>
wait_sonos() {
    local want=$1 i st=''
    for (( i = 0; i < $2 * 2; i++ )); do
        st=$(sonos_state)
        [[ -z $st || $st =~ ^($want)$ ]] && return 0   # empty = cannot check
        sleep 0.5
    done
    say "    speaker state is '$st', expected '$want'"
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
      [[ -d /opt/sonos-squeezebox/.git ]] && echo "bridge=$(git -C /opt/sonos-squeezebox rev-parse --short HEAD)"
    } > "$OUT/run-info.txt"
    tar -czf "$OUT.tar.gz" -C "$(dirname "$OUT")" "$(basename "$OUT")"
    say ""
    say "Done. Send this file:"
    say "  $OUT.tar.gz"
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
    play_track "$TRACK_A_ID"; wait_s 20; snapshot
    local tp
    mark "S1 LMS pause";  lms pause 1; wait_s 5; snapshot
    tp=$(lms_time)
    mark "S1 LMS resume"; lms pause 0; wait_s 10; snapshot
    position_check "S1" "$tp"
    observe "S1: did it resume where it paused? (c=continued, r=restarted, s=silent, other)"
}

scenario_2() {
    mark "S2 SONOS APP pause/resume x3 on track A"
    setup_playing_a || return 0
    local i st tp
    for i in 1 2 3; do
        # Precondition: the speaker really plays the current song. A failed
        # previous resume must not turn into "pause the silence".
        if ! wait_sonos PLAYING 5 || ! speaker_on_current_song; then
            mark "S2.$i NOT PLAYING before pause: speaker=$(sonos_state); $(now_playing)"
            observe "S2.$i: before this round nothing plays - what do the speaker and app show?"
            setup_playing_a || { mark "S2 ABORTED: could not restart playback"; return 0; }
        fi
        prompt "S2.$i press PAUSE in the Sonos app"
        if wait_sonos "$PAUSED_STATES" 10; then
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
    mark "S5 LMS loads track B while Sonos-paused"
    play_track "$TRACK_B_ID"; wait_s 15; snapshot
    check_song "S5"
    observe "S5: do you hear $TRACK_B_NAME? (y = yes, n = nothing, a = $TRACK_A_NAME came back, other)"
    # S5a: after a Sonos-app pause, loading a new track in LMS must start it by
    # itself. Decide from the data (speaker PLAYING, on the bridge's newest
    # stream, which is the song LMS reports); ask the tester only if unsure.
    local st auto=0
    st=$(sonos_state)
    if [[ $st == PLAYING ]] && speaker_on_current_song; then
        auto=1
    elif [[ -z $st ]]; then
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
    if ! wait_sonos "$PAUSED_STATES" 10 || [[ $(lms_mode) != pause ]]; then
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

# Opt in with SCENARIOS=7 until physically verified.
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

# ------------------------------------------------------------------ main ---

[[ $EUID -eq 0 ]] || fail "run as root (tcpdump and journal access)"
have systemd-escape && UNIT=$(systemd-escape --template=sonos-squeezebox@.service -- "$ROOM") \
    || UNIT="sonos-squeezebox@$ROOM.service"

if [[ -z ${LMS:-} ]]; then
    LMS=$(sed -n 's/^[[:space:]]*LMS_SERVER=[[:space:]]*//p' /etc/sonos-squeezebox/config 2>/dev/null | head -n1)
fi
if [[ -z ${LMS:-} ]] && have journalctl; then
    LMS=$(journalctl -u "$UNIT" -n 500 --no-pager 2>/dev/null \
        | sed -n 's/.*LMS server from [^:]*: \([^ ]*\).*/\1/p' | tail -n1)
fi
[[ -n ${LMS:-} ]] || fail "LMS host unknown; run with LMS=<ip>"
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
say "Scenarios $SCENARIOS. Keep the Sonos app open on room $ROOM."
say "When asked, do the action first, then press Enter immediately."
read -r -p "Press Enter to start " _ || true

trap finish EXIT
trap 'exit 130' INT TERM
start_capture
mark "RUN START room=$ROOM player=$PLAYER track_a=$TRACK_A_ID track_b=$TRACK_B_ID"
snapshot

for s in $SCENARIOS; do
    if declare -F "scenario_$s" >/dev/null; then "scenario_$s"; else say "unknown scenario $s"; fi
done

mark "RUN END (LMS pause)"
lms pause 1
