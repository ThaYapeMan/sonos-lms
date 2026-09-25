"""Installer merge and apply tests; all paths are temporary and systemctl is mocked."""
from pathlib import Path
import importlib.util
import io
import os
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
        self.calls = []
        self.active, self.enabled, self.missing = set(), set(), set()

    def __call__(self, args, **kwargs):
        self.calls.append(args)
        if args == ['./sonos-lms', '--list-rooms']:
            assert kwargs['cwd'] == ROOT
            return subprocess.CompletedProcess(args, self.discovery_status, self.rooms,
                                               'No Sonos rooms found.' if self.discovery_status else '')
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
        self.assertEqual(self.config.read_text(), installer.ROOM_HEADER
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
        self.assertFalse(self.config.exists())
        self.assertIn('Room discovery failed', self.err.getvalue())
        self.assertIn('No rooms configured.', self.out.getvalue())

    def test_failure_still_accepts_explicit_overrides(self):
        self.config.write_text('room.Study=no\n')
        self.run_install(Commands('', 2), ('Study', '--server=example'))
        self.assertEqual(self.config.read_text(), 'room.Study=yes\nLMS_SERVER=example\n')

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
        self.assertEqual(self.config.read_text(), 'room.Study=no\n')
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
    suite = unittest.defaultTestLoader.loadTestsFromTestCase(InstallerTests)
    result = unittest.TextTestRunner(verbosity=2).run(suite)
    if not result.wasSuccessful(): raise SystemExit(1)
    dry_run_report()
