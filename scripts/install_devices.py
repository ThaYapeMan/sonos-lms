#!/usr/bin/env python3
"""Config-driven service installation. Tests inject paths and a command runner."""
from pathlib import Path
import difflib
import hashlib
import queue
import socket
import threading
import time
from urllib.parse import unquote
import os
import stat
import subprocess
import sys
import tempfile
import termios
import unicodedata

ROOM_HEADER = ('# Sonos rooms found on the network.\n'
               '# yes = bridge this room to LMS, no = ignore it.\n')
LMS_COMMENT = '# LMS host or IP (no port). Empty = automatic discovery on the local network.\n'
TRUE = {'yes', '1', 'true', 'on'}
FALSE = {'no', '0', 'false', 'off'}


def valid_room(name):
    return bool(name and not name.isspace() and not any(c in name for c in '=\r\n\0'))


def valid_server(value):
    return not any(c == ':' or c.isspace() or unicodedata.category(c).startswith('C') for c in value)


def arguments(args):
    server = None
    rooms = []
    non_interactive = False
    for arg in args:
        if arg.startswith('--server='):
            server = arg[len('--server='):]
            if not valid_server(server):
                raise ValueError('Server must have no port, whitespace or control characters.')
        elif arg in ('--yes', '--non-interactive'):
            non_interactive = True
        elif arg in ('--reconfigure', '--restart'):
            pass  # Applied when selecting rooms and planning service actions.
        elif arg.startswith('--'):
            raise ValueError('Usage: scripts/install-devices.sh [--server=<lms-host-or-ip>] [--yes|--non-interactive|--reconfigure] [--restart] [room ...]')
        elif not valid_room(arg):
            raise ValueError('Room names must be nonempty and cannot contain "=", newlines or NUL.')
        else:
            rooms.append(arg)
    return server, rooms, non_interactive


def setting(line):
    """Indentation is optional; everything after room. up to '=' is the name."""
    line = line.lstrip(' \t')
    if line.startswith(('#', ';')) or '=' not in line:
        return None
    key, value = line.rstrip('\r\n').split('=', 1)
    return key, value


def parse_rooms(text, warn):
    rooms = {}
    for line in text.splitlines():
        entry = setting(line)
        if not entry or not entry[0].startswith('room.'):
            continue
        name, value = entry[0][5:], entry[1].strip().lower()
        if not valid_room(name):
            warn(f'Ignoring invalid room name {name!r}.')
            continue
        if name in rooms:
            warn(f'Duplicate room.{name}; the last value wins.')
        if value not in TRUE | FALSE:
            warn(f'Unknown value {entry[1]!r} for room.{name}; treating it as no.')
        rooms[name] = value in TRUE
    return rooms


def merge_config(text, discovered, enabled, server=None, answers=None):
    """Preserve all unmodified bytes, including comments, CRLF and unknown values."""
    lines = text.splitlines(keepends=True)
    existing = {entry[0][5:] for line in lines if (entry := setting(line))
                and entry[0].startswith('room.')}
    new = sorted(set(discovered) - existing)
    answers = answers or {}
    updates = {'room.' + name: 'yes' for name in enabled}
    updates.update({'room.' + name: 'yes' if value else 'no' for name, value in answers.items()})
    if server is not None:
        updates['LMS_SERVER'] = server
    seen = set()
    for i, line in enumerate(lines):
        entry = setting(line)
        if not entry or entry[0] not in updates:
            continue
        key = entry[0]
        ending = '\r\n' if line.endswith('\r\n') else '\n' if line.endswith('\n') else ''
        lines[i] = line.split('=', 1)[0] + '=' + updates[key] + ending
        seen.add(key)
    result = ''.join(lines)

    def append(line):
        nonlocal result
        if result and not result.endswith('\n'):
            result += '\n'
        result += line

    if server is not None and 'LMS_SERVER' not in seen:
        append(LMS_COMMENT + 'LMS_SERVER=' + server + '\n')
    additions = sorted((set(discovered) | set(enabled)) - existing)
    if additions and not existing:
        append(ROOM_HEADER)
    for name in additions:
        append('room.' + name + ('=yes\n' if answers.get(name, name in enabled) else '=no\n'))
    return result, new


