"""Installer merge and apply tests; all paths are temporary and systemctl is mocked."""
from pathlib import Path
import importlib.util
import io
import os
import pty
import select
import time
import stat
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import patch

sys.dont_write_bytecode = True
ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location('installer', ROOT / 'scripts/install_devices.py')
installer = importlib.util.module_from_spec(spec)
spec.loader.exec_module(installer)


class Commands:
    def __init__(self, rooms='Study\nSonos Port\nKitchen\n', discovery_status=0):
        self.rooms, self.discovery_status = rooms, discovery_status
        self.server = ''
        self.build = 'build-a'
        self.details = None
        self.calls = []
        self.active, self.enabled, self.missing = set(), set(), set()

    def __call__(self, args, **kwargs):
        self.calls.append(args)
        if args == ['./sonos-lms', '--list-rooms', '--details']:
            assert kwargs['cwd'] == ROOT
            return subprocess.CompletedProcess(args, self.discovery_status, self.details if self.details is not None else ''.join(name + '\t-\t-\t-\t-\n' for name in self.rooms.splitlines()),
                                               'No Sonos rooms found.' if self.discovery_status else '')
        if args == ['./sonos-lms', '--find-server']:
            return subprocess.CompletedProcess(args, 0 if self.server else 2, self.server, '')
        if args[0] == 'git':
            value = '' if args[1] == 'status' else self.build
            return subprocess.CompletedProcess(args, 0, value + '\n', '')
        if args[0] == 'systemd-escape':
            return subprocess.run(args, **kwargs)
        assert args[0] == 'systemctl', args
        action, unit = args[1], args[-1]
        code, output = 0, ''
        if action == 'is-active': code = 0 if unit in self.active else 3
        elif action == 'is-enabled': code = 0 if unit in self.enabled else 1
        elif action == 'show': output = 'not-found\n' if unit in self.missing else 'loaded\n'
        elif action == 'enable':
            self.enabled.add(unit)
            if '--now' in args: self.active.add(unit)
        elif action == 'restart': self.active.add(unit)
        elif action == 'stop': self.active.discard(unit)
        elif action == 'disable': self.enabled.discard(unit)
        else: assert action == 'daemon-reload', args
        return subprocess.CompletedProcess(args, code, output, '')

    def actions(self):
        return [args for args in self.calls if args[0] == 'systemctl'
                and args[1] in ('enable', 'restart', 'stop', 'disable')]


def mock_lms(host, rooms):
    # Exercise the real parser/polling code with an in-memory CLI socket.
    class Socket:
        def __enter__(self): return self
        def __exit__(self, *args): pass
        def settimeout(self, value): pass
        def sendall(self, value): assert value == b'players 0 999\n'
        def recv(self, size):
            from urllib.parse import quote
            return ('players 0 999 ' + ' '.join(f'playerindex%3A{i} name%3A{quote(room + " (Sonos)")} connected%3A1' for i, room in enumerate(rooms)) + '\n').encode()
    return installer.check_lms(host, rooms, connect=lambda *a, **k: Socket())


class InstallerTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix='sonos-installer-')
        self.addCleanup(self.temp.cleanup)
        self.config_dir = Path(self.temp.name) / 'etc/sonos-lms'
        self.config_dir.mkdir(parents=True)
        self.unit_dir = Path(self.temp.name) / 'etc/systemd/system'
        self.config = self.config_dir / 'config'
        self.out, self.err = io.StringIO(), io.StringIO()

    def run_install(self, runner=None, args=()):
        runner = runner or Commands()
        installer.install(ROOT, self.config_dir, self.unit_dir, args, runner, self.out, self.err, lms_check=mock_lms)
        return runner

    def test_parsing_exact_names_boolean_aliases_and_comments(self):
        warnings = []
        text = '# room.Comment=yes\n; room.Other=yes\nLMS_SERVER=host\n  room.Sonos Port=YeS\n'
        for i, value in enumerate(('yes', '1', 'TRUE', 'On', 'no', '0', 'FALSE', 'OFF', 'nonsense', '', 'yes # comment')):
            text += f'room.Room {i}={value}\n'
        text += 'room. Room with edge spaces = true \nroom.Bad=perhaps\nroom.Bad=off\n'
        rooms = installer.parse_rooms(text, warnings.append)
        self.assertTrue(rooms['Sonos Port'])
        self.assertTrue(rooms[' Room with edge spaces '])
        self.assertNotIn('Comment', rooms)
        self.assertNotIn('Other', rooms)
        self.assertEqual([rooms[f'Room {i}'] for i in range(11)], [True]*4 + [False]*7)
        self.assertFalse(rooms['Bad'])
        self.assertEqual(sum('Unknown value' in warning for warning in warnings), 4)
        self.assertTrue(any('Duplicate room.Bad' in warning for warning in warnings))

    def test_merge_preserves_existing_values_offline_rooms_and_bytes(self):
        original = '# keep this\r\nLMS_SERVER=server\\path\r\nroom.Study=ON\r\nroom.Offline= true \r\nroom.Bad=weird'
        merged, new = installer.merge_config(original, ['Study', 'Kitchen', 'Kitchen', 'Sonos Port'], [])
        self.assertTrue(merged.startswith(original + '\n'))
        self.assertTrue(merged.endswith('room.Kitchen=no\nroom.Sonos Port=no\n'))
        self.assertEqual(new, ['Kitchen', 'Sonos Port'])
        self.assertEqual(installer.merge_config(merged, ['Kitchen'], [])[0], merged)
        self.assertEqual(installer.merge_config(original, [], [])[0], original)

    def test_first_install_discovers_disabled_rooms_only(self):
        runner = self.run_install()
        self.assertEqual(self.config.read_text(), installer.LMS_COMMENT + 'LMS_SERVER=\n' + installer.ROOM_HEADER
                         + 'room.Kitchen=no\nroom.Sonos Port=no\nroom.Study=no\n')
        self.assertFalse(runner.enabled)
        self.assertFalse(runner.active)
        self.assertEqual(self.out.getvalue().count('New rooms added as no:'), 1)

    def test_active_not_enabled_and_inactive_enabled(self):
        self.config.write_text('room.Study=yes\nroom.Sonos Port=on\n')
        runner = Commands('Study\nSonos Port\n')
        runner.active.add('sonos-lms@Study.service')
        runner.enabled.add(r'sonos-lms@Sonos\x20Port.service')
        self.run_install(runner)
        self.assertEqual(runner.actions(), [
            ['systemctl', 'enable', 'sonos-lms@Study.service'],
            ['systemctl', 'enable', '--now', r'sonos-lms@Sonos\x20Port.service'],
            ['systemctl', 'restart', 'sonos-lms@Study.service'],
        ])

    def test_migration_new_rooms_and_apply_plan(self):
        self.config.write_text('# Existing settings\nLMS_SERVER=lms.example\n')
        legacy = self.config_dir / 'rooms'
        legacy.write_text('Study\nSonos Port\n')
        runner = Commands()
        runner.active.add('sonos-lms@Study.service')
        runner.enabled.add('sonos-lms@Study.service')
        self.run_install(runner)
        self.assertEqual(self.config.read_text(), '# Existing settings\nLMS_SERVER=lms.example\n' + installer.ROOM_HEADER
                         + 'room.Kitchen=no\nroom.Sonos Port=yes\nroom.Study=yes\n')
        self.assertFalse(legacy.exists())
        self.assertEqual((self.config_dir / 'rooms.migrated').read_text(), 'Study\nSonos Port\n')
        self.assertEqual(runner.actions(), [
            ['systemctl', 'enable', '--now', r'sonos-lms@Sonos\x20Port.service'],
            ['systemctl', 'restart', 'sonos-lms@Study.service'],
        ])
        self.assertRegex(self.out.getvalue(), r'Study +running +LMS player')
        self.assertRegex(self.out.getvalue(), r'Sonos Port +running +LMS player')
        self.assertRegex(self.out.getvalue(), r'Kitchen +disabled')
        self.assertIn('New rooms added as no: Kitchen — edit', self.out.getvalue())
        self.assertNotIn('New rooms found: Study', self.out.getvalue())
        # Once migrated, editing no must survive subsequent runs.
        self.config.write_text(self.config.read_text().replace('room.Study=yes', 'room.Study=no'))
        self.run_install(Commands())
        self.assertIn('room.Study=no', self.config.read_text())

    def test_explicit_rooms_and_server_override_migration_existing_values(self):
        self.config.write_text('# comment\nLMS_SERVER=old\n  room.Study=no\nroom.Sonos Port=off\nroom.Kitchen=TRUE\n')
        (self.config_dir / 'rooms').write_text('Study\n')
        self.run_install(args=('--server=new\\literal', 'Sonos Port', 'New Room'))
        self.assertEqual(self.config.read_text(), '# comment\nLMS_SERVER=new\\literal\n  room.Study=yes\nroom.Sonos Port=yes\nroom.Kitchen=TRUE\nroom.New Room=yes\n')
        self.assertNotIn('New rooms found:', self.out.getvalue())

    def test_failure_keeps_config_exact_and_applies_offline_rooms(self):
        original = '# untouched\r\nLMS_SERVER=old\r\nroom.Offline=yes\r\nroom.Kitchen=no'
        self.config.write_bytes(original.encode())
        self.config.chmod(0o640)
        inode = self.config.stat().st_ino
        runner = Commands('Spurious partial room\n', 2)
        runner.missing.add('sonos-lms@Kitchen.service')
        self.run_install(runner)
        self.assertEqual(self.config.read_bytes(), original.encode())
        self.assertEqual(self.config.stat().st_ino, inode)
        self.assertEqual(runner.actions(), [['systemctl', 'enable', '--now', 'sonos-lms@Offline.service']])
        self.assertIn('Room discovery failed', self.err.getvalue())
        self.assertNotIn('Spurious', self.config.read_text())
        self.assertRegex(self.out.getvalue(), r'Kitchen +disabled')

    def test_empty_discovery_and_absent_config(self):
        self.run_install(Commands(''))
        self.assertEqual(self.config.read_text(), installer.LMS_COMMENT + 'LMS_SERVER=\n')
        self.assertIn('Room discovery failed', self.err.getvalue())
        self.assertIn('No rooms configured.', self.out.getvalue())

    def test_failure_still_accepts_explicit_overrides(self):
        self.config.write_text('room.Study=no\n')
        self.run_install(Commands('', 2), ('Study', '--server=example'))
        self.assertEqual(self.config.read_text(), 'room.Study=yes\n' + installer.LMS_COMMENT + 'LMS_SERVER=example\n')

    def test_disabled_unknown_values_and_unrelated_units(self):
        self.config.write_text('room.Study=oops\nroom.Kitchen=OFF\n')
        runner = Commands('Study\nKitchen\n')
        runner.active = {'sonos-lms@Study.service', 'unrelated.service'}
        runner.enabled = set(runner.active)
        self.run_install(runner)
        self.assertEqual(runner.active, {'unrelated.service'})
        self.assertEqual(runner.enabled, {'unrelated.service'})
        self.assertIn("Unknown value 'oops'", self.err.getvalue())
        self.assertIn('room.Study=oops', self.config.read_text())

    def test_atomic_write_preserves_mode_and_failed_write_keeps_legacy(self):
        self.config.write_text('room.Study=no\n')
        self.config.chmod(0o640)
        (self.config_dir / 'rooms').write_text('Study\n')
        replace = os.replace
        def fail_config(source, dest):
            if dest == self.config: raise OSError('simulated rename failure')
            return replace(source, dest)
        with patch.object(installer.os, 'replace', side_effect=fail_config):
            with self.assertRaises(OSError): self.run_install()
        self.assertEqual(self.config.read_text(), 'room.Study=no\n')
        self.assertTrue((self.config_dir / 'rooms').exists())
        self.assertFalse((self.config_dir / 'rooms.migrated').exists())
        self.assertEqual(list(self.config_dir.glob('.install.*')), [])
        self.run_install()
        self.assertEqual(stat.S_IMODE(self.config.stat().st_mode), 0o640)

    def test_migration_is_not_repeated_or_backup_overwritten(self):
        self.config.write_text('room.Study=no\n')
        (self.config_dir / 'rooms.migrated').write_text('saved original\n')
        (self.config_dir / 'rooms').write_text('Study\n')
        self.run_install(Commands('Study\n'))
        self.assertEqual(self.config.read_text(), 'room.Study=no\n' + installer.LMS_COMMENT + 'LMS_SERVER=\n')
        self.assertEqual((self.config_dir / 'rooms.migrated').read_text(), 'saved original\n')
        self.assertIn('migration already completed', self.err.getvalue())

    def test_invalid_names_and_literal_shell_characters(self):
        for name in ('', ' ', 'Room=Wrong', 'Room\nWrong'):
            with self.assertRaises(ValueError): installer.arguments([name])
        runner = Commands('Bad=Name\nStudy\n')
        self.run_install(runner, ('Room $(literal)',))
        self.assertIn('room.Room $(literal)=yes', self.config.read_text())
        self.assertIn(['systemd-escape', '--template=sonos-lms@.service', '--', 'Room $(literal)'], runner.calls)
        self.assertNotIn('room.Bad', self.config.read_text())
        self.assertIn('Cannot represent discovered room', self.err.getvalue())



    def interactive(self, answers, runner=None, args=()):
        runner = runner or Commands()
        class TTY(io.StringIO):
            def isatty(self): return True
        self.out = TTY()
        installer.install(ROOT, self.config_dir, self.unit_dir, args, runner,
                          self.out, self.err, TTY(answers), lms_check=mock_lms)
        return runner

    def test_flush_once_only_interactive_tty_and_ignore_errors(self):
        with patch.object(installer, 'flush_pending_input') as flush:
            self.interactive('bad host\n\n\n\n\n\n')
            flush.assert_called_once()
            flush.reset_mock()
            self.run_install()
            self.interactive('', args=('--yes',))
            self.interactive('', args=('--non-interactive',))
            flush.assert_not_called()
        with patch.object(installer.termios, 'tcflush') as flush:
            installer.flush_pending_input(io.StringIO())
            flush.assert_not_called()
            with tempfile.TemporaryFile(mode='w+') as stream:
                with patch.object(stream, 'isatty', return_value=True):
                    installer.flush_pending_input(stream)
                    flush.assert_called_once_with(stream.fileno(), installer.termios.TCIFLUSH)
                    for error in (OSError(), ValueError(), installer.termios.error()):
                        flush.side_effect = error
                        installer.flush_pending_input(stream)

    def test_rejected_input_preserves_case_and_escapes_controls(self):
        out = io.StringIO()
        self.assertTrue(installer.ask_yes('Answer? ', False,
                        io.StringIO('Maybe\x01\t\nY\n'), out))
        self.assertIn('Rejected "Maybe\\x01\\t": please answer y, n or press Enter.', out.getvalue())
        self.assertNotIn('\x01', out.getvalue())
        self.assertEqual(installer.quoted_input('a"b\\c'), '"a\\"b\\\\c"')

    def test_lms_found_empty_existing_and_override(self):
        for host in ('lms.example', ''):
            self.config.unlink(missing_ok=True)
            runner = Commands(); runner.server = host
            self.run_install(runner)
            self.assertTrue(self.config.read_text().startswith(installer.LMS_COMMENT + f'LMS_SERVER={host}\n'))
        for host in ('original', ''):
            self.config.write_text(f'LMS_SERVER={host}\n')
            runner = self.run_install()
            self.assertTrue(self.config.read_text().startswith(f'LMS_SERVER={host}\n'))
            self.assertIn(['./sonos-lms', '--find-server'], runner.calls)
        self.run_install(args=('--server=override',))
        self.assertIn('LMS_SERVER=override', self.config.read_text())

    def test_interactive_answers_validation_defaults_and_offline(self):
        self.config.write_text('# keep\nLMS_SERVER=old\nroom.Study=ON\nroom.Offline= true \nOTHER=unchanged\n')
        self.interactive('n\nbad:9000\nbad host\nbad\x01host\nnew.example\nmaybe\ny\nn\n\n\n')
        self.assertEqual(self.config.read_text(), '# keep\nLMS_SERVER=new.example\nroom.Study=yes\nroom.Offline= true \nOTHER=unchanged\nroom.Kitchen=yes\nroom.Sonos Port=no\n')
        self.assertRegex(self.out.getvalue(), r'Offline +.*offline, yes')
        self.assertIn('"Study" to LMS? [Y/n]', self.out.getvalue())
        self.assertEqual(self.out.getvalue().count('LMS server [old]:'), 4)
        self.assertIn('Rejected "bad\\x01host": LMS server must be a host or IP without port or spaces.', self.out.getvalue())
        self.assertEqual(self.out.getvalue().count('"Kitchen" to LMS? [y/N]'), 2)

    def test_cancel_interrupt_and_eof_leave_everything_untouched(self):
        original = 'LMS_SERVER=old\nroom.Study=ON\n'
        self.config.write_text(original)
        legacy = self.config_dir / 'rooms'; legacy.write_text('Study\n')
        for mode in ('decline', 'interrupt', 'eof'):
            runner = Commands()
            if mode == 'interrupt':
                with patch.object(installer, 'ask', side_effect=KeyboardInterrupt):
                    self.interactive('', runner)
            else:
                self.interactive('n\n\ny\ny\nn\nn\n' if mode == 'decline' else '', runner)
            self.assertEqual(self.config.read_text(), original)
            self.assertTrue(legacy.exists())
            self.assertFalse(self.unit_dir.exists())
            self.assertFalse(runner.actions())
            self.assertFalse((self.config_dir / 'installed-build').exists())

    def test_flags_no_tty_and_explicit_room(self):
        for args in (('--yes',), ('--non-interactive',), ('--yes', '--reconfigure')):
            self.config.unlink(missing_ok=True)
            self.interactive('', args=args)
            self.assertNotIn('Apply?', self.out.getvalue())
            self.assertIn('room.Study=no', self.config.read_text())
        self.config.unlink()
        self.run_install(args=('--reconfigure',))  # non-TTY still never asks
        self.assertIn('room.Study=no', self.config.read_text())
        self.interactive('\n\n\n\n', args=('Study', '--reconfigure'))
        self.assertNotIn('"Study" to LMS?', self.out.getvalue())
        self.assertIn('room.Study=yes', self.config.read_text())

    def test_details_table_groups_unknown_new_and_offline(self):
        warnings = []
        found = installer.parse_details('Study\tPlay:1\t192.0.2.1\tStudy\tStudy,Sonos Port\n'
                                        'Sonos Port\tPort\t192.0.2.2\tStudy\tStudy,Sonos Port\n'
                                        'Kitchen\t-\t-\t-\t-\ninvalid\n', warnings.append)
        states = {'Study': ('unit', True, True), 'Sonos Port': ('unit', False, True),
                  'Offline': ('unit', False, False), 'Kitchen': ('unit', False, False)}
        installer.room_table(found, {'Study': True, 'Sonos Port': True, 'Offline': False}, states, self.out)
        text = self.out.getvalue()
        self.assertIn('Sonos rooms found: 3, offline: 1', text)
        self.assertIn('coordinator: Study+Sonos Port', text)
        self.assertIn('member of Study', text)
        self.assertRegex(text, r'Kitchen +- +- +- +new')
        self.assertRegex(text, r'Offline +.*offline, no')
        self.assertIn('yes, stopped', text)
        header = next(line for line in text.splitlines() if line.startswith('Room '))
        self.assertTrue(header.endswith('Bridge to LMS'))
        for name, value in [('Kitchen', 'new, not bridged'), ('Study', 'yes, running'),
                            ('Sonos Port', 'yes, stopped'), ('Offline', 'offline, no')]:
            row = next(line for line in text.splitlines() if line.startswith(name + ' '))
            self.assertEqual(row.index(value), header.index('Bridge to LMS'))
        self.assertEqual(len(warnings), 1)

    def test_plan_build_server_force_stop_and_nothing(self):
        runner = Commands('Study\n')
        self.config.write_text('LMS_SERVER=host\nroom.Study=yes\n')
        self.run_install(runner)
        self.assertEqual(len(runner.actions()), 1)
        for kind in ('same', 'different', 'binary', 'missing', 'server', 'forced', 'stop'):
            runner.calls.clear(); self.out = io.StringIO()
            args = ()
            if kind == 'different': runner.build = 'build-b'
            if kind == 'binary':
                stamp = self.config_dir / 'installed-build'
                stamp.write_text(stamp.read_text().replace('sha256=', 'sha256=changed'))
            if kind == 'missing': (self.config_dir / 'installed-build').unlink()
            if kind == 'server': args = ('--server=other',)
            if kind == 'forced': args = ('--restart',)
            if kind == 'stop': self.config.write_text('LMS_SERVER=other\nroom.Study=no\n')
            self.run_install(runner, args)
            text = self.out.getvalue()
            if kind == 'same':
                self.assertIn('Keep:    Study', text)
                self.assertIn('Nothing to do.', text)
                self.assertFalse(runner.actions())
                self.assertIn('LMS player "Study (Sonos)" connected', text)
            elif kind == 'stop':
                self.assertIn('Stop:    Study', text)
                self.assertEqual([a[1] for a in runner.actions()], ['stop', 'disable'])
            else:
                self.assertEqual([a[1] for a in runner.actions()], ['restart'])
                expected = 'forced' if kind == 'forced' else 'LMS server changed' if kind == 'server' else 'new build build-b'
                self.assertIn(expected, text)
                self.assertIn('; playback stops briefly', text)

    def test_installed_build_atomic_and_service_failure(self):
        runner = Commands('Study\n')
        self.config.write_text('LMS_SERVER=host\nroom.Study=yes\n')
        self.run_install(runner)
        stamp = self.config_dir / 'installed-build'
        original = stamp.read_text()
        self.assertEqual(original, installer.build_identity(ROOT, runner)[1])
        runner.build = 'next'
        def failing(args, **kwargs):
            if args[:2] == ['systemctl', 'restart']:
                raise subprocess.CalledProcessError(1, args)
            return runner(args, **kwargs)
        with self.assertRaises(subprocess.CalledProcessError): self.run_install(failing)
        self.assertEqual(stamp.read_text(), original)
        replace = os.replace
        writes = []
        def observe(source, dest):
            if dest == stamp:
                self.assertEqual(stamp.read_text(), original)
                self.assertEqual(Path(source).parent, stamp.parent)
                self.assertIn('sha256=', Path(source).read_text())
                writes.append(dest)
            return replace(source, dest)
        with patch.object(installer.os, 'replace', side_effect=observe): self.run_install(runner)
        self.assertEqual(writes, [stamp])
        self.assertEqual(stamp.read_text(), installer.build_identity(ROOT, runner)[1])
        self.assertFalse(list(self.config_dir.glob('.install.*')))

    def test_keep_existing_configuration_and_new_room(self):
        runner = Commands('Study\nSonos Port\n')
        self.config.write_text('LMS_SERVER=host\nroom.Study=yes\nroom.Sonos Port=no\n')
        self.run_install(runner)
        runner.calls.clear()
        self.interactive('\n', runner)
        text = self.out.getvalue()
        self.assertIn('Keep this configuration? [Y/n]', text)
        self.assertNotIn('LMS server [', text)
        self.assertNotIn('Bridge Sonos room', text)
        self.assertNotIn('Apply?', text)
        self.assertIn('Nothing to do.', text)
        self.assertFalse(runner.actions())
        runner.rooms += 'Kitchen\n'
        self.interactive('\n', runner)
        text = self.out.getvalue()
        self.assertIn('new, not bridged', text)
        self.assertNotIn('Bridge Sonos room', text)
        self.assertNotIn('Apply?', text)
        self.assertIn('room.Kitchen=no', self.config.read_text())
        self.assertIn('room.Study=yes', self.config.read_text())
        self.assertFalse(runner.actions())

    def test_decline_keep_and_reconfigure_ask_every_room(self):
        runner = Commands('Study\nSonos Port\n')
        self.config.write_text('LMS_SERVER=host\nroom.Study=yes\nroom.Sonos Port=no\n')
        self.interactive('n\nnew.host\ny\nn\n\n', runner)
        text = self.out.getvalue()
        self.assertIn('Keep this configuration? [Y/n]', text)
        self.assertIn('LMS server [host]', text)
        self.assertEqual(text.count('Bridge Sonos room'), 2)
        self.assertIn('Apply?', text)
        self.assertIn('LMS_SERVER=new.host', self.config.read_text())
        self.assertIn('room.Sonos Port=yes', self.config.read_text())
        self.assertIn('room.Study=no', self.config.read_text())
        self.interactive('\n\n\n', runner, ('--reconfigure',))
        self.assertEqual(self.out.getvalue().count('Bridge Sonos room'), 2)
        self.assertNotIn('Keep this configuration?', self.out.getvalue())

    def test_lms_sources_and_mismatch(self):
        runner = Commands('Study\n'); runner.server = 'found'
        self.run_install(runner)
        self.assertIn('LMS server: found (discovery)', self.out.getvalue())
        self.config.write_text('LMS_SERVER=saved\nroom.Study=no\n')
        self.out = io.StringIO()
        self.run_install(runner)
        self.assertIn('LMS server: saved (config)', self.out.getvalue())
        self.assertIn('LMS config is saved; discovery found found', self.err.getvalue())
        self.out = io.StringIO(); self.run_install(runner, ('--server=override',))
        self.assertIn('LMS server: override (--server)', self.out.getvalue())
        self.config.unlink(); runner.server = ''; self.out = io.StringIO()
        self.run_install(runner)
        self.assertIn('LMS server: - (none)', self.out.getvalue())

    def test_lms_cli_encoding_timeout_unreachable_and_shared_deadline(self):
        response = 'players 0 999 count%3A2 playerindex%3A0 name%3ASonos%20Port%20%28Sonos%29 connected%3A1 playerindex%3A1 name%3AStudy%20%28Sonos%29 connected%3A0'
        self.assertEqual(installer.connected_players(response), {'Sonos Port (Sonos)'})
        self.assertEqual(installer.connected_players('players 0 999 playerindex:0 name:A%2BB%3A%20C connected:1'), {'A+B: C'})
        class Socket:
            def __enter__(self): return self
            def __exit__(self, *args): pass
            def settimeout(self, value): pass
            def sendall(self, value): assert value == b'players 0 999\n'
            def recv(self, size): return (response + '\n').encode()
        start = time.monotonic()
        checks, skipped = installer.check_lms('host', ['Sonos Port', 'Study', 'Other'], timeout=0.03,
                                             connect=lambda *a, **k: Socket())
        self.assertLess(time.monotonic() - start, 0.3)
        self.assertFalse(skipped)
        self.assertIn('connected', checks['Sonos Port'])
        self.assertIn('not seen after 0.03 s', checks['Study'])
        self.assertIn('not seen after 0.03 s', checks['Other'])
        def unreachable(*args, **kwargs): raise OSError('connection refused')
        self.assertIn('port 9090 unreachable', installer.check_lms('host', ['Study'], connect=unreachable)[1])
        self.assertEqual(installer.check_lms('', ['Study'])[1], 'no LMS host known')
        # Even a resolver ignoring socket timeout cannot hold the caller up.
        def stalled(*args, **kwargs):
            time.sleep(0.1)
            raise OSError('resolver stalled')
        start = time.monotonic()
        installer.check_lms('host', ['Study'], timeout=0.02, connect=stalled)
        self.assertLess(time.monotonic() - start, 0.08)

    def test_real_tty_session_and_yes(self):
        for case in ('first', 'same', 'new', 'yes'):
            base = Path(self.temp.name) / case
            master, slave = pty.openpty()
            if case == 'first': os.write(master, b'pasted-follow-up-command\n')
            process = subprocess.Popen([sys.executable, str(Path(__file__).resolve()),
                                        '--pty-child', str(base), case],
                                       stdin=slave, stdout=slave, stderr=slave)
            os.close(slave)
            transcript = b''
            answered_through = 0
            prompts = [(b'LMS server [lms.example]: ', b'\n')]
            if case == 'first':
                prompts = [(b'LMS server [lms.example]: ', b'sudo make install\n'),
                           (b'LMS server [lms.example]: ', b'\n')]
                prompts += [(b'Bridge Sonos room "Kitchen" to LMS? [y/N] ', b'n\n'),
                            (b'Bridge Sonos room "Sonos Port" to LMS? [y/N] ', b'y\n'),
                            (b'Bridge Sonos room "Study" to LMS? [y/N] ', b'y\n'),
                            (b'Apply? [Y/n] ', b'\n')]
            elif case in ('same', 'new'):
                prompts = [(b'Keep this configuration? [Y/n] ', b'\n')]
            else: prompts = []
            deadline = time.monotonic() + 10
            try:
                while time.monotonic() < deadline:
                    if select.select([master], [], [], 0.1)[0]:
                        try: chunk = os.read(master, 65536)
                        except OSError: break
                        if not chunk: break
                        transcript += chunk
                        if prompts and prompts[0][0] in transcript[answered_through:]:
                            prompt, answer = prompts.pop(0)
                            answered_through = transcript.index(prompt, answered_through) + len(prompt)
                            os.write(master, answer)
                    elif process.poll() is not None: break
                self.assertEqual(process.wait(timeout=1), 0, transcript.decode())
                self.assertFalse(prompts)
            finally:
                if process.poll() is None: process.kill(); process.wait()
                os.close(master)
            text = transcript.decode().replace('\r\n', '\n')
            Path('/tmp/sonos-plan-' + case + '.txt').write_text(text)
            self.assertIn(installer.LMS_COMMENT + 'LMS_SERVER=lms.example', text)
            self.assertIn('room.Sonos Port=yes', text)
            if case == 'first':
                self.assertIn('Rejected "sudo make install": LMS server must be a host or IP without port or spaces.', text)
                self.assertNotIn('Rejected "pasted-follow-up-command"', text)
            if case == 'same':
                self.assertNotIn('Bridge Sonos room', text)
                self.assertNotIn('Apply?', text)
                self.assertNotIn('Restart:', text)
                self.assertIn('Keep:    Sonos Port, Study', text)
                self.assertIn('Nothing to do.', text)
            if case in ('new', 'yes'):
                self.assertIn('Restart: Sonos Port, Study (new build build-b); playback stops briefly', text)
                self.assertIn('room.MBR=no', text)
                self.assertNotIn('Apply?', text)
            if case == 'yes':
                self.assertNotIn('Apply?', text)
                self.assertIn('New rooms added as no: MBR', text)
            else:
                self.assertNotIn('New rooms added as no:', text)

    def test_enter_keeps_found_server_and_room_defaults(self):
        runner = Commands(); runner.server = 'found.example'
        self.interactive('\n\nYES\nNO\nY\n', runner)
        self.assertIn('LMS_SERVER=found.example', self.config.read_text())
        self.assertIn('room.Kitchen=no', self.config.read_text())
        self.assertIn('room.Sonos Port=yes', self.config.read_text())
        self.assertIn('room.Study=no', self.config.read_text())


