"""Exercise the actual CLI with each discovery backend against a loopback speaker."""
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
import os
import subprocess
import threading
from xml.sax.saxutils import escape

ROOT = Path(__file__).resolve().parents[1]
SOAP = 'http://schemas.xmlsoap.org/soap/envelope/'

class Speaker(BaseHTTPRequestHandler):
    protocol_version = 'HTTP/1.1'

    def log_message(self, *args): pass

    def do_SUBSCRIBE(self):
        # Force noson's existing SOAP topology fallback; no callback to a real device.
        self.send_response(412)
        self.send_header('Content-Length', '0')
        self.send_header('Connection', 'close')
        self.end_headers()

    do_UNSUBSCRIBE = do_SUBSCRIBE

    def do_GET(self):
        models = {'/study.xml': '<displayName>Play:1</displayName><modelName>Sonos Play:1</modelName>',
                  '/port.xml': '<modelName>Port</modelName>'}
        body = ('<root><device>' + models.get(self.path, '') + '</device></root>').encode()
        self.send_response(200)
        self.send_header('Content-Length', str(len(body)))
        self.send_header('Connection', 'close')
        self.end_headers()
        self.wfile.write(body)

    def do_POST(self):
        self.rfile.read(int(self.headers['Content-Length']))
        action = self.headers['SOAPAction'].strip('"').split('#')[1]
        self.server.actions.append(action)
        values = ''
        service = 'DeviceProperties'
        if action == 'GetZoneGroupState':
            service = 'ZoneGroupTopology'
            values = '<ZoneGroupState>' + escape(self.server.topology) + '</ZoneGroupState>'
        elif action == 'ListAvailableServices':
            service = 'MusicServices'
            values = '<AvailableServiceDescriptorList>&lt;Services/&gt;</AvailableServiceDescriptorList>'
        elif action == 'GetHouseholdID':
            values = '<CurrentHouseholdID>test</CurrentHouseholdID>'
        elif action == 'GetZoneInfo':
            values = '<SerialNumber>test</SerialNumber><SoftwareVersion>1</SoftwareVersion>'
        body = (f'<s:Envelope xmlns:s="{SOAP}"><s:Body><u:{action}Response '
                f'xmlns:u="urn:schemas-upnp-org:service:{service}:1">{values}'
                f'</u:{action}Response></s:Body></s:Envelope>').encode()
        self.send_response(200)
        self.send_header('Content-Length', str(len(body)))
        self.send_header('Connection', 'close')
        self.end_headers()
        self.wfile.write(body)

# Use a distinct loopback address so a local listener on 127.0.0.1:1400 is harmless.
server = ThreadingHTTPServer(('127.0.0.8', 1400), Speaker)
thread = threading.Thread(target=server.serve_forever)
thread.start()
try:
    topology = (ROOT / 'tests/fixtures/topology.xml').read_text()
    duplicate = ('<ZoneGroup Coordinator="DUPLICATE" ID="duplicate"><ZoneGroupMember '
                 'UUID="DUPLICATE" ZoneName="Study" Location="http://127.0.0.8:1400/xml/device_description.xml"/>'
                 '</ZoneGroup>')
    topology = topology.replace('</ZoneGroups>', duplicate + '</ZoneGroups>')
    for backend in ('own', 'noson', None):
        for empty in (False, True):
            server.topology = '<ZoneGroupState><ZoneGroups/></ZoneGroupState>' if empty else topology
            server.actions = []
            env = dict(os.environ)
            env.pop('SONOS_LMS_UPNP', None)
            if backend is not None: env['SONOS_LMS_UPNP'] = backend
            result = subprocess.run([str(ROOT / 'sonos-lms'), '--list-rooms', '--ip=127.0.0.8'],
                                    env=env, capture_output=True, text=True, timeout=30)
            assert result.returncode == (2 if empty else 0), result
            assert result.stdout == ('' if empty else 'Living & Dining\nSonos Port\nStudy\n'), result
            if empty: assert 'No Sonos rooms found.' in result.stderr, result
            assert f'UPnP layer: {backend or "noson"}' in result.stderr
            assert 'Stream session:' not in result.stderr and 'SONOS_LMS_PAUSE=' not in result.stderr
            assert 'GetZoneGroupState' in server.actions, server.actions
            assert set(server.actions) <= {'GetZoneGroupState', 'GetHouseholdID', 'GetZoneInfo', 'ListAvailableServices'}, server.actions
            print(f'PASS: --list-rooms backend={backend or "default"} empty={empty}: exact sorted unique rooms, group member, --ip, exit status and clean stdout')
    topology = (ROOT / 'tests/fixtures/topology.xml').read_text()
    for ip, path in (('1', 'study'), ('2', 'port'), ('3', 'unknown'), ('4', 'bonded')):
        topology = topology.replace(f'127.0.0.{ip}:1400/xml/device_description.xml', f'127.0.0.8:1400/{path}.xml')
    topology = topology.replace('ZoneName="Study"/>', 'ZoneName="Study"><Satellite UUID="SUB" ZoneName="Sub" Location="http://127.0.0.8:1400/sub.xml"/></ZoneGroupMember>')
    expected = ('Living & Dining\t-\t127.0.0.8\tLiving & Dining\tLiving & Dining\n'
                'Sonos Port\tPort\t127.0.0.8\tStudy\tStudy,Sonos Port\n'
                'Study\tPlay:1\t127.0.0.8\tStudy\tStudy,Sonos Port\n')
    for backend in ('own', 'noson'):
        server.topology = topology
        result = subprocess.run([str(ROOT / 'sonos-lms'), '--list-rooms', '--details', '--ip=127.0.0.8'],
                                env=dict(os.environ, SONOS_LMS_UPNP=backend), capture_output=True, text=True, timeout=30)
        assert result.returncode == 0 and result.stdout == expected, result
        print(f'PASS: --list-rooms --details backend={backend}: model fallback, unknown model, groups, room device and satellites')
    Path('/tmp/sonos-room-details.txt').write_text(result.stdout)

finally:
    server.shutdown()
    server.server_close()
    thread.join()