def read_text(path):
    if not path.exists():
        return ''
    with path.open(encoding='utf-8', newline='') as file:
        return file.read()


def atomic_write(path, text, mode=0o644):
    previous = path.stat() if path.exists() else None
    fd, name = tempfile.mkstemp(prefix='.install.', dir=path.parent)
    try:
        with os.fdopen(fd, 'w', encoding='utf-8', newline='') as file:
            file.write(text)
            file.flush()
            os.fsync(file.fileno())
            os.fchmod(file.fileno(), stat.S_IMODE(previous.st_mode) if previous else mode)
            if previous and os.geteuid() == 0:
                os.fchown(file.fileno(), previous.st_uid, previous.st_gid)
        os.replace(name, path)
    finally:
        if os.path.exists(name):
            os.unlink(name)


def flush_pending_input(input_stream):
    if input_stream.isatty():
        try:
            termios.tcflush(input_stream.fileno(), termios.TCIFLUSH)
        except (OSError, ValueError, termios.error):
            pass


def quoted_input(value):
    # Keep ordinary text readable, but never emit terminal control characters.
    return '"' + ''.join('\\\\' if c == '\\' else '\\"' if c == '"'
                         else repr(c)[1:-1] if not c.isprintable() else c
                         for c in value) + '"'


def ask(prompt, input_stream, output):
    print(prompt, end='', file=output, flush=True)
    value = input_stream.readline()
    if not value:
        raise EOFError('Input closed; installation cancelled.')
    return value.rstrip('\r\n')


def ask_yes(prompt, default, input_stream, output):
    while True:
        original = ask(prompt, input_stream, output)
        answer = original.lower()
        if not answer:
            return default
        if answer in ('y', 'yes', 'n', 'no'):
            return answer in ('y', 'yes')
        print(f'Rejected {quoted_input(original)}: please answer y, n or press Enter.', file=output)


def lms_value(text):
    return next((entry[1].strip() for line in text.splitlines()
                 if (entry := setting(line)) and entry[0] == 'LMS_SERVER'), None)


def build_identity(repo, run):
    def git(*args):
        return run(['git', *args], cwd=repo, capture_output=True, text=True, check=True).stdout.strip()
    describe = git('describe', '--always', '--dirty')
    commit = git('rev-parse', 'HEAD')
    if git('status', '--porcelain', '--untracked-files=no'):
        commit += '-dirty'
    digest = hashlib.sha256((repo / 'sonos-lms').read_bytes()).hexdigest()
    return describe, f'commit={commit}\nsha256={digest}\n'


def parse_details(text, warn):
    found = {}
    for line in text.splitlines():
        fields = line.split('\t')
        if len(fields) != 5 or not valid_room(fields[0]):
            warn(f'Cannot represent discovered room details {line!r}; skipping it.')
            continue
        name, model, ip, coordinator, members = fields
        found.setdefault(name, (model, ip, coordinator, members.split(',') if members != '-' else []))
    return found


def service_states(names, run):
    result = {}
    for room in sorted(names):
        unit = run(['systemd-escape', '--template=sonos-lms@.service', '--', room],
                   capture_output=True, text=True, check=True).stdout.strip()
        if not unit or '\n' in unit:
            raise ValueError(f'Invalid systemd-escape result for {room!r}')
        active = run(['systemctl', 'is-active', '--quiet', unit]).returncode == 0
        enabled = run(['systemctl', 'is-enabled', '--quiet', unit]).returncode == 0
        result[room] = (unit, active, enabled)
    return result


