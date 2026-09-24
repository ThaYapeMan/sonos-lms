#!/usr/bin/env bash
# reference-test.sh -- how does Sonos pause/resume a live stream that WORKS?
#
# Known-good reference for the device-resume problem. For a few minutes the
# room's bridge is stopped and Sonos plays a real radio station through
# scripts/reference-relay.py on this host, started with exactly the same UPnP
# command and metadata the bridge sends. You pause and resume in the Sonos app;
# the relay logs every request, response and first bytes, tcpdump captures the
# traffic. Afterwards the bridge is started again and everything is bundled.
#
# Usage (as root on the bridge host):
#   scripts/reference-test.sh
#   UPSTREAM=http://some.station/stream.mp3 scripts/reference-test.sh
#
# Settings (environment):
#   UPSTREAM     live station URL, http or https (default: Radio Paradise FLAC, see below)
#   SONOS_IP     speaker to use                     (default: 192.168.178.132, Study)
#   ROOM         bridge room to stop meanwhile      (default: Study)
#   HOST_IP      this host's address as Sonos sees it (default: first of hostname -I)
#   PORT         relay port                          (default: 8990)
#   ROUNDS       pause/resume rounds                 (default: 3)
#   PAUSE_SECS   seconds to stay paused per round    (default: 10)
#   MIME         content type announced to Sonos    (default: the station's own)
#   NO_PCAP      1 = skip tcpdump
#
# Check the Radio Paradise URL on https://radioparadise.com/listen/stream-links;
# any live http(s) station works (an MP3 station is a useful second reference).

set -uo pipefail

UPSTREAM=${UPSTREAM:-http://stream.radioparadise.com/flac}
SONOS_IP=${SONOS_IP:-192.168.178.132}
ROOM=${ROOM:-Study}
PORT=${PORT:-8990}
ROUNDS=${ROUNDS:-3}
PAUSE_SECS=${PAUSE_SECS:-10}
NO_PCAP=${NO_PCAP:-0}
HOST_IP=${HOST_IP:-$(hostname -I 2>/dev/null | awk '{print $1}')}
HERE=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)

STAMP=$(date +%Y%m%d-%H%M%S)
OUT=${OUT:-/tmp/sonos-reference-$STAMP}
mkdir -p "$OUT"
PIDS=()
BRIDGE_WAS_ACTIVE=0

say()  { printf '%s\n' "$*" >&2; }
fail() { say "Error: $*"; exit 1; }
have() { command -v "$1" >/dev/null 2>&1; }
now()  { date '+%H:%M:%S.%3N'; }
mark() { printf '[%s] === %s\n' "$(now)" "$*" | tee -a "$OUT/steps.log" >&2; }
prompt() {
    say ""; say ">>> $*"
    mark "PROMPT shown: $*"
    read -r -p "    press Enter right after you did it " _ || true
    mark "SONOS APP confirmed (Enter): $*"
}
observe() {
    local answer
    say ""; read -r -p "??? $* " answer || true
    printf '[%s] OBSERVED: %s -> %s\n' "$(now)" "$*" "${answer:-<no answer>}" | tee -a "$OUT/steps.log" >&2
}

# ------------------------------------------------------------------ UPnP ---

soap() {  # soap <action> <inner xml>
    curl -s -m 5 "http://$SONOS_IP:1400/MediaRenderer/AVTransport/Control" \
        -H 'Content-Type: text/xml; charset=utf-8' \
        -H "SOAPAction: \"urn:schemas-upnp-org:service:AVTransport:1#$1\"" \
        --data "<?xml version=\"1.0\" encoding=\"utf-8\"?><s:Envelope xmlns:s=\"http://schemas.xmlsoap.org/soap/envelope/\" s:encodingStyle=\"http://schemas.xmlsoap.org/soap/encoding/\"><s:Body><u:$1 xmlns:u=\"urn:schemas-upnp-org:service:AVTransport:1\"><InstanceID>0</InstanceID>$2</u:$1></s:Body></s:Envelope>"
}
# sed, not ${var//x/y}: since bash 5.2 an "&" in that replacement means "the
# matched text", which turned "&lt;" into "<lt;" and made Sonos answer 402.
xml_escape() { printf '%s' "$1" | sed -e 's/&/\&amp;/g' -e 's/</\&lt;/g' -e 's/>/\&gt;/g' -e 's/"/\&quot;/g'; }

# "STATE STATUS", e.g. "PLAYING OK" or "STOPPED ERROR_CORRUPT_FILE"
sonos_state() {
    local r st ss
    r=$(soap GetTransportInfo "") || { printf 'UNREACHABLE -'; return; }
    st=$(sed -n 's:.*<CurrentTransportState>\([^<]*\)<.*:\1:p' <<<"$r")
    ss=$(sed -n 's:.*<CurrentTransportStatus>\([^<]*\)<.*:\1:p' <<<"$r")
    printf '%s %s' "${st:-?}" "${ss:-?}"
}
wait_state() {  # wait_state <STATE> <seconds>
    local i st
    for (( i = 0; i < $2 * 2; i++ )); do
        st=$(sonos_state); [[ ${st%% *} == "$1" ]] && return 0
        sleep 0.5
    done
    return 1
}

