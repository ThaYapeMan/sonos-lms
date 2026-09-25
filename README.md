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

## Pause and resume on Sonos

Pausing ends the HTTP response and sends UPnP **Stop** by default. Sonos resumes
FLAC radio (`x-rincon-mp3radio` with `audio/flac`) incorrectly from PAUSED:
the first Play can close the GET with ERROR_CORRUPT_FILE and an app dialog.
A plain native-FLAC relay reproduced the pause/resume failure independently of
the bridge. Stopping after pause made all three reference resumes play cleanly;
the bridge's Stop mode was physically verified on 2026-09-25 with status OK,
continued audio, and no dialog.

The app still shows Play. Its fresh GET receives a normal FLAC header and
chunked audio when LMS resumes; an LMS resume without a GET reissues the same
URL. Each connection's RelTime is anchored to its first PCM's track offset so
LMS position continues across reconnects. The deferred `strm q` path is unchanged.
LMS pause/play intent is retained while a transport call or stream restart is
busy; the latest state is applied once ready, with a deferred-transport log.
PlayStream failures retry up to three attempts, one second apart, then wait for
a new stream or transport command. A stream is complete only after success.
A device-resume request expires after five seconds without LMS `strm u`, allowing
another attempt; CLI errors retain that lease instead of retrying every poll.
No environment overrides are needed. `SONOS_SQUEEZEBOX_PAUSE=pause` remains an
explicit fallback to UPnP Pause and the previous same-URL resume behavior
(including HTTP 503 for a speculative held GET). The pause switch is read and
logged once at startup; invalid values warn and use `stop`.

### How the fix was found

This was a hard one. Finding it took more than nine hours of structured
troubleshooting over two days (24–25 September 2026), on top of earlier
attempts that went nowhere.

What made it so difficult:

- **The symptom pointed the wrong way.** The dialog blames our stream
  ("could not be played"), and Sonos closes our connection right before it
  appears, so every sign said the bridge was sending something wrong.
- **Sonos documents none of this.** Why a speaker closes a connection, sends a
  HEAD request or raises an error is visible only on the wire and in its UPnP
  events, never in a log.
- **Every plausible fix failed in its own way.** Answering Sonos's resume
  request with 503, closing it, leaving it open, sending only a Play, starting
  the audio at a clean FLAC frame, restarting the stream automatically,
  dropping chunked encoding: each one changed the details and none of them
  removed the dialog. A few brought the music back, but the dialog still appeared.
- **The cause was not in this code at all.** Sonos cannot resume FLAC radio from
  PAUSED, whoever serves it.

Proving that meant building dedicated tooling first:
`scripts/device-test.sh` runs fixed scenarios against a real speaker and
captures the bridge journal, the LMS event stream and status, and the network
traffic of each run. `scripts/reference-test.sh` with `scripts/reference-relay.py`
puts known-good sources (a live MP3 station, a headerless FLAC station, a clean
FLAC file) in front of the same speaker with the same UPnP command. It took at
least a dozen captured runs, and hours of reading packet captures and
cross-checking journals, LMS logs and Sonos event notifications, before a plain
FLAC file served by a minimal relay reproduced the exact same error. Only then
was it clear that the bridge had never been the problem. That pointed to a
simple fix: stop instead of pause.

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
