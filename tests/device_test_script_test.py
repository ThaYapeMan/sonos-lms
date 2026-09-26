"""Exercise device-test host selection with fake unit and journal output."""
import os
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
script = (ROOT / "scripts/device-test.sh").read_text()
helpers = script[script.index("lms_from_exec_start() {"):script.index("# ------------------------------------------------------------------ main ---")]
assert "SCENARIOS=${SCENARIOS:-1 2 3 4 5 6 7}" in script
harness = r'''
set -uo pipefail
have() { return 0; }
mark() { printf '%s\n' "$*" >&2; return 1; } # logger failure must not erase a valid host
systemctl() {
    [[ $# == 4 && $1 == show && $2 == "$UNIT" && $3 == -p && $4 == ExecStart ]] || exit 97
    printf 'unit\n' >> "$CALLS"
    printf '%s\n' "$EXEC_START"
}
journalctl() {
    [[ $1 == -u && $2 == "$UNIT" ]] || exit 98
    if [[ $* == *'-n 500'* ]]; then
        printf 'recent\n' >> "$CALLS"
        printf '%s\n' "$RECENT"
    else
        [[ $# == 5 && $3 == --no-pager && $4 == -o && $5 == cat ]] || exit 99
        printf 'full\n' >> "$CALLS"
        printf '%s\n' "$FULL"
    fi
}
if resolve_lms_host "$CONFIG"; then
    printf '%s|%s\n' "$LMS" "$LMS_SOURCE"
else
    exit 1
fi
'''
with tempfile.TemporaryDirectory(prefix="sonos-device-script-") as tmp:
    config = Path(tmp) / "config"
    calls = Path(tmp) / "calls"
    cases = [
        ("explicit override", "override", "LMS_SERVER=config", "recent", "--server=unit", "full", "override|LMS override", []),
        ("config", "", "LMS_SERVER=config", "recent", "--server=unit", "full", "config|config LMS_SERVER", []),
        ("recent journal", "", "", "recent", "--server=unit", "full", "recent|recent unit journal", ["recent"]),
        ("unit equals", "", "", "", "--server=192.168.178.23", "full", "192.168.178.23|unit ExecStart --server", ["recent", "unit"]),
        ("unit separate", "", "", "", "--room Study --server lms.local", "full", "lms.local|unit ExecStart --server", ["recent", "unit"]),
        ("unit quoted host", "", "", "", '--server="lms.local"', "full", "lms.local|unit ExecStart --server", ["recent", "unit"]),
        ("full journal", "", "", "", "--room Study", "old-lms", "old-lms|full unit journal", ["recent", "unit", "full"]),
        ("missing server value", "", "", "", "--server --room Study", "old-lms", "old-lms|full unit journal", ["recent", "unit", "full"]),
        ("unknown host", "", "", "", "--room Study", "", None, ["recent", "unit", "full"]),
    ]
    for label, override, contents, recent, unit, full, expected, sources in cases:
        config.write_text(contents + "\n")
        calls.write_text("")
        env = {**os.environ, "LMS": override, "UNIT": "sonos-lms@Study Room.service",
               "CONFIG": str(config), "CALLS": str(calls),
               "EXEC_START": "ExecStart={ path=/bridge ; argv[]=/bridge " + unit + " ; ignore_errors=no ; }",
               "RECENT": "LMS server from discovery: " + recent if recent else "unrelated status output",
               "FULL": "LMS server from discovery: obsolete\nLMS server from discovery: " + full if full else "unrelated status output"}
        result = subprocess.run(["bash", "-c", helpers + harness], env=env, capture_output=True, text=True)
        assert result.returncode == (0 if expected else 1), (label, result.stderr)
        assert result.stdout.strip() == (expected or ""), (label, result.stdout)
        assert calls.read_text().splitlines() == sources, (label, calls.read_text())
        if expected:
            host, source = expected.split("|")
            assert f"LMS host {host} (source: {source})" in result.stderr
        print(f"PASS: device-test LMS {label}, fallback order and source logging")
subprocess.run(["bash", "-n", str(ROOT / "scripts/device-test.sh")], check=True)
print("PASS: device-test syntax and default scenarios 1–7")

