# SONOS::Squeezebox

This software lets you integrate Sonos players in a Logitech Media Server (Squeezebox) environment. It registers each Sonos
room as a real, synchronisable LMS player — not an LMS plugin, but a standalone program built on the squeezelite codebase with
a Sonos output driver in place of ALSA.

It builds on two excellent software projects, being Noson and Squeezelite. Noson is a C++ language library to control Sonos
equipment, primarily used in the Noson-app. Squeezelite is a headless C language client for LMS that is often used as a skeleton
for larger projects.

## Features

* **Track metadata**: at stream start, the LMS CLI (port 9090) is queried for the current track's title, artist, album, and cover art. The title and artwork URL are passed to Sonos via `PlayStream()`. The MAC address is sent raw (not URL-encoded) — URL-encoding causes LMS to silently fail to match the player.

* **Human-readable player name**: the player registers in LMS as `<room> (Sonos)` (e.g. `Study (Sonos)`), so it shows up correctly in every LMS controller.

* **Accurate playback position reporting**: squeezelite's internal position counter tracks how much audio has been decoded and sent to the encoder — this runs 1800 ms or more ahead of what actually comes out of the Sonos speaker, because Sonos adds its own network and playback buffer. This fork corrects the position reported to LMS by polling the real Sonos playback position via UPnP AVTransport `GetPositionInfo` (already part of the noson library) once per second. The result is stored in an `std::atomic<uint32_t>` in `sonos-position.cpp` and read by the output thread to compute `output.device_frames`, which feeds directly into slimproto's `ms_played` formula. LMS now receives the position that the speaker is actually at, not what the decoder is ahead of.

