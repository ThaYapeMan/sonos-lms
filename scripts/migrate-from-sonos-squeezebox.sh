#!/usr/bin/env bash
# migrate-from-sonos-squeezebox.sh -- one-off migration of a bridge host from
# the old sonos-squeezebox installation to sonos-lms.
#
# Run as root on the bridge host, from the old checkout after pointing it at
# the new repository:
#
#   cd /opt/sonos-squeezebox
#   git remote set-url origin https://github.com/ThaYapeMan/sonos-lms.git
#   git pull --ff-only && git submodule update --init --recursive
#   bash scripts/migrate-from-sonos-squeezebox.sh
#
# What it does:
#   1. records every enabled sonos-squeezebox@<room> service, stops and disables them
#   2. moves /opt/sonos-squeezebox -> /opt/sonos-lms and /etc/sonos-squeezebox -> /etc/sonos-lms
#   3. moves drop-ins sonos-squeezebox@*.service.d -> sonos-lms@*.service.d and
#      renames SONOS_SQUEEZEBOX_* settings to SONOS_LMS_* inside them
#   4. removes the old unit file, builds, and installs + starts sonos-lms@<room>
#      for the same rooms (scripts/install-devices.sh)
# It stops at the first error; nothing is deleted except the old unit file.

set -euo pipefail

OLD=sonos-squeezebox
NEW=sonos-lms
fail() { printf 'Error: %s\n' "$*" >&2; exit 1; }
step() { printf '\n== %s\n' "$*"; }

[[ $EUID -eq 0 ]] || fail "run as root"
[[ -d /opt/$OLD/.git ]] || fail "/opt/$OLD not found (already migrated?)"
[[ ! -e /opt/$NEW ]] || fail "/opt/$NEW already exists; remove or rename it first"
[[ -f /opt/$OLD/packaging/$NEW@.service ]] || fail "/opt/$OLD is not on the renamed code yet; run the git remote set-url + git pull first"

step "1. old services"
mapfile -t instances < <(
    find /etc/systemd/system -path "*.wants/$OLD@*.service" -printf '%f\n' 2>/dev/null \
    | sed -e "s/^$OLD@//" -e 's/\.service$//' | sort -u)
rooms=()
for inst in "${instances[@]}"; do
    room=$(systemd-escape --unescape -- "$inst")
    rooms+=("$room")
    printf '   %s (unit %s@%s.service)\n' "$room" "$OLD" "$inst"
done
[[ ${#rooms[@]} -gt 0 ]] || echo "   none enabled; the rooms file decides"
for inst in "${instances[@]}"; do
    systemctl disable --now "$OLD@$inst.service"
done
# Anything still running from the old template (e.g. started by hand).
systemctl stop "$OLD@*.service" 2>/dev/null || true

step "2. move checkout and configuration"
mv -- "/opt/$OLD" "/opt/$NEW"
echo "   /opt/$OLD -> /opt/$NEW"
if [[ -d /etc/$OLD ]]; then
    [[ ! -e /etc/$NEW ]] || fail "/etc/$NEW already exists; merge it with /etc/$OLD by hand"
    mv -- "/etc/$OLD" "/etc/$NEW"
    echo "   /etc/$OLD -> /etc/$NEW"
fi

step "3. drop-ins"
shopt -s nullglob
for d in /etc/systemd/system/"$OLD"@*.service.d; do
    nd=${d/$OLD@/$NEW@}
    [[ ! -e $nd ]] || fail "$nd already exists"
    mv -- "$d" "$nd"
    for f in "$nd"/*.conf; do sed -i 's/SONOS_SQUEEZEBOX_/SONOS_LMS_/g' "$f"; done
    echo "   $(basename "$d") -> $(basename "$nd")"
    grep -h '^Environment' "$nd"/*.conf 2>/dev/null | sed 's/^/      /' || true
done
shopt -u nullglob

step "4. remove old unit, build, install new services"
rm -f -- "/etc/systemd/system/$OLD@.service"
systemctl daemon-reload
cd "/opt/$NEW"
# noson's CMake cache records the old absolute path; a stale cache makes any
# later noson rebuild fail. The built library stays; only the cache goes.
if grep -qs "/opt/$OLD" noson/CMakeCache.txt; then
    rm -rf -- noson/CMakeCache.txt noson/CMakeFiles
    echo "   removed stale noson CMake cache (old path)"
fi
make
scripts/install-devices.sh "${rooms[@]}"

step "5. check"
sleep 3
systemctl --no-pager --plain list-units "$NEW@*.service"
journalctl -u "$NEW@*" --since "-1min" --no-pager -o cat | grep -E 'SONOS_LMS_PAUSE|session' | head -n 6 || true
cat <<EOF

Done. Expect every room active above and SONOS_LMS_PAUSE=stop in the log.
Test: cd /opt/$NEW && scripts/device-test.sh
EOF