# Source the actual script's functions, stopping before its physical-device entry point.
import importlib.util
import sys
from unittest.mock import patch
sys.dont_write_bytecode = True
spec = importlib.util.spec_from_file_location('device_auto', ROOT / 'scripts/device_test_auto.py')
auto = importlib.util.module_from_spec(spec)
spec.loader.exec_module(auto)
prefix = script[:script.index('# ------------------------------------------------------------------ main ---')]
with tempfile.TemporaryDirectory(prefix='sonos-auto-fixture-') as directory:
    env = {**os.environ, 'OUT': directory, 'AUTO': '1', 'QUICK': '0'}
    def run_shell(body, **overrides):
        return subprocess.run(['bash', '-c', prefix + '\n' + body],
                              env={**env, **overrides}, capture_output=True, text=True)
    journal = ('Stream session: abc\nCreating new stream (21)\n'
               'PlaySqueezeBox: title=\'A real track\' art=\'\'\n'
               '| Title  A real track |\nspeaker URI: stream=21 session=abc\n')
    for extra, expected in [('', '21'), ('speaker URI: other spotify\n', ''),
                            ('speaker URI: stream=22 session=old\n', ''),
                            ('Stream session: restarted\n', '')]:
        result = run_shell('journal_tail() { printf "%s" "$JOURNAL"; }; speaker_stream', JOURNAL=journal+extra)
        assert result.returncode == 0 and result.stdout.strip() == expected, result
    result = run_shell('journal_tail() { printf "%s" "$JOURNAL"; }; speaker_stream',
                       JOURNAL='| Title  squeezebox.flac?session=abc&stream=99 |\n')
    assert result.stdout.strip() == ''
    print('PASS: device-test speaker URI parser uses real-title journal, clears external/stale/restarted streams')

    for mode, quick, expected in [('0', '0', '1 2 3 4 5 6 7|3|120'), ('1', '0', '1 2 5 6 7|1|30'), ('0', '1', '1 2 5 6 7|1|30')]:
        result = run_shell('printf "%s|%s|%s" "$SCENARIOS" "$S2_ROUNDS" "$LONG_PAUSE"', AUTO=mode, QUICK=quick)
        assert result.stdout == expected, result
    result = run_shell('printf "%s|%s|%s" "$SCENARIOS" "$S2_ROUNDS" "$LONG_PAUSE"', SCENARIOS='7', S2_ROUNDS='2', LONG_PAUSE='90')
    assert result.stdout == '7|2|90'
    print('PASS: device-test AUTO/QUICK defaults and explicit overrides; manual defaults unchanged')

    result = run_shell('''
mark() { printf '=== %s\n' "$*"; }
python3() { printf '%s\n' "$*" >> "$OUT/commands"; }
auto_progress() { printf 'progress checked\n'; }
COORDINATOR_IP=192.0.2.7
prompt 'S2.1 press PAUSE in the Sonos app'
prompt 'S2.1 press PLAY in the Sonos app'
observe 'S2: error dialog?'
''')
    assert result.returncode == 0, result.stderr
    assert '=== AUTO device PAUSE sent' in result.stdout and '=== AUTO device PLAY sent' in result.stdout
    commands = (Path(directory)/'commands').read_text()
    assert 'command --host 192.0.2.7 --action Pause' in commands
    assert 'command --host 192.0.2.7 --action Play' in commands
    assert 'progress checked' in result.stdout and 'OBSERVED' not in result.stdout
    print('PASS: device-test AUTO prompts issue coordinator Pause/Play without reading stdin')

    for fail, code in [(False, 0), (True, 1)]:
        body = 'auto_record 1\n'
        if fail: body += 'auto_fail "CurrentTransportStatus=ERROR_NO_PLAYABLE_CONTENT"\n'
        body += 'auto_record 2\nAUTO_REASONS=()\nauto_record 5\nauto_record 6\nauto_record 7\nauto_summary\nexit "$AUTO_FAILED"'
        result = run_shell(body)
        assert result.returncode == code, result
        assert 'S1 | PASS |' in result.stdout and 'S5 | PASS |' in result.stdout
        assert ('S2 | FAIL | CurrentTransportStatus=ERROR_NO_PLAYABLE_CONTENT' in result.stdout) == fail
        if fail:
            Path('/tmp/sonos-auto-summary.txt').write_text(result.stdout)
            print(result.stdout, end='')
    print('PASS: device-test AUTO summary exit 0 only when every scenario passes; recovery retains failure')

    # Run the actual main driver with all external I/O stubbed and closed stdin.
    # This exercises capture, scenario dispatch, summary and the EXIT trap together.
    main = script[script.index('# ------------------------------------------------------------------ main ---'):]
    stubs = r'''
have() { [[ $1 == python3 ]]; }
resolve_lms_host() { LMS=fixture; }
cli_raw() { printf 'title:Track%%20B\n'; }
resolve_track() { printf 1; }
start_capture() { :; }
snapshot() { :; }
mark() { printf '=== %s\n' "$*"; }
lms() { :; }
finish() { auto_summary; }
python3() { printf '192.0.2.10\n'; }
./sonos-lms() { :; }
auto_begin() { AUTO_REASONS=(); }
auto_end() { auto_record "$1"; }
scenario_1() { prompt 'S1 press PAUSE in the Sonos app'; observe 'S1 question'; }
scenario_2() { if [[ $FIXTURE_FAIL == 1 ]]; then auto_fail 'fixture error'; fi; }
'''
    # EUID is readonly; tests may be run by an ordinary developer, so omit only the root gate.
    main = main.replace('[[ $EUID -eq 0 ]] || fail "run as root (tcpdump and journal access)"', ':')
    for fail in ('0', '1'):
        result = run_shell(stubs + main, PLAYER='fixture', SCENARIOS='1 2', FIXTURE_FAIL=fail)
        assert result.returncode == int(fail), (result.stdout, result.stderr)
        assert 'scenario | PASS/FAIL | reason' in result.stdout
        assert 'Press Enter' not in result.stderr and '???' not in result.stderr
    print('PASS: device-test AUTO main driver and exit trap never prompt and preserve summary exit codes')

