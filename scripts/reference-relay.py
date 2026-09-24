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
  reference-relay.py --probe --upstream URL     # print content type + format, exit
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
    if len(data) >= 2 and data[0] == 0xFF and (data[1] & 0xF6) == 0xF0:
        return "AAC ADTS frame"
    if len(data) >= 2 and data[0] == 0xFF and (data[1] & 0xE0) == 0xE0:
        return "MPEG audio frame (MP3)"
    if len(data) >= 2 and data[0] == 0xFF and data[1] in (0xF8, 0xF9):
        return "FLAC audio frame (no header)"
    return "unknown"


def open_upstream(url, sonos_headers):
    req = urllib.request.Request(url)
    req.add_header("User-Agent", "sonos-squeezebox-reference-relay/1")
    icy = sonos_headers.get("Icy-MetaData") or sonos_headers.get("icy-metadata")
    if icy:
        req.add_header("Icy-MetaData", icy)
    return urllib.request.urlopen(req, timeout=10)


class Relay(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"
    upstream = None
    forced_type = None

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
        ctype = self.forced_type or LAST_CONTENT_TYPE or "application/octet-stream"
        self._send_head(rid, ctype, [])
        log(f"#{rid} HEAD answered")

    def do_GET(self):
        global LAST_CONTENT_TYPE
        rid = next(_ids)
        self._log_request(rid)
        started = time.monotonic()
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
    ap.add_argument("--upstream", required=True, help="live station URL (http or https)")
    ap.add_argument("--port", type=int, default=8990)
    ap.add_argument("--content-type", help="force the Content-Type sent to Sonos")
    ap.add_argument("--probe", action="store_true", help="print the upstream content type and format, then exit")
    args = ap.parse_args()
    if args.probe:
        return probe(args.upstream)
    Relay.upstream = args.upstream
    Relay.forced_type = args.content_type
    server = ThreadingHTTPServer(("0.0.0.0", args.port), Relay)
    server.daemon_threads = True
    log(f"relay listening on :{args.port} -> {args.upstream}")
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        pass
    return 0


if __name__ == "__main__":
    sys.exit(main())
