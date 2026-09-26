import re
import subprocess
import time

result = subprocess.run(['./streamer-test', 'send-error'], check=True,
                        capture_output=True, text=True)
print(result.stdout, end='')
lines = [line for line in result.stdout.splitlines() if 'send to Sonos failed' in line]
assert len(lines) == 1, lines
assert re.fullmatch(r'stream 1: send to Sonos failed after \d+\.\d s '
                    r'\(Connection reset by peer; errno=\d+; 0 bytes confirmed sent, '
                    r'partial failed write uncounted\)', lines[0]), lines
print('PASS: one send-error log includes errno, reason, bytes and elapsed seconds')

result = subprocess.run(['./streamer-test', 'send-error-real'], check=True,
                        capture_output=True, text=True)
print(result.stdout, end='')
lines = [line for line in result.stdout.splitlines() if 'send to Sonos failed' in line]
assert len(lines) == 1 and 'Broken pipe; errno=' in lines[0], lines
assert '0 bytes confirmed sent' in lines[0], lines
print('PASS: real kernel socket failure preserves errno in exactly one diagnostic')

for mode, normal in [('close-pause', True), ('close-stop', True), ('promotion-close', True),
                     ('close-old', False), ('close-unexpected', False)]:
    result = subprocess.run(['./streamer-test', mode], check=True, capture_output=True, text=True)
    print(result.stdout, end='')
    if normal:
        assert 'client closed after' in result.stdout, result.stdout
    else:
        assert 'send to Sonos failed' in result.stdout, result.stdout
        assert 'client closed after' not in result.stdout, result.stdout
    print(f'PASS: send-close classification {mode}: {"normal close" if normal else "unexpected failure"}')

for mode, expected in [('own-close-after', 'client closed after'),
                       ('own-close-before', 'client closed after'),
                       ('own-close-end', 'client closed after'),
                       ('own-close-none', 'send to Sonos failed'),
                       ('own-close-other-stream', 'send to Sonos failed')]:
    started = time.monotonic()
    result = subprocess.run(['./streamer-test', mode], check=True, capture_output=True, text=True)
    if expected == 'send to Sonos failed': assert time.monotonic() - started >= 1.9
    print(result.stdout, end='')
    lines = [line for line in result.stdout.splitlines()
             if 'client closed after' in line or 'send to Sonos failed' in line]
    assert len(lines) == 1 and expected in lines[0], result.stdout
    assert 'Socket cleanup completed without waiting' in result.stdout
    print(f'PASS: deferred close {mode}: exactly one {expected} diagnostic; cleanup immediate')

result = subprocess.run(['./streamer-test', 'own-promotion-close'], check=True, capture_output=True, text=True)
print(result.stdout, end='')
assert result.stdout.count('client closed after') == 2, result.stdout
assert 'send to Sonos failed' not in result.stdout, result.stdout
print('PASS: deferred promotion closes: exactly one normal diagnostic for each of two errors')