# SOAP bodies and destination are tested at the HTTP boundary, without network I/O.
import xml.etree.ElementTree as ET
for action in ('Pause', 'Play'):
    captured = []
    class Reply:
        def __enter__(self): return self
        def __exit__(self, *args): pass
        def read(self): return b'<Envelope><Body><Response/></Body></Envelope>'
    def request(req, timeout):
        captured.append((req, timeout))
        return Reply()
    with patch.object(auto.urllib.request, 'urlopen', request): auto.soap('192.0.2.10', action, 20)
    req, timeout = captured[0]
    assert req.full_url == 'http://192.0.2.10:1400/MediaRenderer/AVTransport/Control'
    assert req.get_header('Soapaction') == f'"{auto.SERVICE}#{action}"'
    root = ET.fromstring(req.data)
    command = root.find('{http://schemas.xmlsoap.org/soap/envelope/}Body')[0]
    assert command.tag == f'{{{auto.SERVICE}}}{action}'
    assert [(node.tag, node.text) for node in command] == [('InstanceID', '0')] + ([('Speed', '1')] if action == 'Play' else [])
    assert timeout == 20
print('PASS: device-test AUTO Pause/Play SOAP bodies, InstanceID, Speed, action headers and coordinator endpoint')
details = 'Study\tPlay:1\t192.0.2.10\tStudy\tStudy,Sonos Port\nSonos Port\tPort\t192.0.2.11\tStudy\tStudy,Sonos Port\n'
assert auto.coordinator(details, 'Sonos Port') == '192.0.2.10'
try: auto.coordinator('Unknown\t-\t-\t-\t-\n', 'Unknown')
except ValueError: pass
else: raise AssertionError('unknown coordinator accepted')
print('PASS: device-test AUTO resolves group coordinator, spaces in names, rejects unknown address')

class Clock:
    now = 0
    def clock(self): return self.now
    def sleep(self, seconds): self.now += seconds