def dry_run_report():
    with tempfile.TemporaryDirectory(prefix='sonos-installer-dry-run-') as temp:
        config_dir = Path(temp) / 'etc/sonos-lms'
        config_dir.mkdir(parents=True)
        (config_dir / 'config').write_text('LMS_SERVER=lms.example\n')
        (config_dir / 'rooms').write_text('Study\nSonos Port\n')
        out, errors = io.StringIO(), io.StringIO()
        installer.install(ROOT, config_dir, Path(temp) / 'etc/systemd/system', [], Commands(), out, errors, lms_check=mock_lms)
        report = ('Mocked discovery: Study, Sonos Port, Kitchen\n'
                  'Existing rooms file: Study, Sonos Port\n\n'
                  'Config produced:\n' + (config_dir / 'config').read_text()
                  + '\nInstaller summary (temporary test path shown literally):\n' + out.getvalue())
        Path('/tmp/sonos-installer-dry-run.txt').write_text(report)
        print(report, end='')
        assert not errors.getvalue(), errors.getvalue()


if __name__ == '__main__':
    if len(sys.argv) > 1 and sys.argv[1] == '--pty-child':
        base, case = Path(sys.argv[2]), sys.argv[3]
        config_dir, unit_dir = base / 'etc/sonos-lms', base / 'etc/systemd/system'
        runner = Commands(); runner.server = 'lms.example'
        if case != 'first':
            config_dir.mkdir(parents=True)
            (config_dir / 'config').write_text(installer.LMS_COMMENT + 'LMS_SERVER=lms.example\n' + installer.ROOM_HEADER +
                                             'room.Kitchen=no\nroom.Sonos Port=yes\nroom.Study=yes\n')
            (config_dir / 'installed-build').write_text(installer.build_identity(ROOT, runner)[1])
            unit_dir.mkdir(parents=True)
            (unit_dir / 'sonos-lms@.service').write_text((ROOT / 'packaging/sonos-lms@.service').read_text())
            runner.active = {r'sonos-lms@Sonos\x20Port.service', 'sonos-lms@Study.service'}
            runner.enabled = set(runner.active)
        if case in ('new', 'yes'):
            runner.build = 'build-b'
            runner.rooms += 'MBR\n'
        runner.details = ('Study\tPlay:1\t192.0.2.1\tStudy\tStudy,Sonos Port\n'
                          'Sonos Port\tPort\t192.0.2.2\tStudy\tStudy,Sonos Port\n'
                          'Kitchen\t-\t192.0.2.3\tKitchen\tKitchen\n')
        if case in ('new', 'yes'):
            runner.details += 'MBR\tOne\t192.0.2.4\tMBR\tMBR\n'
        installer.install(ROOT, config_dir, unit_dir, ['--yes'] if case == 'yes' else [],
                          runner, lms_check=mock_lms)
        if case == 'same': assert not runner.actions(), runner.actions()
        if case in ('new', 'yes'):
            assert len([a for a in runner.actions() if a[1] == 'restart']) == 2
        print('Resulting config:')
        print((base / 'etc/sonos-lms/config').read_text(), end='')
        raise SystemExit(0)
    suite = unittest.defaultTestLoader.loadTestsFromTestCase(InstallerTests)
    result = unittest.TextTestRunner(verbosity=2).run(suite)
    if not result.wasSuccessful(): raise SystemExit(1)
    for test in unittest.defaultTestLoader.getTestCaseNames(InstallerTests):
        print('PASS: installer ' + test.removeprefix('test_'))
    dry_run_report()
