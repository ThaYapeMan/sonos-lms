#!/usr/bin/env python3
"""reference-relay.py -- pass a live radio stream through to Sonos and log everything.

Purpose: a known-good reference for how Sonos pauses and resumes a live HTTP
stream. Sonos is pointed at this relay (see reference-test.sh); every GET/HEAD
from Sonos opens its own upstream connection to the real station, so Sonos gets
exactly what a normal radio server sends on each (re)connect. The relay logs:

  - every request from Sonos: method, path, all headers
  - the upstream status/headers and what was sent back to Sonos
  - the first bytes of every response body and their format (fLaC / OggS / MP3 / AAC)
  - how every connection ended (client closed, upstream ended), bytes and duration

HTTPS stations work too: the relay terminates TLS itself, so the capture on the
bridge host stays readable.

Standard library only.

  reference-relay.py --upstream URL [--port 8990] [--content-type TYPE]
  reference-relay.py --file song.flac [--port 8990]   # serve a local FLAC file as live radio
  reference-relay.py --probe --upstream URL     # print content type + format, exit
  reference-relay.py --probe --file song.flac

--file turns the relay into a minimal FLAC radio server: every GET gets the
complete file from the start (fLaC header, metadata, frames), with a 2-second
burst and then paced at the file's own average byte rate, looping. Plain HTTP
body, Connection: close, no chunked encoding -- exactly how the reference MP3
station was served. It answers one question: does Sonos also report a FLAC
radio stream as corrupt on the first Play after a pause, or only ours?
"""

import argparse
import itertools
import socket
import sys
import threading
import time
import urllib.request
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

HOP_BY_HOP = {"connection", "transfer-encoding", "keep-alive", "content-length"}
_ids = itertools.count(1)
_log_lock = threading.Lock()
LAST_CONTENT_TYPE = None


def now():
    t = time.time()
    return time.strftime("%H:%M:%S", time.localtime(t)) + ".%03d" % int((t % 1) * 1000)


def log(msg):
    with _log_lock:
        print(f"{now()} {msg}", flush=True)


def describe(data):
    """Name the stream format from its first bytes."""
    if data.startswith(b"fLaC"):
        return "native FLAC stream header (fLaC)"
    if data.startswith(b"OggS"):
        return "Ogg page (OggS)"
    if data.startswith(b"ID3"):
        return "ID3 tag (MP3)"
    # FLAC frame sync (FF F8/F9) is also a valid MPEG-2 AAC ADTS sync, so test it first
    if len(data) >= 2 and data[0] == 0xFF and data[1] in (0xF8, 0xF9):
        return "FLAC audio frame (no header) or MPEG-2 AAC ADTS"
    if len(data) >= 2 and data[0] == 0xFF and (data[1] & 0xF6) == 0xF0:
        return "AAC ADTS frame"
    if len(data) >= 2 and data[0] == 0xFF and (data[1] & 0xE0) == 0xE0:
        return "MPEG audio frame (MP3)"
    return "unknown"


def open_upstream(url, sonos_headers):
    req = urllib.request.Request(url)
    req.add_header("User-Agent", "sonos-lms-reference-relay/1")
    icy = sonos_headers.get("Icy-MetaData") or sonos_headers.get("icy-metadata")
    if icy:
        req.add_header("Icy-MetaData", icy)
    return urllib.request.urlopen(req, timeout=10)


def flac_rate(path):
    """Average bytes per second of a native FLAC file (from STREAMINFO)."""
    with open(path, "rb") as f:
        head = f.read(42)
    if not head.startswith(b"fLaC") or head[4] & 0x7F != 0:
        raise ValueError(f"{path} is not a native FLAC file (no fLaC + STREAMINFO)")
    info = head[8:42]
    sample_rate = (info[10] << 12) | (info[11] << 4) | (info[12] >> 4)
    total = ((info[13] & 0x0F) << 32) | int.from_bytes(info[14:18], "big")
    if not sample_rate or not total:
        raise ValueError(f"{path}: STREAMINFO has no sample rate or length")
    seconds = total / sample_rate
    with open(path, "rb") as f:
        f.seek(0, 2)
        size = f.tell()
    return size / seconds, seconds, sample_rate