for advance in (True, False):
    clock = Clock()
    def read_soap(*args, **kwargs): return {'RelTime': f'0:00:{clock.now if advance else 0}'}
    def read_lms(*args, **kwargs): return {'time': str(clock.now)}
    try:
        message = auto.progress('speaker', 'lms', 9090, 'player', clock=clock.clock, sleep=clock.sleep,
                                read_soap=read_soap, read_lms=read_lms)
    except RuntimeError as error:
        assert not advance and 'did not start increasing' in str(error)
    else: assert advance and 'audio continues' in message
    assert clock.now <= 16
print('PASS: device-test AUTO requires both clocks advance 3 s within 6 s, rejects stalled speaker')

with tempfile.TemporaryDirectory(prefix='sonos-uri-fixture-') as directory:
    executable = Path(directory) / 'test'
    subprocess.run(['g++', '-O2', '-Wall', '-I', str(ROOT),
                    str(ROOT / 'tests/speaker_uri_test.cpp'), str(ROOT / 'sonos-status.cpp'),
                    '-lcrypto', '-o', str(executable)], check=True)
    result = subprocess.run([str(executable)], capture_output=True, text=True, check=True)
    lines = result.stdout.splitlines()
    assert len(lines) == 3, lines
    assert lines[0].startswith('speaker URI: stream=21 session=')
    assert lines[1:] == ['speaker URI: other spotify', 'speaker URI: other unknown']
    print(result.stdout, end='')
print('PASS: bridge speaker URI log changes only, independent of title, shared by both adapters')

# Production scenario completion must reject both SOAP status and journal errors.
with tempfile.TemporaryDirectory(prefix='sonos-auto-end-') as directory:
    env = {**os.environ, 'OUT': directory, 'AUTO': '1'}
    for status, journal, expected in [('OK', '', 0), ('ERROR_LOST_CONNECTION', '', 1),
                                       ('OK', 'UPnP ERROR_NO_PLAYABLE_CONTENT', 1)]:
        body = r'''
have() { return 1; }
COORDINATOR_IP=192.0.2.10
UNIT=fixture
AUTO_SINCE=now
AUTO_STATUS_FILE="$OUT/status"
printf '{"timestamp":1,"state":"PLAYING","status":"%s"}\n' "$STATUS" > "$AUTO_STATUS_FILE"
python3() {
    if [[ $2 == sample ]]; then
        printf '{"timestamp":2,"state":"PLAYING","status":"%s"}\n' "$STATUS"
    else
        command python3 "$REAL_HELPER" "${@:2}"
    fi
}
journalctl() { printf '%s' "$JOURNAL"; }
sleep 60 & AUTO_MONITOR_PID=$!
auto_end 7
auto_summary
exit "$AUTO_FAILED"
'''
        result = subprocess.run(['bash', '-c', prefix + body], capture_output=True, text=True,
                                env={**env, 'STATUS': status, 'JOURNAL': journal, 'REAL_HELPER': str(ROOT / 'scripts/device_test_auto.py')})
        assert result.returncode == expected, result
        if expected: assert (status if status != 'OK' else journal) in result.stdout
print('PASS: device-test AUTO completion rejects non-OK transport text and scenario journal ERROR_*')

# Run the actual Python state operation with mocked SOAP, including error text.
for status in ('OK', 'ERROR_NO_PLAYABLE_CONTENT'):
    import io
    from contextlib import redirect_stdout
    with patch.object(sys, 'argv', ['auto', 'state', '--host', 'fixture']), \
         patch.object(auto, 'soap', return_value={'CurrentTransportState': 'PLAYING', 'CurrentTransportStatus': status}):
        if status == 'OK':
            output = io.StringIO()
            with redirect_stdout(output): auto.main()
            assert output.getvalue() == 'PLAYING\n'
        else:
            try: auto.main()
            except RuntimeError as error: assert status in str(error)
            else: raise AssertionError('bad transport status accepted')
print('PASS: device-test AUTO SOAP state check reports CurrentTransportStatus text')

with tempfile.TemporaryDirectory(prefix='sonos-auto-mark-') as directory:
    result = subprocess.run(['bash', '-c', prefix + r'''
have() { return 1; }
mark 'S2.1 POSITION RESTARTED: paused at 12s, now 0s'
mark 'S5a AUTO-START FAIL: speaker=STOPPED'
mark 'S1 CHECK OK: track title=FAILED'
auto_record 2
auto_summary
exit "$AUTO_FAILED"
'''], env={**os.environ, 'AUTO': '1', 'OUT': directory}, capture_output=True, text=True)
    assert result.returncode == 1, result
    assert 'POSITION RESTARTED' in result.stdout and 'AUTO-START FAIL' in result.stdout
    assert 'title=FAILED' not in result.stdout