def room_table(found, configured, states, output):
    offline = sorted(set(configured) - set(found))
    print(f'Sonos rooms found: {len(found)}' + (f', offline: {len(offline)}' if offline else ''), file=output)
    rows = [('Room', 'Model', 'IP', 'Group', 'Bridge to LMS')]
    for name in sorted(set(found) | set(configured)):
        model, ip, coordinator, members = found.get(name, ('-', '-', '-', []))
        group = '-'
        if len(members) > 1:
            group = 'coordinator: ' + '+'.join(members) if coordinator == name else 'member of ' + coordinator
        bridge = ('yes, running' if states[name][1] else 'yes, stopped') if configured.get(name) else 'no'
        if name not in configured:
            bridge = 'new'
        if name not in found:
            bridge = 'offline, ' + ('yes' if configured[name] else 'no')
        rows.append((name, model, ip, group, bridge))
    widths = [max(len(row[i]) for row in rows) for i in range(5)]
    for row in rows:
        print('  '.join(value.ljust(width) for value, width in zip(row, widths)).rstrip(), file=output)


def service_plan(rooms, states, build_changed, build, server_changed, force):
    plan = {kind: [] for kind in ('Start', 'Restart', 'Stop', 'Keep', 'Enable')}
    reasons = []
    if force: reasons.append('forced')
    if build_changed: reasons.append('new build ' + build)
    if server_changed: reasons.append('LMS server changed')
    for room, wanted in rooms.items():
        _, active, enabled = states[room]
        if wanted:
            if not active: plan['Start'].append(room)
            elif reasons: plan['Restart'].append(room)
            else: plan['Keep'].append(room)
            if active and not enabled: plan['Enable'].append(room)
        elif active or enabled:
            plan['Stop'].append(room)
    return plan, '; '.join(reasons)


def connected_players(reply):
    """Split wire tokens BEFORE URL decoding; names can contain spaces and colons."""
    connected = set()
    record = {}
    for token in reply.split():
        key, sep, value = unquote(token).partition(':')
        if key == 'playerindex':
            if record.get('connected') == '1' and 'name' in record: connected.add(record['name'])
            record = {}
        if sep: record[key] = value
    if record.get('connected') == '1' and 'name' in record: connected.add(record['name'])
    return connected


def check_lms(host, rooms, timeout=15, connect=socket.create_connection):
    """One shared deadline, including DNS/connect/read, regardless of room count.

    Network work is in a daemon so even a stalled OS resolver cannot hold up install.
    The worker only reads LMS; it never mutates config or services.
    """
    if not rooms: return {}, ''
    if not host: return {}, 'no LMS host known'
    events, stop = queue.Queue(), threading.Event()
    deadline = time.monotonic() + timeout
    wanted = {room + ' (Sonos)' for room in rooms}
    def poll():
        try:
            while not stop.is_set() and time.monotonic() < deadline:
                remaining = max(0.001, deadline - time.monotonic())
                with connect((host, 9090), timeout=min(2, remaining)) as sock:
                    sock.settimeout(min(2, remaining))
                    sock.sendall(b'players 0 999\n')
                    data = b''
                    while b'\n' not in data:
                        sock.settimeout(max(0.001, min(2, deadline - time.monotonic())))
                        chunk = sock.recv(65536)
                        if not chunk: raise OSError('CLI connection closed')
                        data += chunk
                        if len(data) > 1024 * 1024: raise OSError('CLI reply too large')
                        if stop.is_set() or time.monotonic() >= deadline: return
                    seen = connected_players(data.split(b'\n', 1)[0].decode('utf-8', errors='replace'))
                    events.put(('seen', seen))
                    if wanted <= seen: return
                stop.wait(min(0.5, max(0, deadline - time.monotonic())))
        except OSError as error:
            events.put(('skip', f'port 9090 unreachable: {error}'))
    threading.Thread(target=poll, daemon=True).start()
    seen, skipped = set(), ''
    try:
        while wanted - seen:
            remaining = deadline - time.monotonic()
            if remaining <= 0: break
            try: kind, value = events.get(timeout=remaining)
            except queue.Empty: break
            if kind == 'skip':
                skipped = value
                break
            seen.update(value)
    finally:
        stop.set()
    return {room: f'LMS player "{room} (Sonos)" ' +
            ('connected' if room + ' (Sonos)' in seen else f'not seen after {timeout:g} s')
            for room in rooms}, skipped