class Relay(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"
    upstream = None
    forced_type = None
    file = None
    file_rate = 0.0

    def log_message(self, fmt, *args):  # silence the default access log
        pass

    def _log_request(self, rid):
        log(f"#{rid} {self.command} {self.path} from {self.client_address[0]}:{self.client_address[1]}")
        for k, v in self.headers.items():
            log(f"#{rid}   > {k}: {v}")

    def _send_head(self, rid, content_type, extra):
        lines = ["HTTP/1.1 200 OK", f"Content-Type: {content_type}", "Connection: close"]
        lines += [f"{k}: {v}" for k, v in extra]
        for line in lines[1:]:
            log(f"#{rid}   < {line}")
        self.wfile.write(("\r\n".join(lines) + "\r\n\r\n").encode("latin-1"))
        self.wfile.flush()

    def do_HEAD(self):
        rid = next(_ids)
        self._log_request(rid)
        ctype = self.forced_type or LAST_CONTENT_TYPE or ("audio/flac" if self.file else "application/octet-stream")
        self._send_head(rid, ctype, [])
        log(f"#{rid} HEAD answered")

    def serve_file(self, rid, started):
        ctype = self.forced_type or "audio/flac"
        self._send_head(rid, ctype, [])
        sent = 0
        first = True
        reason = "file served"
        burst = self.file_rate * 2.0
        try:
            while True:  # loop the file like a station that never ends
                with open(self.file, "rb") as f:
                    while True:
                        chunk = f.read(8192)
                        if not chunk:
                            break
                        if first:
                            log(f"#{rid} first body bytes: {chunk[:32].hex(' ')} -> {describe(chunk)}")
                            first = False
                        self.wfile.write(chunk)
                        sent += len(chunk)
                        due = started + max(0.0, (sent - burst) / self.file_rate)
                        wait = due - time.monotonic()
                        if wait > 0:
                            time.sleep(wait)
        except (BrokenPipeError, ConnectionResetError, socket.timeout) as exc:
            reason = f"client closed ({type(exc).__name__})"
        except Exception as exc:
            reason = f"error {exc!r}"
        log(f"#{rid} ended: {reason}; {sent} bytes in {time.monotonic() - started:.1f}s")

    def do_GET(self):
        global LAST_CONTENT_TYPE
        rid = next(_ids)
        self._log_request(rid)
        started = time.monotonic()
        if self.file:
            return self.serve_file(rid, started)
        try:
            up = open_upstream(self.upstream, self.headers)
        except Exception as exc:  # report upstream failure as Sonos would see it
            log(f"#{rid} upstream open failed: {exc!r} -> 503")
            self.wfile.write(b"HTTP/1.1 503 Service Unavailable\r\nContent-Length: 0\r\nConnection: close\r\n\r\n")
            return
        log(f"#{rid} upstream {up.status} {up.reason} ({self.upstream})")
        extra = []
        for k, v in up.headers.items():
            log(f"#{rid}   upstream< {k}: {v}")
            kl = k.lower()
            if kl.startswith("icy-") or kl in ("cache-control", "server", "accept-ranges"):
                extra.append((k, v))
        ctype = self.forced_type or up.headers.get("Content-Type", "application/octet-stream")
        LAST_CONTENT_TYPE = ctype
        self._send_head(rid, ctype, extra)
        sent = 0
        first = True
        reason = "upstream ended"
        try:
            while True:
                chunk = up.read(8192)
                if not chunk:
                    break
                if first:
                    log(f"#{rid} first body bytes: {chunk[:32].hex(' ')} -> {describe(chunk)}")
                    first = False
                self.wfile.write(chunk)
                sent += len(chunk)
        except (BrokenPipeError, ConnectionResetError, socket.timeout) as exc:
            reason = f"client closed ({type(exc).__name__})"
        except Exception as exc:
            reason = f"error {exc!r}"
        finally:
            try:
                up.close()
            except Exception:
                pass
        log(f"#{rid} ended: {reason}; {sent} bytes in {time.monotonic() - started:.1f}s")


def probe_file(path):
    try:
        rate, seconds, sample_rate = flac_rate(path)
        with open(path, "rb") as f:
            data = f.read(4096)
    except Exception as exc:
        print(f"ERROR {exc}")
        return 1
    print("CONTENT_TYPE=audio/flac")
    print(f"FORMAT={describe(data)}, {sample_rate} Hz, {seconds:.0f} s, {rate * 8 / 1000:.0f} kbit/s")
    return 0


def probe(url):
    try:
        up = open_upstream(url, {})
    except Exception as exc:
        print(f"ERROR {exc!r}")
        return 1
    data = up.read(4096)
    up.close()
    print(f"CONTENT_TYPE={up.headers.get('Content-Type', 'application/octet-stream')}")
    print(f"FORMAT={describe(data)}")
    return 0


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    src = ap.add_mutually_exclusive_group(required=True)
    src.add_argument("--upstream", help="live station URL (http or https)")
    src.add_argument("--file", help="local native FLAC file, served as a looping live stream")
    ap.add_argument("--port", type=int, default=8990)
    ap.add_argument("--content-type", help="force the Content-Type sent to Sonos")
    ap.add_argument("--probe", action="store_true", help="print the upstream content type and format, then exit")
    args = ap.parse_args()
    if args.probe:
        return probe_file(args.file) if args.file else probe(args.upstream)
    Relay.upstream = args.upstream
    Relay.forced_type = args.content_type
    if args.file:
        Relay.file = args.file
        Relay.file_rate, seconds, _ = flac_rate(args.file)
    server = ThreadingHTTPServer(("0.0.0.0", args.port), Relay)
    server.daemon_threads = True
    log(f"relay listening on :{args.port} -> {args.file or args.upstream}")
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        pass
    return 0


if __name__ == "__main__":
    sys.exit(main())