print('PASS: device-test AUTO records position/setup failures without misclassifying track titles')

with tempfile.TemporaryDirectory(prefix='sonos-auto-s5-') as directory:
    body = r'''
have() { return 1; }
ROOM=Study; UNIT=fixture; PLAYER=fixture
TRACK_A_ID=1; TRACK_B_ID=2; TRACK_A_NAME=A; TRACK_B_NAME=B; TRACK_B_TITLE=B
fixture_state=PLAYING; fixture_stream=21
setup_playing_a() { return 0; }
prompt() { fixture_state=STOPPED; }
sonos_state() { printf '%s' "$fixture_state"; }
wait_s() { :; }; sleep() { :; }; snapshot() { :; }
auto_progress() { :; }
play_track() { fixture_state=PLAYING; fixture_stream=$NEW_STREAM; }
cli_raw() { printf 'title:%s\n' "$NEW_TITLE"; }
journal_tail() {
    printf "Creating new stream (21)\nPlaySqueezeBox: title='A' art=''\n"
    printf "Creating new stream (22)\nPlaySqueezeBox: title='B' art=''\n"
    printf 'speaker URI: stream=%s session=fixture\n' "$fixture_stream"
}
scenario_5
auto_record 5
auto_summary
exit "$AUTO_FAILED"
'''
    for stream, title, expected in [('22', 'B', 0), ('21', 'B', 1), ('22', 'A', 1)]:
        result = subprocess.run(['bash', '-c', prefix+body], capture_output=True, text=True,
                                env={**os.environ, 'AUTO': '1', 'OUT': directory,
                                     'NEW_STREAM': stream, 'NEW_TITLE': title})
        assert result.returncode == expected, (result.stdout, result.stderr)
print('PASS: device-test AUTO S5 requires a new speaker stream and LMS track B title')

# A fake systemd version returns stale records only when -n/--grep are used.
with tempfile.TemporaryDirectory(prefix='sonos-journal-reader-') as directory:
    base = Path(directory)
    journalctl = base / 'journalctl'
    journalctl.write_text(r'''#!/bin/bash
printf '%s\n' "$*" >> "$CALLS"
case "$*" in
    *--grep*|*' -n '*|*'-5min'*)
        printf "Stream session: old\nCreating new stream (1) for Sonos\nPlaySqueezeBox: title='Old title' art=''\nspeaker URI: stream=1 session=old\n" ;;
    *)
        printf "noise\nCreating new stream (2) for Sonos\nspeaker URI: stream=2 session=old\nStream session: fresh\nCreating new stream (5) for Sonos\nPlaySqueezeBox: title='Current title' art=''\nspeaker URI: stream=5 session=fresh\n" ;;
esac
''')
    journalctl.chmod(0o755)
    calls = base / 'calls'
    env = {**os.environ, 'OUT': directory, 'AUTO': '1', 'UNIT': 'sonos-lms@Study.service',
           'PATH': directory + os.pathsep + os.environ['PATH'], 'CALLS': str(calls)}
    stale = subprocess.run(['journalctl', '-u', env['UNIT'], '-n', '600', '--grep=Creating'],
                           env=env, capture_output=True, text=True, check=True)
    assert 'stream (1)' in stale.stdout and 'stream (5)' not in stale.stdout
    calls.write_text('')
    body = r'''
START_EPOCH=1700000000; JOURNAL_SINCE="@$START_EPOCH"
mark() { printf '%s\n' "$*"; }
journal_tail
printf 'bridge=%s speaker=%s\n' "$(bridge_stream)" "$(speaker_stream)"
journal_self_check
JOURNAL_SINCE="-5min" journal_tail
'''
    result = subprocess.run(['bash', '-c', prefix+body], env=env, capture_output=True, text=True)
    assert result.returncode == 0, result.stderr
    assert 'bridge=5 speaker=5' in result.stdout and 'self-check OK' in result.stdout
    assert 'Stream session: fresh' in result.stdout and 'Stream session: old' in result.stdout
    recorded = calls.read_text().splitlines()
    assert all('--grep' not in line and ' -n ' not in line for line in recorded), recorded
    assert all('--since @1700000000' in line or '--since -5min' in line for line in recorded), recorded
    result = subprocess.run(['bash', '-c', prefix + r'''
s=1
journal_tail() { echo 'Creating new stream (1) for Sonos'; }
journal_self_check
echo SHOULD_NOT_CONTINUE
'''], env=env, capture_output=True, text=True)
    assert result.returncode == 1 and 'SHOULD_NOT_CONTINUE' not in result.stdout
    assert 'Journal reader self-check failed' in result.stderr and 'stream (5)' in result.stderr
    assert 'JOURNAL_SINCE="-5min" snapshot' in script