def install(repo, config_dir, unit_dir, args, run=subprocess.run, output=sys.stdout, errors=sys.stderr,
            input_stream=sys.stdin, lms_check=check_lms):
    server, enabled, non_interactive = arguments(args)
    explicit = set(enabled)
    interactive = input_stream.isatty() and output.isatty() and not non_interactive
    warn = lambda message: print('Warning: ' + message, file=errors)
    build, identity = build_identity(repo, run)
    print(f'sonos-lms installer — build {build}', file=output)
    config, stamp = config_dir / 'config', config_dir / 'installed-build'
    original = read_text(config)
    configured = parse_rooms(original, warn)
    current_server = lms_value(original)
    discovered_server = ''
    try:
        result = run(['./sonos-lms', '--find-server'], cwd=repo, capture_output=True, text=True)
        if result.returncode == 0 and valid_server(result.stdout.strip()):
            discovered_server = result.stdout.strip()
    except OSError as error:
        warn(f'LMS discovery failed ({error}).')
    if current_server is not None and discovered_server and current_server != discovered_server:
        warn(f'LMS config is {current_server or "<empty>"}; discovery found {discovered_server}. Keeping config unless overridden.')
    source = '--server' if server is not None else 'config' if current_server is not None else 'discovery' if discovered_server else 'none'
    if server is None and current_server is None: server = discovered_server
    shown = server if server is not None else current_server
    print(f'LMS server: {shown or "-"} ({source})', file=output)
    try:
        if interactive:
            flush_pending_input(input_stream)
            while True:
                answer = ask(f'LMS server [{shown}]: ', input_stream, output)
                if not answer: break
                if valid_server(answer):
                    server = answer
                    break
                print(f'Rejected {quoted_input(answer)}: LMS server must be a host or IP without port or spaces.', file=output)
        found = {}
        try:
            discovery = run(['./sonos-lms', '--list-rooms', '--details'], cwd=repo, capture_output=True, text=True)
            if discovery.returncode or not discovery.stdout.strip():
                warn('Room discovery failed; keeping the existing configuration. ' + discovery.stderr.strip())
            else:
                found = parse_details(discovery.stdout, warn)
        except OSError as error:
            warn(f'Room discovery failed ({error}); keeping the existing configuration.')
        legacy, migrated = config_dir / 'rooms', config_dir / 'rooms.migrated'
        migrate = legacy.exists() and not migrated.exists()
        if migrate:
            for name in read_text(legacy).splitlines():
                if not name.strip(): continue
                if not valid_room(name): raise ValueError(f'Cannot migrate room {name!r}; fix {legacy} first.')
                enabled.append(name)
        elif legacy.exists():
            warn(f'{migrated} already exists; ignoring {legacy} (migration already completed).')
        merged, new = merge_config(original, found, enabled, server)
        rooms = parse_rooms(merged, warn)
        states = service_states(rooms, run)
        room_table(found, configured, states, output)
        answers = {}
        def select(names):
            for room in sorted(set(names) - explicit):
                answers[room] = ask_yes(f'Bridge Sonos room "{room}" to LMS? ' +
                                        ('[Y/n] ' if rooms[room] else '[y/N] '),
                                        rooms[room], input_stream, output)
        if interactive:
            if not configured or '--reconfigure' in args:
                select(found)
            elif new:
                select(new)
                if ask_yes('Change other rooms bridged to LMS? [y/N] ', False, input_stream, output):
                    select(set(found) - set(new))
            elif ask_yes('Change which rooms are bridged to LMS? [y/N] ', False, input_stream, output):
                select(found)
        merged, new = merge_config(original, found, enabled, server, answers)
        rooms = parse_rooms(merged, warn)
        build_changed = read_text(stamp) != identity
        server_changed = (lms_value(merged) or '') != (current_server or '')
        plan, reason = service_plan(rooms, states, build_changed, build, server_changed, '--restart' in args)
        print('Config:  ' + ('no changes' if merged == original else '\n' + ''.join(difflib.unified_diff(
            original.splitlines(keepends=True), merged.splitlines(keepends=True),
            fromfile='current config', tofile='proposed config'))), file=output)
        for kind, names in plan.items():
            if names:
                suffix = f' ({reason}); playback stops briefly' if kind == 'Restart' else ''
                print(f'{kind + ":":9}' + ', '.join(names) + suffix, file=output)
        target = unit_dir / 'sonos-lms@.service'
        template = read_text(repo / 'packaging/sonos-lms@.service')
        if not template: raise ValueError('Missing packaging/sonos-lms@.service')
        template_changed = read_text(target) != template
        if template_changed: print('Service template: install/update', file=output)
        if migrate: print('Migration: rooms → rooms.migrated', file=output)
        if build_changed: print('Build record: update installed-build', file=output)
        actions = any(plan[kind] for kind in ('Start', 'Restart', 'Stop', 'Enable'))
        work = merged != original or actions or template_changed or migrate or build_changed
        if not work:
            print('Nothing to do.', file=output)
        elif interactive and not ask_yes('Apply? [Y/n] ', True, input_stream, output):
            print('Cancelled; no changes applied.', file=output)
            return
    except (KeyboardInterrupt, EOFError):
        print('\nCancelled; no changes applied.', file=output)
        return
    if work:
        config_dir.mkdir(parents=True, exist_ok=True)
        if merged != original: atomic_write(config, merged)
        if migrate: os.replace(legacy, migrated)
        unit_dir.mkdir(parents=True, exist_ok=True)
        if template_changed:
            atomic_write(target, template)
            run(['systemctl', 'daemon-reload'], check=True)
        for kind in ('Stop', 'Enable', 'Start', 'Restart'):
            for room in plan[kind]:
                unit = states[room][0]
                if kind == 'Stop':
                    run(['systemctl', 'stop', unit], check=True)
                    run(['systemctl', 'disable', unit], check=True)
                elif kind == 'Enable':
                    run(['systemctl', 'enable', unit], check=True)
                elif kind == 'Start':
                    run(['systemctl', 'enable', '--now', unit], check=True)
                else:
                    run(['systemctl', 'restart', unit], check=True)
        # A failed service action must never bless this build as installed.
        if build_changed: atomic_write(stamp, identity)
    host = lms_value(merged) or discovered_server
    checks, skipped = lms_check(host, [room for room in rooms if rooms[room]])
    if skipped: print(f'LMS check skipped ({skipped})', file=output)
    width = max([len(room) for room in rooms] + [4])
    for room, wanted in rooms.items():
        active = run(['systemctl', 'is-active', '--quiet', states[room][0]]).returncode == 0
        state = 'running' if active else 'stopped' if wanted else 'disabled'
        print((f'{room:<{width}}  {state:<8}  ' + (checks.get(room, '') if not skipped else '')).rstrip(), file=output)
    if not rooms: print('No rooms configured.', file=output)
    disabled_new = [room for room in new if not rooms[room]]
    if disabled_new and not interactive:
        print(f'New rooms added as no: {", ".join(disabled_new)} — edit {config} or run sudo make install in a terminal.', file=output)
    print("Logs: journalctl -u 'sonos-lms@*' -f", file=output)

def main():
    if os.geteuid() != 0:
        print('Error: Run as root.', file=sys.stderr)
        return 1
    repo = Path(__file__).resolve().parents[1]
    if not (repo / 'sonos-lms').is_file():
        print('Error: Missing ./sonos-lms; run make first.', file=sys.stderr)
        return 1
    try:
        install(repo, Path('/etc/sonos-lms'), Path('/etc/systemd/system'), sys.argv[1:])
    except (OSError, ValueError, subprocess.CalledProcessError) as error:
        print(f'Error: {error}', file=sys.stderr)
        return 1
    return 0


if __name__ == '__main__':
    sys.exit(main())
