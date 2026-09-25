import re
import subprocess

result = subprocess.run(['./streamer-test', 'send-error'], check=True,
                        capture_output=True, text=True)
print(result.stdout, end='')
lines = [line for line in result.stdout.splitlines() if 'send to Sonos failed' in line]
assert len(lines) == 1, lines
assert re.fullmatch(r'stream 1: send to Sonos failed after \d+\.\d s '
                    r'\(Connection reset by peer; errno=\d+; 0 bytes confirmed sent, '
                    r'partial failed write uncounted\)', lines[0]), lines
print('PASS: one send-error log includes errno, reason, bytes and elapsed seconds')
