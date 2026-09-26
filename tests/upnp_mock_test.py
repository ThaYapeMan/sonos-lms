"""Loopback speaker: capture actual noson bytes, then test own control and bridge polling."""
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
import os
import subprocess
import tempfile
import threading
import time
import xml.etree.ElementTree as ET
from xml.sax.saxutils import escape

ROOT = Path(__file__).resolve().parents[1]
SOAP = 'http://schemas.xmlsoap.org/soap/envelope/'
class Speaker(BaseHTTPRequestHandler):
    protocol_version = 'HTTP/1.1'
    def log_message(self, *args): pass
    def send(self, data, status=200):
        data = data.encode()
        self.send_response(status)
        self.send_header('Content-Length', str(len(data)))
        self.send_header('Connection', 'close')
        self.end_headers()
        self.wfile.write(data)
    def do_POST(self):
        try:
            self.handle_post()
        except (BrokenPipeError, ConnectionResetError):
            pass  # deadline tests intentionally close before the response
        except Exception as error:
            self.server.errors.append(error)
            print("mock speaker error:", repr(error), flush=True)
            self.close_connection = True
    def handle_post(self):
        body = self.rfile.read(int(self.headers['Content-Length']))
        if self.path == '/slow':
            time.sleep(.3)
            self.send('too late')
            return
        if self.path in ('/chunked', '/bad-chunk'):
            self.send_response(200)
            self.send_header('Transfer-Encoding', 'chunked')
            self.send_header('Connection', 'close')
            self.end_headers()
            self.wfile.write(b'5;foo=bar\r\nhello\r\n6\r\n world\r\n0\r\nX-Trailer: yes\r\n\r\n'
                            if self.path == '/chunked' else b'xyz\r\nx\r\n0\r\n\r\n')
            return
        if self.path == '/truncated':
            self.send_response(200)
            self.send_header('Content-Length', '100')
            self.send_header('Connection', 'close')
            self.end_headers()
            self.wfile.write(b'short')
            return
        action = self.headers['SOAPAction'].strip('"').split('#')[1]
        self.server.counts[action] = self.server.counts.get(action, 0) + 1
        count = self.server.counts[action]
        self.server.requests.append((action, body))
        node = ET.fromstring(body).find(f'{{{SOAP}}}Body')[0]
        service = 'ZoneGroupTopology' if action == 'GetZoneGroupState' else 'RenderingControl' if action == 'GetVolume' else 'AVTransport'
        assert node.tag == f'{{urn:schemas-upnp-org:service:{service}:1}}{action}'
        assert self.path == ('/ZoneGroupTopology/Control' if service == 'ZoneGroupTopology'
                             else '/MediaRenderer/RenderingControl/Control' if service == 'RenderingControl' else '/MediaRenderer/AVTransport/Control')
        if service == 'AVTransport':
            assert node.findtext('InstanceID') == '0'
            if action in self.server.golden:
                assert body == self.server.golden[action], action
        if action == 'SetAVTransportURI' and self.server.mode in ('control', 'golden'):
            assert body == (ROOT / 'tests/fixtures/noson-set-uri.xml').read_bytes()
        if action == 'Play':
            assert node.findtext('Speed') == '1'
            if self.server.mode in ('control', 'delayed-play') and count == 1: time.sleep(6)
            if self.server.mode == 'timeout-playing': time.sleep(21)
        if action == 'SetAVTransportURI': self.server.current_uri = node.findtext('CurrentURI')
        values = ''
        fault = self.server.mode == 'control' and (action == 'Pause' or action == 'Play' and count == 2)
        if fault:
            if action == 'Pause': time.sleep(.2)
            self.send(f'<s:Envelope xmlns:s="{SOAP}"><s:Body><s:Fault><faultcode>s:Client</faultcode>'
                      '<detail><UPnPError><errorCode>701</errorCode><errorDescription>Transition not available</errorDescription>'
                      '</UPnPError></detail></s:Fault></s:Body></s:Envelope>', 500)
            return
        if action == 'GetZoneGroupState':
            topology = (ROOT / 'tests/fixtures/topology.xml').read_text()
            if self.server.mode == 'control' and count >= 3:
                topology = topology.replace('Coordinator="RINCON_00112233445501400"',
                                            'Coordinator="RINCON_66778899AABB01400"')
            values = '<ZoneGroupState>' + escape(topology) + '</ZoneGroupState>'
        if action == 'GetTransportInfo':
            state = 'PLAYING'
            if self.server.mode == 'poll':
                state = {3: 'PAUSED_PLAYBACK', 4: 'TRANSITIONING'}.get(count, 'PLAYING')
            values = f'<CurrentTransportState>{state}</CurrentTransportState><CurrentTransportStatus>OK</CurrentTransportStatus>'
        if action == 'GetPositionInfo': values = '<RelTime>0:02:03</RelTime><TrackDuration>0:04:56</TrackDuration><TrackMetaData>' + escape('<DIDL-Lite><item><dc:title xmlns:dc="urn:dc">Title from speaker</dc:title></item></DIDL-Lite>') + '</TrackMetaData>'
        if action == 'GetVolume':
            assert node.findtext('Channel') == 'Master'
            values = '<CurrentVolume>37</CurrentVolume>'
        if action == 'GetMediaInfo':
            uri = self.server.current_uri if self.server.mode == 'timeout-playing' else 'http://external/?a=1&b=2'
            values = '<CurrentURI>' + escape(uri) + '</CurrentURI><TrackURI>wrong</TrackURI>'
        self.send(f'<s:Envelope xmlns:s="{SOAP}"><s:Body><u:{action}Response '
                  f'xmlns:u="urn:schemas-upnp-org:service:{service}:1">{values}</u:{action}Response></s:Body></s:Envelope>')


