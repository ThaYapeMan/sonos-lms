# sonos-squeezebox

Make a Sonos speaker behave like a real Logitech Media Server player: synchronisable
with other Squeezebox players, controllable from any LMS app (Material, iPeng,
Squeezer, ...), and driven entirely through the standard slimproto protocol -- no
Sonos-specific app, no separate remote, no manual pairing step per track.

## The problem this solves

LMS knows how to talk to Squeezebox hardware and to squeezelite instances. It has
no idea what a Sonos speaker is. Sonos, in turn, expects to be driven through its
own UPnP/SOAP control surface and to pull audio from an HTTP URL it is handed --
not to receive a slimproto stream.

This project sits between the two. It is squeezelite itself (same decode, buffer
and stream pipeline LMS already trusts), but with the ALSA output swapped for a
custom backend that talks to a Sonos device instead of a sound card. From LMS's
point of view, a Sonos room is just another squeezelite client. From Sonos's point
of view, it is being handed a normal HTTP audio URL to play, the same as if you had
pasted a stream link into the Sonos app.

## How the pieces fit together

Each running instance represents exactly one Sonos room. On startup it discovers
the Sonos device (or connects to a given IP), discovers the LMS server (or uses
`--server`), derives a stable player identity from the Sonos player's UUID, and
launches squeezelite against that identity as if it were any other client.

From there, three loosely-coupled pieces keep the two sides in sync:

**Getting audio out.** The output backend does not push PCM to a sound card; it
watches squeezelite's own silent/non-silent flag. A transition into audio starts a
FLAC encoder and opens an HTTP endpoint (`/music/squeezebox.flac`) that Sonos is
told to `PlayStream()`. A transition back to silence tears the stream down and
stops the speaker. The encoder deliberately stays only a couple of seconds ahead
of real time -- Sonos buffers aggressively on its own, and letting the encoder run
far ahead only made that worse and confused LMS's own progress tracking.

**Keeping transport commands sane.** LMS's `p`/`q`/`s`/`u` commands and the Sonos
device's own pause/play events arrive independently and can race. Two small,
independently testable state machines absorb that: one decides what an LMS
"unpause" actually means right now (a genuinely new stream, resuming a held HTTP
request, or reissuing the play command against the same URL), and the other
delays turning a stop into an actual device pause by 400 ms, because a drag-seek
in the UI arrives as a stop immediately followed by a new play -- without the
delay, every seek would cause an audible blip on the speaker.

**Keeping LMS's numbers honest.** squeezelite counts frames as it decodes them,
which runs well over a second ahead of what the Sonos speaker is physically
outputting once its own network and playback buffering is accounted for. A
background poll of the Sonos device's actual transport position (via UPnP) feeds
a corrected figure back into the same counter LMS reads for its progress bar and
`ms_played` calculation, so the displayed position tracks what you actually hear
rather than what has merely been decoded.

## Source layout

| Path | What lives there |
| --- | --- |
| `sonos-squeezebox.cpp` | Startup: argument parsing, Sonos/LMS discovery, wiring the rest together. |
| `slimproto_sonos.c` | The unmodified upstream `slimproto.c`, plus an interception point for `strm` transport commands. |
| `output_sonos.c` / `.h` | Squeezelite output backend; silent/non-silent detection, feeds the encoder. |
| `sbencoder.cpp` / `.h` | FLAC encoding of the decoded PCM, rate-limited relative to real time. |
| `sbstreamer.cpp` / `.h` | The HTTP server Sonos actually connects to for the audio. |
| `resume_state.h` | The "what does this unpause mean" decision logic, isolated from I/O for testing. |
| `stop_debounce.h` | The 400 ms stop/seek debounce logic, likewise isolated. |
| `sonos-status.cpp` / `.h` | UPnP polling of the Sonos device's own transport/track state. |
| `sonos-position.cpp` / `.h` | UPnP polling of actual playback position, exposed to the output thread. |
| `noson/`, `squeezelite/` | Vendored GPL-3.0 submodules, each our own fork -- see `.gitmodules`. Do not hand-edit; changes there are lost on `git submodule update`. |

## Building it

```sh
sudo apt-get install -y --no-install-recommends \
    make cmake g++ libz-dev libssl-dev libflac++-dev libpulse-dev \
    libasound-dev libvorbis-dev libfaad-dev libmad0-dev libmpg123-dev libsoxr-dev

git clone --recursive https://github.com/ThaYapeMan/sonos-squeezebox.git
cd sonos-squeezebox
make
```

Forgot `--recursive`? `git submodule update --init --recursive` fixes it after the
fact.

## Running it

```
sonos-squeezebox --room="Living Room" [--ip=<sonos-ip>] [--server=<lms-host>]
```