* **Robust pause, resume and seek handling**: LMS transport commands (`p`/`q`/`s`/`u`) and Sonos device-initiated pause/play are reconciled through two small explicit state machines (`ResumeState`, `stop_debounce.h`) rather than restarting the stream on every transition. A plain LMS stop is debounced 400 ms so a drag-seek (which arrives as `q` immediately followed by `s`) never triggers an audible device pause; a real pause holds the HTTP connection open and re-feeds it on resume instead of reissuing `PlayStream`. See [Technical notes](#technical-notes) and [DEVICE-VERIFICATION.md](DEVICE-VERIFICATION.md) for the exact decision table and the physical-device acceptance checks this behaviour is validated against.

## Usage

```sh
./sonos-squeezebox [options] --room=<Room/Zone name>
```

* Connecting to Sonos. You specify the room or zone name of the player you want to control using the `--room` option. The application will
scan the network for Sonos players. If this fails or if the players are located in a separate network you may provide the IP-address of
the Sonos player using the `--ip` option. This can be the IP-address of any player in the network as they generally find each other and provide
the software with a complete list of available players. You may need to open a port in the firewall to allow access from the Sonos box to the `sonos-squeezebox` software. The first instance of the software will be listening on port 1400, additional instances will use 1401, 1402, etc.

* Connecting to the Logitech Media Server (LMS). The application searches for the squeezebox server by scanning the network. If this fails or if the server is located in a separate network you may provide the server address using the `--server` option. Pass the hostname or IP address only — do **not** append a port. The slimproto protocol uses port 3483; port 9000 is the LMS web interface and will not work.

* Additional flags: `--debug` raises noson's own logging verbosity; `--file=<path>` plays a local audio file to the connected Sonos player once, bypassing LMS entirely — useful for verifying the Sonos connection and encoder independently of the streaming/transport logic.

### Example

```text
$ ./sonos-squeezebox --ip=192.168.15.247 --room=Bibliotheek --server=192.168.15.1


| SONOS::Squeezebox -- deploy Sonos in a Logitech Media Server (LMS) streaming environment
|
| Copyright (c) 2026 Jaap van Vliet


Connecting to Sonos (through player 192.168.15.247) ... SUCCESS

+----------------------------------------------------------------------- devices / players ---+
| player name                         | uuid                     | host               |  port |
+---------------------------------------------------------------------------------------------+
| Bibliotheek                         | RINCON_000E5883160901400 | 192.168.15.243     |  1400 |
| Eetkamer                            | RINCON_5CAAFDF6210101400 | 192.168.15.247     |  1400 |
+---------------------------------------------------------------------------------------------+

+--------------------------------------------------------------------------- zones / rooms ---+
| room name                           | coordinating player                                   |
+---------------------------------------------------------------------------------------------+
| Bibliotheek                         | Bibliotheek                                           |
| Eetkamer                            | Eetkamer                                              |
+---------------------------------------------------------------------------------------------+

Connecting to room Bibliotheek ... SUCCESS (MAC = 00:0E:58:83:16:09)
```

## Building

### Requirements

```sh
apt-get install -y --no-install-recommends \
        make cmake g++ libz-dev libssl-dev libflac++-dev libpulse-dev \
        libasound-dev libvorbis-dev libfaad-dev libmad0-dev libmpg123-dev libsoxr-dev
```

### Cloning

```sh
git clone --recursive https://github.com/ThaYapeMan/sonos-squeezebox.git
```

If you cloned without the `--recursive` option, you can initialize the sub-modules using:

```sh
git submodule update --init --recursive
```

### Compiling

```sh
make
```

### Testing

```sh
make test
```

Builds and runs three local test binaries against the real HTTP broker and encoder code (with simulated sockets, no
physical Sonos device involved):

* `encoder-test` — `sbencoder.cpp`, the FLAC encoding path.
* `resume-state-test` — `resume_state.h`/`stop_debounce.h`, the pause/seek/resume decision logic in isolation.
* `streamer-test` — `sbstreamer.cpp`, the HTTP request broker: headers, client-owned probe closure, retained EOF state
  across reconnects, 30-second pause, idle limits, and stop/seek debounce timing.

These cover the transport/stream state machine's logic exhaustively, but cannot exercise a physical Sonos device.
[DEVICE-VERIFICATION.md](DEVICE-VERIFICATION.md) is the acceptance checklist to run by hand against a real player before
trusting a change to the transport code; it is not covered by `make test`.

## Architecture

The program is a modified squeezelite build: it links the unmodified squeezelite decode/buffer/stream pipeline against a
custom output backend and slimproto command handler, so squeezelite itself needs no changes.

| File | Role |
| --- | --- |
| `sonos-squeezebox.cpp` | Entry point: CLI parsing, Sonos device/zone discovery, LMS server discovery, wiring everything together. |
| `slimproto_sonos.c` | Compiles the upstream `slimproto.c` unchanged, then intercepts `strm` (transport) commands before they reach it. Holds the state-machine invariants as a block comment at the top of the file. |
| `output_sonos.c` / `output_sonos.h` | The squeezelite output backend. Detects the silent/non-silent transitions that start and stop a stream, and feeds decoded PCM to the encoder. |
| `sbstreamer.cpp` / `sbstreamer.h` | The HTTP request broker (`/music/squeezebox.flac`) that actually serves the FLAC stream to Sonos: header handling, held-GET resume, idle timeouts, and rejecting a duplicate concurrent request from the same device. |
| `sbencoder.cpp` / `sbencoder.h` | FLAC encoding of the PCM squeezelite decodes, throttled to at most ~2 seconds ahead of real time. |
| `resume_state.h` | `ResumeState` — decides, for every `strm u` (LMS unpause), whether that means a new stream, feeding a held HTTP GET, or reissuing `PlayStream` on the same URL. Pure logic, no I/O; unit-tested by `resume-state-test`. |
| `stop_debounce.h` | `StopDebounce` — delays turning an LMS `strm q` into a device Pause by 400 ms, so a drag-seek's `q`-then-`s` pair never produces an audible pause. |
| `sonos-status.cpp` / `sonos-status.h` | Polls the Sonos player's transport/track state via UPnP and reports device-initiated pause/play back into the LMS-facing state machine. |
| `sonos-position.cpp` / `sonos-position.h` | Polls real Sonos playback position (see Features above) and exposes it to the output thread as an `std::atomic<uint32_t>`. |
| `noson/`, `squeezelite/` | Vendored GPL-3.0 submodules (own forks — see `.gitmodules`). Do not edit in place; changes are lost on `git submodule update`. |

Two independent transport state machines are kept deliberately small and separate rather than merged into one: LMS's
`p`/`q`/`s`/`u` commands on one side, the Sonos device's own pause/play events on the other, reconciled through
`ResumeState` so that exactly one action results from any transition. `slimproto_sonos.c`'s top-of-file comment is the
authoritative, current statement of that contract — keep it in sync with any change to the transport logic.

## Technical notes

* Sonos buffers a lot and causes latency issues with other software. Similar stuff happened to the pulseaudio support in Noson and the Noson-app. A different solution was chosen here. We throttle the encoder to not encode more than 2 seconds of music in the future. This also keeps the squeezebox server happy as it does not really understand minutes of music being consumed in mere seconds.

* Sonos sometimes gets greedy and requests the same stream twice. Streaming to multiple sinks is not supported in the software, so the first idea was to cancel the first request and continue serving on the second. That doesn't work. What works is to reject the second request, and continue serving on the first.

* Goal was to use squeezelite as much "out-of-the-box" as possible, by providing only a Sonos output module. But how to know when to start a (new) stream to the Sonos? Turned out that the output module receives a silent flag. Looking at transitions here is the solution. From silent to non-silent requires to start a new stream, from non-silent to silent needs to terminate the current stream which will stop the Sonos. While silent we do not stream silence over the network. During the playback of an album or playlist, the silent flag will not toggle between tracks so the stream continues nicely.

* Sonos does not support high quality streams, but this is handled quite nicely by LMS. By only advertising support for 44k1 to the squeezebox server, streams are automatically sampled down by the server. No need for resampling in our client software.

* Squeezebox differentiates between different players using the MAC-address. Squeezelite by default uses the host MAC, and we would run into problems controlling multiple players from the same host. We therefore use the player MAC-address and try to retrieve it from the UUID string (assuming it will always be in the form `RINCON_<MAC><PORT>`). The MAC is sent to LMS as a raw colon-separated hex string; URL-encoding it causes LMS to silently fail to match the player.

* The LMS player name is registered as `<room> (Sonos)` (e.g. `Study (Sonos)`) so it is human-readable in every LMS controller, including iPeng and Material Skin.

* **Position reporting**: squeezelite's internal `frames_played` counter represents how much audio the decoder has processed, not what the speaker has actually output. Because Sonos adds a 1800 ms+ network and playback buffer, the LMS position display was always significantly ahead of the audible playback. The fix polls the actual Sonos position via UPnP AVTransport `GetPositionInfo` every second (noson caches the result, so only one network request per second is made) and uses the `RelTime` field to back-calculate `output.device_frames`. slimproto's existing `ms_played` formula then yields the Sonos position rather than the decode offset.

## Known limitations

* **Artist and album not shown on Sonos**: noson's `PlayStream()` interface accepts only a title and an artwork URL — there is no parameter for artist or album. The DIDL metadata sent to Sonos therefore lacks `dc:creator` and `upnp:album` fields. The Sonos app shows the track title and cover art correctly, but the artist and album lines remain empty. Fixing this would require extending noson's `PlayStream()` or bypassing it with a custom DIDL payload.

* **Log output is buffered when running as a service**: the program writes to stdout, which the C runtime buffers when no terminal is attached. Running under systemd means `journalctl` shows nothing until the process exits. Use `ss -tnp` to inspect connection state while the process is running.

## Rejected approaches

### ICY/Shoutcast in-band metadata

To update the now-playing title during gapless playback (without restarting the stream), we investigated injecting Shoutcast-style ICY metadata blocks into the FLAC stream. This is a well-known radio-stream technique: the server advertises `icy-metaint: N` in the HTTP response, and after every N audio bytes it inserts a `StreamTitle='...';` metadata block that the client reads without treating as audio.

The implementation was completed and tested — correct byteteller, exact chunk-splitting at `ICY_METAINT` boundaries, single-quote escaping, 0x00 "no update" block. However, Sonos never sends the `Icy-MetaData: 1` request header that is required to opt in to ICY injection for `audio/flac` streams. All test streams confirmed this:

```
stream 1: Icy-MetaData header = ''
stream 2: Icy-MetaData header = ''
stream 3: Icy-MetaData header = ''
```

Without the opt-in header, injecting ICY blocks corrupts the FLAC stream (Sonos reports `ERROR_CORRUPT_FILE`). This is consistent with documentation for philippe44's LMS-uPnP bridge, which also notes that Sonos does not support ICY metadata for FLAC streams.

The ICY implementation has been removed. Track title and cover art are now set at stream start via `fetchLmsTrackInfo()` and `PlayStream()`. Gapless within-stream metadata updates remain an open problem.

## Related software

* Philippe44 created the [LMS to UPnP bridge](https://github.com/philippe44/LMS-uPnP) which you may be able to use as Sonos supports UPnP.

## License

This project is licensed under the **GNU General Public License v3.0** (GPL-3.0). See the [LICENSE](LICENSE) file for the full text.

Copyright (C) 2026 Jaap van Vliet

Both vendored submodules — noson and squeezelite — are also GPL-3.0. GPL-3.0 copyleft requires that derivative works and combined works remain under a GPL-compatible license; no more-restrictive outbound license (such as a noncommercial clause) can be applied.