print('PASS: AUTO journal reader uses run epoch/local grep, startup -5min, session resets and aborts on stale reader')

# The speaker restarts at zero; LMS does not start until four seconds later.
for stalled in (False, True):
    clock = Clock()
    def speaker(*args, **kwargs):
        return {'RelTime': f'0:00:{0 if stalled else clock.now}'}
    def late_lms(*args, **kwargs):
        return {'time': str(23.5498 + max(0, clock.now - 4))}
    try:
        message = auto.progress('speaker', 'lms', 9090, 'player', clock=clock.clock,
                                sleep=clock.sleep, read_soap=speaker, read_lms=late_lms)
    except RuntimeError as error:
        assert stalled and clock.now == 10, (clock.now, error)
        message = str(error)
    else:
        assert not stalled and 7 <= clock.now <= 10
    assert 'speaker' in message and 'LMS' in message and '23.5498' in message
    print(message)
print('PASS: AUTO progress tolerates late LMS start, rejects stalled speaker after 10 s, reports measured values')

# Transient startup read failures consume the startup budget, not the measurement window.
clock = Clock()
def delayed_read(*args, **kwargs):
    if clock.now < 2: raise TimeoutError('held GET')
    return {'RelTime': f'0:00:{clock.now}'}
message = auto.progress('speaker', 'lms', 9090, 'player', clock=clock.clock, sleep=clock.sleep,
                        read_soap=delayed_read, read_lms=lambda *a, **k: {'time': str(clock.now)})
assert 'audio continues' in message and clock.now <= 16
# Both start, then the speaker stalls during the measurement window.
clock = Clock()
try:
    auto.progress('speaker', 'lms', 9090, 'player', clock=clock.clock, sleep=clock.sleep,
                  read_soap=lambda *a, **k: {'RelTime': f'0:00:{min(clock.now, .5)}'},
                  read_lms=lambda *a, **k: {'time': str(clock.now)})
except RuntimeError as error:
    assert 'did not advance 3 s within 6 s' in str(error) and clock.now <= 16
else: raise AssertionError('stalled measurement accepted')
print('PASS: AUTO progress retries startup reads and still enforces the following six-second advance window')

import json
samples = io.StringIO()
with patch.object(sys, 'argv', ['auto', 'monitor', '--host', 'fixture']), \
     patch.object(auto, 'soap', side_effect=[
         {'CurrentTransportState': 'PLAYING', 'CurrentTransportStatus': 'OK'},
         {'CurrentTransportState': 'STOPPED', 'CurrentTransportStatus': 'ERROR_NO_PLAYABLE_CONTENT'},
         OSError('no reply')]), \
     patch.object(auto.time, 'sleep', side_effect=[None, None, KeyboardInterrupt]), redirect_stdout(samples):
    try: auto.main()
    except KeyboardInterrupt: pass
records = [json.loads(line) for line in samples.getvalue().splitlines()]
assert len(records) == 3 and all('timestamp' in record for record in records)
assert records[0]['status'] == 'OK' and records[0]['state'] == 'PLAYING'
assert records[2]['error'] == 'no reply'
assert '1 GetTransportInfo samples: status OK' == auto.check_status_log(json.dumps(records[0]))
for contents, diagnostic in [('', 'cannot check'), (samples.getvalue(), 'ERROR_NO_PLAYABLE_CONTENT'),
                              ('not json', 'invalid record')]:
    try: auto.check_status_log(contents)
    except RuntimeError as error: assert diagnostic in str(error)
    else: raise AssertionError('missing or bad samples passed')