| Flag | Meaning |
| --- | --- |
| `--room=<name>` | Required. The Sonos room/zone to take over. |
| `--ip=<address>` | Skip Sonos auto-discovery and talk to this player directly (any player in the household will do -- they share topology). Needed if discovery can't reach the Sonos network. |
| `--server=<host>` | LMS hostname or IP **only**, not a web-UI port. Precedence: explicit `--server` > `LMS_SERVER=` in `/etc/sonos-squeezebox/config` > automatic UDP broadcast discovery on port 3483 (same subnet only; does not cross routers). |
| `--debug` | Raise noson's own logging verbosity. |
| `--file=<path>` | Play one local audio file straight to the Sonos speaker, bypassing LMS entirely -- a quick way to check the Sonos connection and encoder in isolation. |

The first instance binds port 1400 for its own use; a second concurrent instance
(a second room) moves to 1401, and so on.

A run looks roughly like this once both sides are found:

```
$ sonos-squeezebox --room="Living Room"

sonos-squeezebox -- Sonos as an LMS player
Copyright (C) 2026 Jaap van Vliet

Discovering Sonos devices ... found 3
  Kitchen     RINCON_B8E9375C412001400  192.168.1.40:1400
  Living Room RINCON_347E5C1A902001400  192.168.1.41:1400
  Bedroom     RINCON_78F9C0A3512001400  192.168.1.42:1400

Zones:
  Kitchen      -> coordinator: Kitchen
  Living Room  -> coordinator: Living Room
  Bedroom      -> coordinator: Bedroom

Taking over "Living Room" (MAC 34:7E:5C:1A:90:20) ... connected to LMS
```

### Installing room services

Run from the deployment checkout at `/opt/sonos-squeezebox`, with root privileges
for the installer (or `make install`). First deployment:

```sh
make
sudo scripts/install-devices.sh "Room One" "Room Two"
```

The LMS address is optional. Add `--server=<lms-host>` when broadcast discovery
cannot reach LMS, for example across VLANs. This saves the override in
`/etc/sonos-squeezebox/config`:

```ini
# Optional LMS override
LMS_SERVER=<host>
```

Edit this file by hand at any time and restart the affected services, or set it
with `scripts/install-devices.sh --server=...`. Omitting the option preserves an
existing override; remove or empty `LMS_SERVER=` to return to discovery. If startup
finds no server, squeezelite still retries its own discovery for core playback;
metadata and Sonos-app pause/play relay need a resolved server and a restart.

Rooms are remembered in `/etc/sonos-squeezebox/rooms`, one per line. Quote names
containing spaces; the installer handles systemd escaping. Subsequent updates,
with root privileges for `make install`:

```sh
git pull && make && make install
```

Only listed rooms are enabled/started or restarted; unrelated units are untouched.

## Testing

```sh
make test
```

Runs three self-contained binaries against simulated sockets -- no physical Sonos
device involved:

- `encoder-test` exercises the FLAC encoding path in `sbencoder.cpp`.
- `resume-state-test` exercises `resume_state.h`/`stop_debounce.h` in isolation.
- `streamer-test` exercises the HTTP broker in `sbstreamer.cpp`: headers, held-GET
  resume, reconnect behaviour, idle timeouts, debounce timing.

Two Python-driven C++ fixtures also extract the production transport functions
and LMS discovery/config parsers. Discovery tests perform no UDP I/O.

These pin down the transport/stream state machine logic exhaustively, but none of
it proves anything about a real speaker. Before trusting a transport-code change,
run through [DEVICE-VERIFICATION.md](DEVICE-VERIFICATION.md) against an actual
Sonos player by hand.

## Where it falls short

**No artist or album on the Sonos display.** The vendored noson library's
`PlayStream()` call only accepts a title and an artwork URL; there is no field for
artist or album, so neither reaches Sonos's DIDL metadata. The speaker's own
display and the Sonos app show title and cover art correctly, but the artist/album
lines stay blank. Fixing this needs either an extension to noson's `PlayStream()`
or a hand-built DIDL payload that bypasses it.

**In-stream metadata updates don't work.** Updating the "now playing" title
mid-stream (without restarting it) was attempted via Shoutcast-style ICY metadata
injection into the FLAC stream -- the mechanism itself was fully implemented and
tested. It doesn't work because Sonos never sends the `Icy-MetaData: 1` opt-in
header for `audio/flac` requests, and injecting the blocks anyway just corrupts
the stream (`ERROR_CORRUPT_FILE`). Track title and artwork are therefore only ever
set once, at stream start.

## Related

