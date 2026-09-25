#!/usr/bin/env bash
set -euo pipefail

fail() { echo "Error: $*" >&2; exit 1; }
[[ $EUID -eq 0 ]] || fail "Run as root."
cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.."
[[ -f ./sonos-lms ]] || fail "Missing ./sonos-lms; run make first."

server_set=false
server=''
rooms=()
for arg in "$@"; do
    case "$arg" in
        --server=*) server=${arg#--server=}; server_set=true ;;
        --*) fail "Usage: scripts/install-devices.sh [--server=<lms-host-or-ip>] [room ...]" ;;
        *) [[ -n $arg ]] || fail "Room names must not be empty."
           [[ $arg != *$'\n'* && $arg != *$'\r'* ]] || fail "Room names must not contain newlines."
           rooms+=("$arg") ;;
    esac
done
[[ $server != *$'\n'* && $server != *$'\r'* ]] || fail "Server must not contain newlines."
config_dir=/etc/sonos-lms
rooms_file=$config_dir/rooms
if [[ ${#rooms[@]} -eq 0 && ! -s $rooms_file ]]; then
    fail "No rooms configured; pass one or more quoted room names."
fi
mkdir -p -- "$config_dir"
tmp=$(mktemp "$config_dir/.install.XXXXXX")
trap 'rm -f -- "$tmp"' EXIT
if $server_set; then
    # ENVIRON preserves the value literally, including awk-sensitive backslashes.
    config_input=/dev/null
    if [[ -e $config_dir/config ]]; then config_input=$config_dir/config; fi
    LMS_SERVER_VALUE=$server awk '
        /^[[:space:]]*LMS_SERVER=/ {
            if (!written++) print "LMS_SERVER=" ENVIRON["LMS_SERVER_VALUE"]
            next
        }
        { print }
        END { if (!written) print "LMS_SERVER=" ENVIRON["LMS_SERVER_VALUE"] }
    ' "$config_input" > "$tmp"
    if [[ -e $config_dir/config ]]; then
        chmod --reference="$config_dir/config" "$tmp"
    else
        chmod 644 "$tmp"
    fi
    mv -- "$tmp" "$config_dir/config"
    tmp=$(mktemp "$config_dir/.install.XXXXXX")
fi
# Only the persisted room list is authoritative; never enumerate systemd units.
{
    if [[ -f $rooms_file ]]; then cat -- "$rooms_file"; printf '\n'; fi
    if [[ ${#rooms[@]} -gt 0 ]]; then printf '%s\n' "${rooms[@]}"; fi
} | awk 'NF && !seen[$0]++' > "$tmp"
[[ -s $tmp ]] || fail "No rooms configured; pass one or more quoted room names."
chmod 644 "$tmp"
mv -- "$tmp" "$rooms_file"
unit_source=packaging/sonos-lms@.service
unit_target=/etc/systemd/system/sonos-lms@.service
if ! cmp -s -- "$unit_source" "$unit_target"; then
    install -m 644 -- "$unit_source" "$unit_target"
fi
systemctl daemon-reload
while IFS= read -r room; do
    unit=$(systemd-escape --template=sonos-lms@.service -- "$room")
    active=false
    if systemctl is-active --quiet "$unit"; then active=true; fi
    if ! systemctl is-enabled --quiet "$unit" || ! $active; then
        systemctl enable --now "$unit"
    fi
    if $active; then
        systemctl restart "$unit"
        printf '%s: restarted\n' "$room"
    else
        printf '%s: enabled and started\n' "$room"
    fi
done < "$rooms_file"