def status_log(*records):
    return '\n'.join(json.dumps(record) for record in records)


def timeout_sample(timestamp):
    return {'timestamp': timestamp, 'error': 'timed out'}


def ok_sample(timestamp):
    return {'timestamp': timestamp, 'state': 'PLAYING', 'status': 'OK'}


assert '1 SOAP timeout tolerated' in auto.check_status_log(status_log(timeout_sample(10), ok_sample(15.5)))
assert '2 SOAP timeouts tolerated' in auto.check_status_log(status_log(timeout_sample(10), timeout_sample(12), ok_sample(17)))
print('PASS: AUTO tolerates one or consecutive SOAP timeouts with OK recovery within 7 s of the first timeout')
for records in [
    (timeout_sample(10), ok_sample(17.1)),
    (ok_sample(10), timeout_sample(11)),
    (timeout_sample(10), dict(ok_sample(11), status='ERROR_NO_PLAYABLE_CONTENT')),
    (timeout_sample(10), timeout_sample(12), ok_sample(18)),
    (dict(timeout_sample(10), error='connection refused'), ok_sample(11)),
    (timeout_sample(10), dict(ok_sample(11), state='')),
]:
    try:
        auto.check_status_log(status_log(*records))
    except RuntimeError:
        pass
    else:
        raise AssertionError(f'Invalid timeout recovery passed: {records}')
print('PASS: AUTO rejects late, trailing, non-OK, missing-state and non-timeout failures')

print('PASS: AUTO monitor records timestamped OK/state/error samples; empty and malformed logs cannot pass')

with tempfile.TemporaryDirectory(prefix='sonos-empty-status-') as directory:
    result = subprocess.run(['bash', '-c', prefix + r'''
COORDINATOR_IP=fixture; UNIT=fixture; AUTO_SINCE=now
AUTO_STATUS_FILE="$OUT/status"; : > "$AUTO_STATUS_FILE"
python3() {
    if [[ $2 == sample ]]; then printf '{"timestamp":1,"state":"PLAYING","status":"OK"}\n';
    else command python3 "$REAL_HELPER" "${@:2}"; fi
}
journalctl() { :; }; mark() { :; }
sleep 60 & AUTO_MONITOR_PID=$!
auto_end 1
auto_summary
exit "$AUTO_FAILED"
'''], env={**os.environ, 'AUTO': '1', 'OUT': directory,
           'REAL_HELPER': str(ROOT / 'scripts/device_test_auto.py')}, capture_output=True, text=True)
    assert result.returncode == 1, result
    assert 'cannot check: empty GetTransportInfo sample log' in result.stdout
print('PASS: AUTO empty monitor log fails scenario even when final synchronous sample is OK')

with tempfile.TemporaryDirectory(prefix='sonos-discovery-backend-') as directory:
    for fallback in ('0', '1'):
        calls = Path(directory) / 'calls'; calls.write_text('')
        result = subprocess.run(['bash', '-c', prefix + r'''
ROOM=Study
./sonos-lms() {
    printf '%s %s\n' "$SONOS_LMS_UPNP" "$*" >> "$CALLS"
    [[ $FALLBACK != 1 || $SONOS_LMS_UPNP != yeney ]] || return 2
    printf 'Study\tPlay:1\t192.0.2.10\tStudy\tStudy\n'
}
python3() { command python3 "$REAL_HELPER" "${@:2}"; }
discover_coordinator
'''], env={**os.environ, 'OUT': directory, 'FALLBACK': fallback, 'CALLS': str(calls),
           'REAL_HELPER': str(ROOT / 'scripts/device_test_auto.py')}, capture_output=True, text=True)
        assert result.returncode == 0 and result.stdout.strip() == '192.0.2.10', result
        assert calls.read_text().splitlines() == ['yeney --list-rooms --details'] + (
            ['noson --list-rooms --details'] if fallback == '1' else [])
print('PASS: AUTO discovery explicitly selects yeney, uses noson only after yeney failure')
