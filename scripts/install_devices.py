#!/usr/bin/env python3
"""Config-driven service installation. Tests inject paths and a command runner."""
from pathlib import Path
import os
import stat
import subprocess
import sys
import tempfile
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
        elif arg == '--reconfigure':
            pass  # TTY installs already ask every question; never override automation.
        elif arg.startswith('--'):
            raise ValueError('Usage: scripts/install-devices.sh [--server=<lms-host-or-ip>] [--yes|--non-interactive|--reconfigure] [room ...]')
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


def ask(prompt, input_stream, output):
    print(prompt, end='', file=output, flush=True)
    value = input_stream.readline()
    if not value:
        raise EOFError('Input closed; installation cancelled.')
    return value.rstrip('\r\n')


def ask_yes(prompt, default, input_stream, output):
    while True:
        answer = ask(prompt, input_stream, output).lower()
        if not answer:
            return default
        if answer in ('y', 'yes', 'n', 'no'):
            return answer in ('y', 'yes')
        print('Please answer y/yes/n/no, or press Enter.', file=output)


def install(repo, config_dir, unit_dir, args, run=subprocess.run, output=sys.stdout, errors=sys.stderr,
            input_stream=sys.stdin):
    server, enabled, non_interactive = arguments(args)
    explicit = set(enabled)
    interactive = input_stream.isatty() and output.isatty() and not non_interactive
    warn = lambda message: print('Warning: ' + message, file=errors)
    # A failed discovery contributes no partial stdout, and never removes offline rooms.
    try:
        discovery = run(['./sonos-lms', '--list-rooms'], cwd=repo,
                        capture_output=True, text=True)
    except OSError as error:
        warn(f'Room discovery failed ({error}); keeping the existing configuration.')
        discovery = None
    found = []
    if discovery is not None:
        if discovery.returncode or not discovery.stdout.strip():
            detail = discovery.stderr.strip()
            warn('Room discovery failed; keeping the existing configuration.' + (f' {detail}' if detail else ''))
        else:
            for name in discovery.stdout.splitlines():
                if valid_room(name):
                    found.append(name)
                else:
                    warn(f'Cannot represent discovered room {name!r} in the config; skipping it.')

    config = config_dir / 'config'
    legacy = config_dir / 'rooms'
    migrated = config_dir / 'rooms.migrated'
    migrate = legacy.exists() and not migrated.exists()
    if migrate:
        for name in read_text(legacy).splitlines():
            if not name.strip():
                continue
            if not valid_room(name):
                raise ValueError(f'Cannot migrate room {name!r}; fix {legacy} first.')
            enabled.append(name)
    elif legacy.exists():
        warn(f'{migrated} already exists; ignoring {legacy} (migration already completed).')

    original = read_text(config)
    current_server = next((entry[1].strip() for line in original.splitlines()
                           if (entry := setting(line)) and entry[0] == 'LMS_SERVER'), None)
    if server is None and current_server is None:
        try:
            result = run(['./sonos-lms', '--find-server'], cwd=repo, capture_output=True, text=True)
            server = result.stdout.strip() if result.returncode == 0 else ''
            if not valid_server(server):
                warn('Invalid discovered LMS host; leaving LMS_SERVER empty.')
                server = ''
        except OSError as error:
            warn(f'LMS discovery failed ({error}); leaving LMS_SERVER empty.')
            server = ''
    merged, new = merge_config(original, found, enabled, server)
    rooms = parse_rooms(merged, warn)
    if interactive:
        try:
            shown = server if server is not None else current_server
            while True:
                answer = ask(f'LMS server [{shown}]: ', input_stream, output)
                if not answer:
                    break
                if valid_server(answer):
                    server = answer
                    break
                print('Server must have no port, whitespace or control characters.', file=output)
            answers = {}
            for room in sorted(set(found) - explicit):
                default = rooms[room]
                answers[room] = ask_yes(f'Activate Sonos room "{room}"? '
                                        + ('[Y/n] ' if default else '[y/N] '),
                                        default, input_stream, output)
            for room in sorted(set(rooms) - set(found)):
                print(f'offline, kept: {room} ({"yes" if rooms[room] else "no"})', file=output)
            merged, new = merge_config(original, found, enabled, server, answers)
            print('Proposed changes:', file=output)
            import difflib
            changes = ''.join(difflib.unified_diff(original.splitlines(keepends=True),
                              merged.splitlines(keepends=True), fromfile='current config',
                              tofile='proposed config'))
            print(changes or 'No config changes; apply configured room services.', file=output)
            if migrate:
                print('Migrate rooms to rooms.migrated.', file=output)
            if not ask_yes('Apply? [Y/n] ', True, input_stream, output):
                print('Cancelled; no changes applied.', file=output)
                return
        except (KeyboardInterrupt, EOFError):
            print('\nCancelled; no changes applied.', file=output)
            return
    config_dir.mkdir(parents=True, exist_ok=True)
    if merged != original:
        atomic_write(config, merged)
    # Never retire the old list before all of its rooms have been persisted.
    if migrate:
        os.replace(legacy, migrated)
    rooms = parse_rooms(merged, warn)

    unit_dir.mkdir(parents=True, exist_ok=True)
    target = unit_dir / 'sonos-lms@.service'
    template = read_text(repo / 'packaging/sonos-lms@.service')
    if not template:
        raise ValueError('Missing packaging/sonos-lms@.service')
    if read_text(target) != template:
        atomic_write(target, template)
    run(['systemctl', 'daemon-reload'], check=True)
    for room, enable in rooms.items():
        unit = run(['systemd-escape', '--template=sonos-lms@.service', '--', room],
                   capture_output=True, text=True, check=True).stdout.strip()
        if not unit or '\n' in unit:
            raise ValueError(f'Invalid systemd-escape result for {room!r}')
        if enable:
            active = run(['systemctl', 'is-active', '--quiet', unit]).returncode == 0
            is_enabled = run(['systemctl', 'is-enabled', '--quiet', unit]).returncode == 0
            if not is_enabled or not active:
                run(['systemctl', 'enable', '--now', unit], check=True)
            if active:
                run(['systemctl', 'restart', unit], check=True)
            running = run(['systemctl', 'is-active', '--quiet', unit]).returncode == 0
            print(f'{room}: enabled ({"running" if running else "not running"})', file=output)
        else:
            state = run(['systemctl', 'show', '--property=LoadState', '--value', unit],
                        capture_output=True, text=True, check=True).stdout.strip()
            if state != 'not-found':
                run(['systemctl', 'stop', unit], check=True)
                run(['systemctl', 'disable', unit], check=True)
            print(f'{room}: disabled', file=output)
    if not rooms:
        print('No rooms configured.', file=output)
    for room in new:
        if not rooms.get(room, False):
            print(f'New rooms found: {room} — set room.{room}=yes in {config} and run make install to use it', file=output)


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
