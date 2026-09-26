#!/usr/bin/env python3
"""SOAP/CLI checks for device-test.sh; standard library only, no bridge state."""
import argparse
import json
from pathlib import Path
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
             read_soap=soap, read_lms=lms_fields, startup=10):
    deadline = clock() + startup
    phase = f'clocks did not start increasing within {startup:g} s'
    first = last = baseline = None

    def measured():
        def pair(value):
            return 'unavailable' if value is None else f'speaker {value[0]:g}s, LMS {value[1]:g}s'
        return f'initial [{pair(first)}]; measuring from [{pair(baseline)}]; last [{pair(last)}]'

    def sample():
        remaining = deadline - clock()
        if remaining <= 0:
            raise TimeoutError('sampling deadline expired')
        position = reltime(read_soap(host, 'GetPositionInfo', timeout=min(1, remaining)))
        remaining = deadline - clock()
        if remaining <= 0:
            raise TimeoutError('sampling deadline expired')
        lms_time = float(read_lms(lms, port, player, timeout=min(1, remaining))['time'])
        return position, lms_time

    increasing = [False, False]
    last_error = ''
    while clock() < deadline:
        try:
            current = sample()
            if first is None:
                first = last = current
            else:
                previous, last = last, current
                # A reset on resume is not forward progress.
                increasing = [False if b < a else seen or b > a
                              for seen, a, b in zip(increasing, previous, last)]
                if clock() <= deadline and all(increasing):
                    baseline = last
                    break
        except Exception as error:
            last_error = str(error)
        sleep(min(.25, max(0, deadline - clock())))
    if baseline is None:
        raise RuntimeError(f'{phase}; {measured()}; last read error: {last_error or "none"}')

    deadline = clock() + window
    last_error = ''
    while clock() < deadline:
        sleep(min(.25, max(0, deadline - clock())))
        if clock() >= deadline:
            break
        try:
            last = sample()
            if clock() <= deadline and all(b - a >= 3 for a, b in zip(baseline, last)):
                return f'audio continues: {measured()}'
        except Exception as error:
            last_error = str(error)
    raise RuntimeError(f'audio did not advance 3 s within {window:g} s after both clocks started; '
                       f'{measured()}; last read error: {last_error or "none"}')


def status_sample(host, read_soap):
    record = {'timestamp': time.time()}
    try:
        fields = read_soap(host, 'GetTransportInfo')
        record.update(state=fields['CurrentTransportState'], status=fields['CurrentTransportStatus'])
    except Exception as error:
        record['error'] = str(error)
    return record


def check_status_log(text):
    if not text.strip():
        raise RuntimeError('cannot check: empty GetTransportInfo sample log')
    errors = []
    pending = []
    tolerated = 0
    for number, line in enumerate(text.splitlines(), 1):
        try:
            record = json.loads(line)
            error = str(record.get('error', ''))
            timeout = 'timed out' in error.lower() or 'timeout' in error.lower()
            if timeout and record.get('status', 'OK') == 'OK':
                # Consecutive busy reads share the first timeout's recovery deadline.
                pending.append((number, record.get('timestamp')))
                continue
            valid = not error and record.get('status') == 'OK' and bool(record.get('state'))
            if not valid:
                detail = error or ("missing state" if not record.get("state") else "")
                errors.append(f'sample {number}: CurrentTransportStatus={record.get("status", "missing")} {detail}')
            if pending:
                times = [stamp for _, stamp in pending] + [record.get('timestamp')]
                timely = all(isinstance(stamp, (int, float)) for stamp in times) and all(
                    0 <= later - times[0] <= 7 for later in times[1:])
                if valid and timely:
                    tolerated += len(pending)
                else:
                    errors.append(f'sample {pending[0][0]}: SOAP timeout without valid OK recovery within 7 s')
                pending = []
        except (ValueError, AttributeError):
            errors.append(f'cannot check sample {number}: invalid record {line!r}')
    if pending:
        errors.append(f'sample {pending[0][0]}: SOAP timeout at end of log without recovery')
    if errors:
        raise RuntimeError('; '.join(errors))
    summary = f'{len(text.splitlines())} GetTransportInfo samples: status OK'
    if tolerated:
        summary += (f'; {tolerated} SOAP timeout{"s" if tolerated != 1 else ""} tolerated '
                    '(speaker busy on stream request)')
    return summary


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('operation', choices=['coordinator', 'command', 'progress', 'monitor', 'state', 'sample', 'status-log'])
    parser.add_argument('--host')
    parser.add_argument('--file')
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
    elif args.operation == 'sample':
        print(json.dumps(status_sample(args.host, soap)), flush=True)
    elif args.operation == 'status-log':
        print(check_status_log(Path(args.file).read_text()))
    elif args.operation == 'progress':
        print(progress(args.host, args.lms, args.port, args.player))
    else:
        while True:
            print(json.dumps(status_sample(args.host, soap)), flush=True)
            time.sleep(.5)


if __name__ == '__main__':
    try:
        main()
    except Exception as error:
        import sys
        print(str(error), file=sys.stderr)
        sys.exit(1)