def run(mode, command, golden=None):
    server = ThreadingHTTPServer(('127.0.0.1', 0), Speaker)
    server.mode, server.counts, server.requests, server.errors, server.golden = mode, {}, [], [], golden or {}
    thread = threading.Thread(target=server.serve_forever)
    thread.start()
    try:
        result = subprocess.run([*command, str(server.server_port)], cwd=ROOT, check=True, timeout=40, capture_output=True, text=True)
        print(result.stdout, end='')
        server.output = result.stdout
        assert not server.errors, server.errors
        return server
    finally:
        server.shutdown()
        server.server_close()
        thread.join()

# The fixture's --all form accepts the port last, like the other executables.
reference = run('golden', [str(ROOT / 'noson-golden'), '--all'])
golden = dict(reference.requests)
assert set(golden) == {'SetAVTransportURI', 'Play', 'Pause', 'Stop', 'GetTransportInfo', 'GetPositionInfo', 'GetMediaInfo'}
control = run('control', [str(ROOT / 'own-control-test')], golden)
assert control.counts['GetPositionInfo'] == 3
assert control.counts['GetVolume'] == 2
assert control.counts['GetTransportInfo'] == 3
assert control.counts['GetZoneGroupState'] == 3
for action, body in control.requests:
    if action in golden: assert body == golden[action]
print('PASS: all seven own AVTransport request bodies match actual noson SOAP bytes')

source = (ROOT / 'sonos-lms.cpp').read_text()
def production_function(signature):
    start = source.index(signature)
    opening = source.index('{', start)
    depth, end = 1, opening + 1
    while depth:
        depth += (source[end] == '{') - (source[end] == '}')
        end += 1
    return source[start:end]

with tempfile.TemporaryDirectory(prefix='sonos-play-timeout-') as temp:
    temp = Path(temp)
    (temp / 'production_play_timeout.inc').write_text('\n'.join(production_function(s) for s in (
        'static bool alreadyPlayingCurrentStream(unsigned stream)\n',
        'static bool PlaySqueezeBoxLocked(unsigned stream_id, bool resetPosition)\n',
        'static void dispatchStreamStart(')))
    executable = temp / 'play-timeout-test'
    subprocess.run(['g++', '-O2', '-Wall', '-Wextra', '-I', str(ROOT), '-I', str(temp),
                    str(ROOT / 'tests/play_timeout_fixture.cpp'),
                    *[str(ROOT / ('upnp/' + name + '.cpp')) for name in ('own_speaker_control', 'xml', 'soap', 'http', 'discovery')],
                    '-lpthread', '-o', str(executable)], check=True)
    for mode in ('delayed-play', 'timeout-playing'):
        tested = run(mode, [str(executable)])
        assert tested.counts['Play'] == tested.counts['SetAVTransportURI'] == 1
        assert tested.output.count('PlaySqueezeBox: title=') == 1
        if mode == 'timeout-playing':
            assert 'UPnP Play failed: timeout (HTTP 0)' in tested.output
            assert 'PlayStream(stream 7): device already playing current stream; no retry' in tested.output
        else:
            assert 'failed' not in tested.output
        print(f'PASS: {mode}: one PlaySqueezeBox, one SetAVTransportURI, one Play; no track restart')

with tempfile.TemporaryDirectory(prefix='sonos-own-poll-') as temp:
    temp = Path(temp)
    (temp / 'production_own_poll.inc').write_text('\n'.join(production_function(s) for s in (
        'static void ObserveDeviceTransport(', 'void ResumeSqueezeBox(', 'void refreshStatus(')))
    executable = temp / 'poll-test'
    subprocess.run(['g++', '-O2', '-Wall', '-Wextra', '-I', str(ROOT), '-I', str(temp),
                    str(ROOT / 'tests/own_poll_fixture.cpp'), str(ROOT / 'sonos-status.cpp'),
                    *[str(ROOT / ('upnp/' + name + '.cpp')) for name in ('own_speaker_control', 'xml', 'soap', 'http', 'discovery')],
                    '-lpthread', '-o', str(executable)], check=True)
    run('poll', [str(executable)])
    settings = temp / 'settings.cpp'
    settings.write_text('''#include "upnp/backend.h"
#include <cassert>
int main(int argc, char** argv) {
    const auto expected = argv[1][0] == '1' ? upnp::Backend::Own : upnp::Backend::Noson;
    assert(upnp::backend() == expected);
    setenv("SONOS_LMS_UPNP", expected == upnp::Backend::Own ? "noson" : "own", 1);
    assert(upnp::backend() == expected);
}''')
    subprocess.run(['g++', '-I', str(ROOT), str(settings), '-o', str(temp / 'settings')], check=True)
    for value in (None, 'noson', 'own', '', 'invalid'):
        env = dict(os.environ)
        env.pop('SONOS_LMS_UPNP', None)
        if value is not None: env['SONOS_LMS_UPNP'] = value
        result = subprocess.run([str(temp / 'settings'), str(int(value == 'own'))], env=env,
                                check=True, capture_output=True, text=True)
        assert result.stdout.count('UPnP layer:') == 1
        assert result.stdout.count('Warning:') == int(value in ('', 'invalid'))
    print('PASS: SONOS_LMS_UPNP defaults, validation and read-once startup logging')