[philippe44/LMS-uPnP](https://github.com/philippe44/LMS-uPnP) takes the opposite
approach -- driving Sonos purely over UPnP rather than making it look like a
squeezelite client -- and is worth a look if this project's constraints don't fit
your setup.

## License

GPL-3.0. See [LICENSE](LICENSE). The vendored `noson` and `squeezelite` submodules
are GPL-3.0 as well; GPL-3.0's copyleft means a derivative or combined work needs a
GPL-compatible outbound license -- no additional, more restrictive terms (e.g. a
noncommercial clause) can be layered on top.

Copyright (C) 2026 Jaap van Vliet

### Diagnostics

`SONOS_SQUEEZEBOX_DEVICE_RESUME` is a temporary device-test switch, read and
logged once at startup. `sameurl-503` (default) retains the existing same-URL
resume and cancelled held-GET 503 response. `sameurl-close` disconnects a
cancelled GET without audio or HTTP bytes; `sameurl-empty200` sends streaming
200 headers and an empty chunked body. `playonly` keeps an open held GET,
feeds it immediately, and dispatches one UPnP Play from a detached thread. That
thread retries the transport lock every 10 ms for up to three seconds, skipping
if the stream changes or the deadline expires. Without an open GET it falls
back to the same-URL path.

`playonly-frames` uses the same resume and Play retry path, but strips the
`fLaC` marker and all FLAC metadata blocks from only the held GET selected by a
device-initiated resume. Its HTTP body starts at the first audio frame. Initial
playback, ordinary LMS unpause, PlayStream, and other GETs (including promoted
standbys) retain fresh FLAC headers. It logs `stream N: playonly-frames: skipped
<n> metadata bytes`. This tests whether Sonos resumes its existing decoder:
on 2026-09-24 at 17:14, all four tested `playonly` resumes rejected fresh FLAC
metadata with a HEAD request, GET closure, and `ERROR_CORRUPT_FILE` / `STOPPED`.
This decoder-continuation hypothesis is superseded by the MP3 reference finding
below. The mode remains available for comparison.

`SONOS_SQUEEZEBOX_PAUSE=pause|stop` selects how LMS `strm p` pauses the
speaker (default `pause`). The value is read and logged once at startup;
invalid values warn and use `pause`. With `stop`, the bridge ends the HTTP
response first, then sends UPnP Stop and logs `strm p -> UPnP Stop (pause=stop)`.
This also applies to the LMS pause relayed from a Sonos-app pause. The deferred
400 ms `strm q` path continues to use Pause.

Stop-for-pause is remembered for the current stream. STOPPED is idle in that
state: it sends nothing to LMS and does not restart playback. A subsequent
STOPPED → TRANSITIONING/PLAYING is a device resume: the bridge asks LMS to play
and feeds Sonos's fresh GET with normal chunked FLAC, including its header.
An LMS unpause without an open GET uses PlayStream with the same URL. The marker
clears on a new stream, `strm s/q`, or resumed PLAYING; it is not persisted.

With `pause=stop`, DEVICE_RESUME strategies are ignored (logged once if set),
and RESUME_BODY/RESUME_TRANSFER do not alter the fresh resume GET. For physical
testing, remove those experiment settings and use only
`Environment="SONOS_SQUEEZEBOX_PAUSE=stop"` in the service drop-in. This avoids
resuming a PAUSED FLAC radio decoder by leaving the speaker STOPPED instead.

`feed-restart` keeps and feeds Sonos's own device-resume GET with normal FLAC
metadata by default, without invalidation, a server-induced close, PlayStream, or Play at
that point. It logs `device resume: strategy=feed-restart feeding Sonos's own
GET`. A one-shot, five-second watch then observes transport state. STOPPED
schedules one same-URL PlayStream restart after 200 ms; the transport lock is
retried every 10 ms for up to three seconds, bounded also by the watch deadline.
Two continuous seconds of PLAYING disarm it without restarting. Stream changes,
LMS `p/q/s`, a new device pause, and deadline expiry cancel it with a reason.
Without an open held GET, the existing SameURL path is used.

The reference radio test on 2026-09-24 used MP3 through a transparent relay with
the bridge stopped and the same `x-rincon-mp3radio` UPnP setup. In three rounds,
Sonos's first resume GET received data, issued HEAD 11–13 ms later, closed from
the client side, and reported STOPPED/OK; the next GET played. The bridge's
`playonly` trace likewise showed client closure and silence without a reported
dialog, whereas `sameurl-close` recovered playback but produced a dialog in all
three rounds. `feed-restart` tests letting Sonos finish that first attempt
itself before automatically restarting. The later feed-restart test restored
playback but still reported ERROR_CORRUPT_FILE and an iOS dialog. Its resume
response used chunked encoding and fresh FLAC metadata; the reference used raw
MP3 with connection-close framing. These differences motivate two independent
experiments, not a confirmed explanation of the dialog. Raw evidence stays
local in `logs/`.

With `feed-restart`, two switches affect only the held GET selected for device
resume, before PCM is released:

- `SONOS_SQUEEZEBOX_RESUME_BODY=header|frames` (default `header`): send fresh FLAC
  metadata, or strip the `fLaC` marker and metadata blocks and start at the first
  audio frame using the same parser as `playonly-frames`.
- `SONOS_SQUEEZEBOX_RESUME_TRANSFER=chunked|raw` (default `chunked`): use current
  chunked framing, or write the body directly with `Connection: close` and
  neither Transfer-Encoding nor Content-Length.

Both switches are read and logged once at startup; invalid values warn and use
their defaults. Each selected request logs `resume GET #id body=<value>
transfer=<value>`. Initial GETs, promoted standbys, the automatic restart GET,
and other strategies retain their existing response formats. Test frames/raw,
header/raw, and frames/chunked separately to distinguish the two effects.

Unknown values warn and use `sameurl-503`. The uncancelled five-second no-audio
timeout returns 503 in every mode. Each application logs `device resume:
strategy=<mode>`. Physical Sonos verification is still required.