play_reference() {
    local url="http://$HOST_IP:$PORT/reference.stream" didl
    didl="<DIDL-Lite xmlns=\"urn:schemas-upnp-org:metadata-1-0/DIDL-Lite/\" xmlns:r=\"urn:schemas-rinconnetworks-com:metadata-1-0/\" xmlns:dc=\"http://purl.org/dc/elements/1.1/\" xmlns:upnp=\"urn:schemas-upnp-org:metadata-1-0/upnp/\" ><item><upnp:class>object.item.audioItem</upnp:class><dc:title>Reference stream</dc:title><r:streamContent></r:streamContent><res protocolInfo=\"x-rincon-mp3radio:*:$MIME:*\">$url</res></item></DIDL-Lite>"
    mark "SetAVTransportURI $url (protocolInfo x-rincon-mp3radio:*:$MIME:*)"
    soap SetAVTransportURI "<CurrentURI>$url</CurrentURI><CurrentURIMetaData>$(xml_escape "$didl")</CurrentURIMetaData>" \
        > "$OUT/setavtransporturi.xml"
    if grep -q '<errorCode>' "$OUT/setavtransporturi.xml"; then
        mark "SetAVTransportURI REJECTED by Sonos: UPnP error $(sed -n 's:.*<errorCode>\([^<]*\)<.*:\1:p' "$OUT/setavtransporturi.xml")"
    fi
    mark "Play"
    soap Play "<Speed>1</Speed>" > "$OUT/play.xml"
}

# --------------------------------------------------------------- cleanup ---

finish() {
    local pid
    trap - EXIT INT TERM
    say ""; say "Restoring ..."
    soap Stop "" >/dev/null 2>&1
    for pid in "${PIDS[@]}"; do kill "$pid" 2>/dev/null; done
    wait 2>/dev/null
    if (( BRIDGE_WAS_ACTIVE )); then
        systemctl start "$UNIT" && mark "bridge $UNIT started again"
    fi
    { echo "upstream=$UPSTREAM mime=$MIME format=$FORMAT"
      echo "sonos=$SONOS_IP host=$HOST_IP port=$PORT rounds=$ROUNDS pause=$PAUSE_SECS"
    } > "$OUT/run-info.txt"
    tar -czf "$OUT.tar.gz" -C "$(dirname "$OUT")" "$(basename "$OUT")"
    say ""; say "Done. Send this file:"; say "  $OUT.tar.gz"
}

# ------------------------------------------------------------------ main ---

[[ $EUID -eq 0 ]] || fail "run as root (stops/starts the bridge, runs tcpdump)"
have curl || fail "curl is required (apt install curl)"
have python3 || fail "python3 is required (apt install python3)"
[[ -n $HOST_IP ]] || fail "cannot determine this host's IP; set HOST_IP="
UNIT=$(systemd-escape --template=sonos-squeezebox@.service -- "$ROOM" 2>/dev/null || echo "sonos-squeezebox@$ROOM.service")

say "Probing $UPSTREAM ..."
probe=$(python3 "$HERE/reference-relay.py" --probe --upstream "$UPSTREAM") || fail "station not reachable: $probe"
MIME=${MIME:-$(sed -n 's/^CONTENT_TYPE=//p' <<<"$probe" | cut -d';' -f1)}
FORMAT=$(sed -n 's/^FORMAT=//p' <<<"$probe")
say "  content type $MIME, format: $FORMAT"
[[ $(sonos_state) != UNREACHABLE* ]] || fail "no UPnP answer from Sonos at $SONOS_IP"

say ""
say "This stops $UNIT for the duration of the test and starts it again afterwards."
say "Keep the Sonos app open on the room that is $SONOS_IP."
read -r -p "Press Enter to start " _ || true

trap finish EXIT
trap 'exit 130' INT TERM
mark "RUN START upstream=$UPSTREAM mime=$MIME format=$FORMAT"
if systemctl is-active --quiet "$UNIT"; then
    BRIDGE_WAS_ACTIVE=1
    systemctl stop "$UNIT" && mark "bridge $UNIT stopped"
fi

python3 -u "$HERE/reference-relay.py" --upstream "$UPSTREAM" --port "$PORT" > "$OUT/relay.log" 2>&1 &
PIDS+=($!)
( prev=''; while :; do st=$(sonos_state); [[ $st != "$prev" ]] && printf '%s %s\n' "$(now)" "$st"; prev=$st; sleep 0.5; done ) \
    > "$OUT/sonos-state.log" 2>&1 &
PIDS+=($!)
if [[ $NO_PCAP != 1 ]] && have tcpdump; then
    tcpdump -i any -s 0 -U -w "$OUT/capture.pcap" "port $PORT or (host $SONOS_IP and port 1400)" \
        2> "$OUT/tcpdump.err" &
    PIDS+=($!)
fi
sleep 1

play_reference
if wait_state PLAYING 20; then
    mark "reference PLAYING ($(sonos_state))"
else
    mark "ABORTED: Sonos never reached PLAYING ($(sonos_state))"
    observe "what does the app show?"
    exit 1
fi
observe "do you hear the station? (y/n)"

for (( r = 1; r <= ROUNDS; r++ )); do
    if ! wait_state PLAYING 5; then
        mark "R$r NOT PLAYING before pause ($(sonos_state)); restarting the reference stream"
        play_reference
        wait_state PLAYING 20 || { mark "R$r ABORTED: cannot restart ($(sonos_state))"; break; }
    fi
    prompt "R$r press PAUSE in the Sonos app"
    if wait_state PAUSED_PLAYBACK 10; then mark "R$r PAUSE OK ($(sonos_state))"
    else mark "R$r PAUSE NOT CONFIRMED ($(sonos_state))"; fi
    sleep "$PAUSE_SECS"
    prompt "R$r press PLAY in the Sonos app"
    if wait_state PLAYING 15; then mark "R$r RESUME OK ($(sonos_state))"
    else mark "R$r RESUME FAIL ($(sonos_state))"; fi
    sleep 5
    observe "R$r: continued (c), restarted/jumped (j), silent (s), error dialog (e)?"
done
mark "RUN END"
