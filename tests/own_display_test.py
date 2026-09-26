"""Production own control with a SOAP boundary stub; no sockets or devices."""
from pathlib import Path
import subprocess
import tempfile
ROOT = Path(__file__).resolve().parents[1]
with tempfile.TemporaryDirectory(prefix='sonos-own-display-') as directory:
    executable = Path(directory) / 'test'
    subprocess.run(['g++', '-O2', '-Wall', '-Wextra', '-I', str(ROOT),
                    str(ROOT / 'tests/own_display_fixture.cpp'),
                    *[str(ROOT / ('upnp/' + name + '.cpp')) for name in
                      ('gena', 'own_speaker_control', 'soap', 'xml', 'discovery')],
                    '-lpthread', '-o', str(executable)], check=True)
    result = subprocess.run([str(executable)], capture_output=True, text=True, check=True)
    print(result.stdout, end='')
    message = 'UPnP GetPositionInfo: no reply while speaker holds a stream request (paused)'
    assert result.stdout.count(message) == 2, result.stdout  # two distinct pauses
    assert 'failed' not in result.stdout, result.stdout
