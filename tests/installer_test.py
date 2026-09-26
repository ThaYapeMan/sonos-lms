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
        self.calls = []
        self.active, self.enabled, self.missing = set(), set(), set()

    def __call__(self, args, **kwargs):
        self.calls.append(args)
        if args == ['./sonos-lms', '--list-rooms']:
            assert kwargs['cwd'] == ROOT
            return subprocess.CompletedProcess(args, self.discovery_status, self.rooms,
                                               'No Sonos rooms found.' if self.discovery_status else '')
        if args == ['./sonos-lms', '--find-server']:
            return subprocess.CompletedProcess(args, 0 if self.server else 2, self.server, '')
        if args[0] == 'systemd-escape':
            return subprocess.run(args, **kwargs)
        assert args[0] == 'systemctl', args
        action, unit = args[1], args[-1]
        code, output = 0, ''
        if action == 'is-active': code = 0 if unit in self.active else 3
        elif action == 'is-enabled': code = 0 if unit in self.enabled else 1
        elif action == 'show': output = 'not-found\n' if unit in self.missing else 'loaded\n'
        elif action == 'enable': self.enabled.add(unit); self.active.add(unit)
        elif action == 'restart': self.active.add(unit)
        elif action == 'stop': self.active.discard(unit)
        elif action == 'disable': self.enabled.discard(unit)
        else: assert action == 'daemon-reload', args
        return subprocess.CompletedProcess(args, code, output, '')

    def actions(self):
        return [args for args in self.calls if args[0] == 'systemctl'
                and args[1] in ('enable', 'restart', 'stop', 'disable')]


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
        installer.install(ROOT, self.config_dir, self.unit_dir, args, runner, self.out, self.err)
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
        self.assertEqual(self.out.getvalue().count('New rooms found:'), 3)

    def test_active_not_enabled_and_inactive_enabled(self):
        self.config.write_text('room.Study=yes\nroom.Sonos Port=on\n')
        runner = Commands('Study\nSonos Port\n')
        runner.active.add('sonos-lms@Study.service')
        runner.enabled.add(r'sonos-lms@Sonos\x20Port.service')
        self.run_install(runner)
        self.assertEqual(runner.actions(), [
            ['systemctl', 'enable', '--now', 'sonos-lms@Study.service'],
            ['systemctl', 'restart', 'sonos-lms@Study.service'],
            ['systemctl', 'enable', '--now', r'sonos-lms@Sonos\x20Port.service'],
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
            ['systemctl', 'stop', 'sonos-lms@Kitchen.service'],
            ['systemctl', 'disable', 'sonos-lms@Kitchen.service'],
            ['systemctl', 'enable', '--now', r'sonos-lms@Sonos\x20Port.service'],
            ['systemctl', 'restart', 'sonos-lms@Study.service'],
        ])
        self.assertIn('Study: enabled (running)', self.out.getvalue())
        self.assertIn('Sonos Port: enabled (running)', self.out.getvalue())
        self.assertIn('Kitchen: disabled', self.out.getvalue())
        self.assertIn('New rooms found: Kitchen — set room.Kitchen=yes', self.out.getvalue())
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
        self.assertIn('Kitchen: disabled', self.out.getvalue())

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
                          self.out, self.err, TTY(answers))
        return runner

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
            self.assertNotIn(['./sonos-lms', '--find-server'], runner.calls)
        self.run_install(args=('--server=override',))
        self.assertIn('LMS_SERVER=override', self.config.read_text())

    def test_interactive_answers_validation_defaults_and_offline(self):
        self.config.write_text('# keep\nLMS_SERVER=old\nroom.Study=ON\nroom.Offline= true \nOTHER=unchanged\n')
        self.interactive('bad:9000\nbad host\nbad\x01host\nnew.example\nmaybe\ny\nn\n\n\n')
        self.assertEqual(self.config.read_text(), '# keep\nLMS_SERVER=new.example\nroom.Study=yes\nroom.Offline= true \nOTHER=unchanged\nroom.Kitchen=yes\nroom.Sonos Port=no\n')
        self.assertIn('offline, kept: Offline (yes)', self.out.getvalue())
        self.assertIn('"Study"? [Y/n]', self.out.getvalue())
        self.assertEqual(self.out.getvalue().count('LMS server [old]:'), 4)
        self.assertEqual(self.out.getvalue().count('"Kitchen"? [y/N]'), 2)

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
                self.interactive('\ny\ny\nn\nn\n' if mode == 'decline' else '', runner)
            self.assertEqual(self.config.read_text(), original)
            self.assertTrue(legacy.exists())
            self.assertFalse(self.unit_dir.exists())
            self.assertFalse(any(call[0] == 'systemctl' for call in runner.calls))

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
        self.assertNotIn('"Study"?', self.out.getvalue())
        self.assertIn('room.Study=yes', self.config.read_text())

    def test_real_tty_session_and_yes(self):
        for non_interactive in (False, True):
            self.config.unlink(missing_ok=True)
            master, slave = pty.openpty()
            process = subprocess.Popen([sys.executable, str(Path(__file__).resolve()),
                                        '--pty-child', self.temp.name]
                                       + (['--yes'] if non_interactive else []),
                                       stdin=slave, stdout=slave, stderr=slave)
            os.close(slave)
            transcript = b''
            prompts = [(b'LMS server [lms.example]: ', b'\n'),
                       (b'Activate Sonos room "Kitchen"? [y/N] ', b'n\n'),
                       (b'Activate Sonos room "Sonos Port"? [y/N] ', b'y\n'),
                       (b'Activate Sonos room "Study"? [y/N] ', b'y\n'),
                       (b'Apply? [Y/n] ', b'\n')] if not non_interactive else []
            deadline = time.monotonic() + 10
            try:
                while time.monotonic() < deadline:
                    if select.select([master], [], [], 0.1)[0]:
                        try: chunk = os.read(master, 65536)
                        except OSError: break
                        if not chunk: break
                        transcript += chunk
                        if prompts and prompts[0][0] in transcript:
                            os.write(master, prompts.pop(0)[1])
                    elif process.poll() is not None:
                        break
                self.assertEqual(process.wait(timeout=1), 0)
                self.assertFalse(prompts)
            finally:
                if process.poll() is None:
                    process.kill(); process.wait()
                os.close(master)
            text = transcript.decode().replace('\r\n', '\n')
            Path('/tmp/sonos-installer-' + ('yes' if non_interactive else 'interactive') + '.txt').write_text(text)
            self.assertIn(installer.LMS_COMMENT + 'LMS_SERVER=lms.example', text)
            self.assertIn('room.Sonos Port=' + ('no' if non_interactive else 'yes'), text)
            self.assertIn('room.Study=' + ('no' if non_interactive else 'yes'), text)
            if non_interactive:
                self.assertNotIn('Apply?', text)

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
        installer.install(ROOT, config_dir, Path(temp) / 'etc/systemd/system', [], Commands(), out, errors)
        report = ('Mocked discovery: Study, Sonos Port, Kitchen\n'
                  'Existing rooms file: Study, Sonos Port\n\n'
                  'Config produced:\n' + (config_dir / 'config').read_text()
                  + '\nInstaller summary (temporary test path shown literally):\n' + out.getvalue())
        Path('/tmp/sonos-installer-dry-run.txt').write_text(report)
        print(report, end='')
        assert not errors.getvalue(), errors.getvalue()


if __name__ == '__main__':
    if len(sys.argv) > 1 and sys.argv[1] == '--pty-child':
        base = Path(sys.argv[2])
        runner = Commands(); runner.server = 'lms.example'
        installer.install(ROOT, base / 'etc/sonos-lms', base / 'etc/systemd/system',
                          sys.argv[3:], runner)
        print('Resulting config:')
        print((base / 'etc/sonos-lms/config').read_text(), end='')
        raise SystemExit(0)
    suite = unittest.defaultTestLoader.loadTestsFromTestCase(InstallerTests)
    result = unittest.TextTestRunner(verbosity=2).run(suite)
    if not result.wasSuccessful(): raise SystemExit(1)
    for test in unittest.defaultTestLoader.getTestCaseNames(InstallerTests):
        print('PASS: installer ' + test.removeprefix('test_'))
    dry_run_report()
