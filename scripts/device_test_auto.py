#!/usr/bin/env python3
"""SOAP/CLI checks for device-test.sh; standard library only, no bridge state."""
import argparse
import socket
import time
import urllib.error
import urllib.request
import xml.etree.ElementTree as ET
from urllib.parse import unquote

SERVICE = 'urn:schemas-upnp-org:service:AVTransport:1'


def soap_body(action):
    extra = '<Speed>1</Speed>' if action == 'Play' else ''
    return (f'<?xml version="1.0" encoding="utf-8"?>'
            f'<s:Envelope xmlns:s="http://schemas.xmlsoap.org/soap/envelope/" '
            f's:encodingStyle="http://schemas.xmlsoap.org/soap/encoding/">'
            f'<s:Body><u:{action} xmlns:u="{SERVICE}"><InstanceID>0</InstanceID>'
            f'{extra}</u:{action}></s:Body></s:Envelope>').encode()


def soap(host, action, timeout=5):
    request = urllib.request.Request(f'http://{host}:1400/MediaRenderer/AVTransport/Control',
                                     soap_body(action),
                                     {'Content-Type': 'text/xml; charset="utf-8"',
                                      'SOAPACTION': f'"{SERVICE}#{action}"'})
    try:
        with urllib.request.urlopen(request, timeout=timeout) as response:
            body = response.read()
    except urllib.error.HTTPError as error:
        body = error.read()
        raise RuntimeError(f'{action}: HTTP {error.code}: {body.decode(errors="replace")}') from error
    root = ET.fromstring(body)
    fields = {element.tag.rsplit('}', 1)[-1]: element.text or '' for element in root.iter()}
    if 'Fault' in fields:
        raise RuntimeError(f'{action}: {fields.get("errorCode")} {fields.get("errorDescription")}')
    return fields


def coordinator(details, room):
    rows = {}
    for line in details.splitlines():
        fields = line.split('\t')
        if len(fields) == 5:
            rows[fields[0]] = fields
    if room not in rows:
        raise ValueError(f'room {room!r} not discovered')
    row = rows[room]
    name = row[3]
    # An unknown coordinator is not safe for an unattended transport command.
    if name == '-' or name not in rows or rows[name][2] == '-':
        raise ValueError(f'coordinator address unavailable for {room!r}')
    return rows[name][2]


def lms_fields(host, port, player, timeout=1):
    with socket.create_connection((host, port), timeout=timeout) as connection:
        connection.sendall(f'{player} status - 1 tags:a\n'.encode())
        with connection.makefile('rb') as reply:
            line = reply.readline(65536).decode()
    # Decode tokens after splitting: encoded spaces belong to the value.
    result = {}
    for token in line.split():
        key, sep, value = unquote(token).partition(':')
        if sep and key not in result:
            result[key] = value
    return result


def reltime(fields):
    h, m, s = map(float, fields['RelTime'].split(':'))
    return h * 3600 + m * 60 + s


def progress(host, lms, port, player, window=6, clock=time.monotonic, sleep=time.sleep,
             read_soap=soap, read_lms=lms_fields):
    deadline = clock() + window
    def sample():
        remaining = deadline - clock()
        if remaining <= 0:
            raise TimeoutError('six-second progress window expired')
        position = reltime(read_soap(host, 'GetPositionInfo', timeout=min(1, remaining)))
        remaining = deadline - clock()
        if remaining <= 0:
            raise TimeoutError('six-second progress window expired')
        lms_time = float(read_lms(lms, port, player, timeout=min(1, remaining))['time'])
        return position, lms_time
    start = sample()
    last = start
    while clock() < deadline:
        sleep(min(.25, max(0, deadline - clock())))
        if clock() >= deadline:
            break
        last = sample()
        if clock() <= deadline and all(b - a >= 3 for a, b in zip(start, last)):
            return f'audio continues: speaker {start[0]:g}->{last[0]:g}s; LMS {start[1]:g}->{last[1]:g}s'
    raise RuntimeError(f'audio did not advance 3 s within 6 s: speaker {start[0]:g}->{last[0]:g}; LMS {start[1]:g}->{last[1]:g}')


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('operation', choices=['coordinator', 'command', 'progress', 'monitor', 'state'])
    parser.add_argument('--host')
    parser.add_argument('--room')
    parser.add_argument('--action', choices=['Pause', 'Play'])
    parser.add_argument('--lms')
    parser.add_argument('--port', type=int, default=9090)
    parser.add_argument('--player')
    args = parser.parse_args()
    if args.operation == 'coordinator':
        import sys
        print(coordinator(sys.stdin.read(), args.room))
    elif args.operation == 'command':
        soap(args.host, args.action, timeout=20)
    elif args.operation == 'state':
        fields = soap(args.host, 'GetTransportInfo')
        if fields.get('CurrentTransportStatus') != 'OK':
            raise RuntimeError(f'CurrentTransportStatus={fields.get("CurrentTransportStatus", "missing")}')
        print(fields['CurrentTransportState'])
    elif args.operation == 'progress':
        print(progress(args.host, args.lms, args.port, args.player))
    else:
        while True:
            try:
                status = soap(args.host, 'GetTransportInfo')['CurrentTransportStatus']
                if status != 'OK':
                    print(f'CurrentTransportStatus={status}', flush=True)
            except Exception as error:
                print(f'GetTransportInfo: {error}', flush=True)
            time.sleep(.5)


if __name__ == '__main__':
    try:
        main()
    except Exception as error:
        import sys
        print(str(error), file=sys.stderr)
        sys.exit(1)
