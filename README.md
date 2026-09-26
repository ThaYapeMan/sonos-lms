# sonos-lms

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

## Why a continuous stream, and how this differs from track-by-track UPnP

There are two basic ways to make LMS music come out of a Sonos speaker. Both use
UPnP to control the speaker -- the difference is **who produces the audio and who
is in charge of the playlist**.

**Track-by-track (the usual UPnP renderer approach).** The Sonos is handed one
track at a time -- either as a file URL or as a queue of tracks -- and plays each
one itself. This is how a Sonos plays its own music library, and how most
UPnP/DLNA bridges work by default.

**Continuous stream (this project).** The Sonos is handed a single, never-ending
FLAC stream, the way it would play an internet radio station. The audio is
produced by squeezelite under full LMS control: LMS decides what plays, when, and
how it sounds; the Sonos simply renders what it receives.

The trade-off, honestly:

| Continuous stream (sonos-lms) | Track-by-track UPnP |
|---|---|
| ✔ LMS processes everything: ReplayGain, crossfade, DSP | ✘ Needs workarounds -- the Sonos plays the original file |
| ✔ Synchronises with every other LMS player | ✘ LMS sync groups need a shared stream |
| ✔ Lossless FLAC, including internet radio and streaming services in LMS | ✔ Lossless for local files; radio/services need separate handling |
| ✔ Pause/resume from LMS or the Sonos app without errors (see [Pause and resume on Sonos](#pause-and-resume-on-sonos)) | ✔ Native Sonos pause |
| ✘ No Next/Previous button in the Sonos app *(under investigation)* | ✔ Next/Previous in the Sonos app |
| ✘ Track title in the Sonos app does not change during an album *(work in progress)* | ✔ Correct title per track |
| ✘ Seeking only from LMS *(under investigation)* | ✔ Seeking in the Sonos app too |

**In short:** a track-by-track approach mainly improves things *in the Sonos
app*. If you control playback from LMS -- Material Skin, the web interface,
iPeng -- what you gain is limited, and what you give up (LMS sound processing and
synchronisation) is real. sonos-lms deliberately chooses the continuous stream and
treats the Sonos as a real LMS player.

The three ✘ points share one root cause: Sonos treats the stream as internet
radio, and radio has no next track, no seek bar, and only updates its title via
ICY metadata, which Sonos requests for MP3/AAC but never for FLAC (see
[Where it falls short](#where-it-falls-short)). Status:

- **Track titles -- work in progress.** Solvable within stream mode, as options:
  start a new stream at each track change (correct titles, but a short gap between
  tracks -- not for gapless albums or DJ mixes), or a lossy MP3/AAC stream that
  carries ICY titles.
- **Next/Previous and seeking in the Sonos app -- under investigation.** Not
  possible while the Sonos sees a radio stream. The idea being explored: give the
  Sonos one item per track that still points to this bridge, so LMS keeps producing
  the audio (with all its processing) while the Sonos app gains Next, Previous and a
  seek bar. Whether Sonos accepts this without new pause or gapless problems still
  has to be proven on real speakers.

## Audio quality

| Setting | Values | Default |
|---|---|---|
| `SONOS_LMS_AUDIO` | `24/48`: 24-bit FLAC at the source's 44.1/48 kHz rate; `16/44`: legacy 16-bit/44.1 kHz | `24/48` |

The setting is read once at startup and logged. Invalid values warn and use
`24/48`. Restart the bridge after changing it, for example with a systemd
`Environment=SONOS_LMS_AUDIO=16/44` override for comparison or older speakers.
The default targets Sonos S2's 24-bit/48 kHz FLAC support.

At 44.1 and 48 kHz, decoded PCM keeps its sample rate and up to 24 bits of
precision. A 16-bit source is padded with zero bits, without changing its sample
values. Bit-exact playback assumes LMS ReplayGain, DSP, crossfade and other sample
processing are disabled; lossy sources remain lossy.

The output driver lists 48,000 and 44,100 Hz. Slimproto advertises
`MaxSampleRate=48000` (a maximum, not an exact rate whitelist), so LMS performs
any necessary downsampling before sending audio. With LMS's standard transcoding
configuration and working resampler, 88.2 kHz becomes 44.1 kHz, and 96/192 kHz
becomes 48 kHz. LMS sync-group limits or custom transcoding settings can lower
that further. See [LMS's sample-rate selection](https://github.com/LMS-Community/slimserver/blob/public/9.0/Slim/Player/CapabilitiesHelper.pm).
Legacy mode advertises `MaxSampleRate=44100` and retains the previous 16-bit path
and per-track stream restarts.

A FLAC header fixes its rate for that stream. Natural playlist continuation at
the same rate keeps the stream gapless; a rate change creates a new stream ID and
uses the normal PlayStream path, so a short gap is possible. Explicit seeks and
track replacements still start a new stream. Each new stream logs, for example,
`stream 12: FLAC 24-bit 48000 Hz`. DIDL remains `audio/flac`.

## Source layout

| Path | What lives there |
| --- | --- |
| `sonos-lms.cpp` | Startup: argument parsing, Sonos/LMS discovery, wiring the rest together. |
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
    make cmake g++ python3 libz-dev libssl-dev libflac++-dev libpulse-dev \
    libasound-dev libvorbis-dev libfaad-dev libmad0-dev libmpg123-dev libsoxr-dev

git clone --recursive https://github.com/ThaYapeMan/sonos-lms.git
cd sonos-lms
make
```

Forgot `--recursive`? `git submodule update --init --recursive` fixes it after the
fact.

## Running it

```
sonos-lms --room="Living Room" [--ip=<sonos-ip>] [--server=<lms-host>]
```

| Flag | Meaning |
| --- | --- |
| `--list-rooms --details` | Sorted tab-separated name, model, IP, coordinator and comma-separated members (coordinator first); `-` means unknown. Uses the room’s primary device for bonded speakers. |
| `--find-server` | Discover LMS; print only its host/IP and exit 0, or print nothing and exit non-zero. |
| `--list-rooms` | Print sorted, unique room names, including group members, then exit. Uses `SONOS_LMS_UPNP` and honours `--ip`. |
| `--room=<name>` | Required except with `--list-rooms` or `--find-server`. The Sonos room/zone to take over. |
| `--ip=<address>` | Skip Sonos auto-discovery and talk to this player directly (any player in the household will do -- they share topology). Needed if discovery can't reach the Sonos network. |
| `--server=<host>` | LMS hostname or IP **only**, not a web-UI port. Precedence: explicit `--server` > `LMS_SERVER=` in `/etc/sonos-lms/config` > automatic UDP broadcast discovery on port 3483 (same subnet only; does not cross routers). |
| `--debug` | Raise noson's own logging verbosity. |
| `--file=<path>` | Play one local audio file straight to the Sonos speaker, bypassing LMS entirely -- a quick way to check the Sonos connection and encoder in isolation. |

The first instance binds port 1400 for its own use; a second concurrent instance
(a second room) moves to 1401, and so on.

A run looks roughly like this once both sides are found:

```
$ sonos-lms --room="Living Room"

sonos-lms -- Sonos as an LMS player
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

From `/opt/sonos-lms`, run `make` then `sudo make install` (root and Python 3
required). The installer shows its build, the LMS host and its source, then a room
table with model, IP, group and bridge state. It warns if LMS discovery differs
from the saved host. An empty `LMS_SERVER` uses discovery; a missing line is added
with the discovered host or an empty value.

On the first install, it asks about every discovered room. Later runs ask only
about new rooms, then offer to change the others. With no new rooms, one selection
question lets you keep the existing choices. Offline rooms retain their config
lines and appear as offline. Room arguments are enabled without per-room questions.

Example re-run after a new build and discovery of MBR:

```text
sonos-lms installer — build abc1234
LMS server: 192.0.2.23 (config)
LMS server [192.0.2.23]:
Sonos rooms found: 3
Room        Model   IP          Group                          Bridge to LMS
MBR         One     192.0.2.24  -                              new
Sonos Port  Port    192.0.2.25  member of Study                yes, running
Study       Play:1  192.0.2.26  coordinator: Study+Sonos Port  yes, running
Bridge Sonos room "MBR" to LMS? [y/N] y
Change other rooms bridged to LMS? [y/N]
Config:
--- current config
+++ proposed config
@@ -4,2 +4,3 @@
 room.Sonos Port=yes
 room.Study=yes
+room.MBR=yes
Start:   MBR
Restart: Sonos Port, Study (new build abc1234); playback stops briefly
Build record: update installed-build
Apply? [Y/n]
Sonos Port  running   LMS player "Sonos Port (Sonos)" connected
Study       running   LMS player "Study (Sonos)" connected
MBR         running   LMS player "MBR (Sonos)" connected
Logs: journalctl -u 'sonos-lms@*' -f
```

With the same build and settings, the plan instead includes:

```text
Change which rooms are bridged to LMS? [y/N]
Config:  no changes
Keep:    Sonos Port, Study, MBR
Nothing to do.
```

Kept rooms are **not restarted**. Running rooms restart only for a changed or
missing build record, an LMS setting changed in this run, or `--restart`.
`/etc/sonos-lms/installed-build` records the commit (with `-dirty` when applicable)
and binary SHA-256; it is written atomically only after service actions succeed.
The plan lists Start, Restart (with its reason), Stop and Keep, plus any service
enabling, template installation, migration or build-record changes. With no work,
there is no Apply prompt. Declining Apply or pressing Ctrl-C at a prompt changes
no files or services.

Afterward, enabled rooms (including kept rooms) share an LMS CLI check on port
9090, waiting at most 15 seconds for `<room> (Sonos)` with `connected:1`.
A missing host or unreachable CLI skips the check without failing installation;
a running service alone does not prove it reached LMS.

Alternatively, edit `/etc/sonos-lms/config` and run
`sudo scripts/install-devices.sh --non-interactive` (or `sudo make install` and
accept the defaults). Example config:

```ini
# LMS host or IP (no port). Empty = automatic discovery on the local network.
LMS_SERVER=192.0.2.23
# Sonos rooms found on the network.
# yes = bridge this room to LMS, no = ignore it.
room.Sonos Port=yes
room.Study=yes
room.MBR=yes
```

Set a room to `no` and apply to stop/disable its bridge. Existing comments, ordering
and unrelated settings are preserved. The old `rooms` file migrates once to
`room.<name>=yes` settings and is renamed `rooms.migrated` after approval.

Installer flags (`scripts/install-devices.sh`):

| Option | Behaviour |
| --- | --- |
| `--yes` | Apply the displayed plan without prompts; new rooms default to `no`, existing values stay unchanged. |
| `--non-interactive` | Same as `--yes`; automatic when stdin or stdout is not a TTY. |
| `--reconfigure` | Ask about every discovered room on a TTY. Non-interactive flags take precedence. |
| `--restart` | Force restart of every running enabled room; plan reason is `forced`. Stopped enabled rooms are started. |
| `--server=<host>` | Override the saved LMS host (no port, whitespace or control characters). |

Quoted room arguments such as `sudo scripts/install-devices.sh "Sonos Port"`
set those rooms to `yes`. Interactive declines produce no new-room reminder;
non-interactive runs print one combined reminder for newly added disabled rooms.

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

These check the transport and stream state machines, but do not prove behavior
on a real speaker. Use the levels below and [DEVICE-VERIFICATION.md](DEVICE-VERIFICATION.md)
to choose the physical checks appropriate to a change.

### Device test levels

- **Level 0 — no physical test:** display, logging and installer changes. Run `make` and `make test` locally.
- **Level 1 — unattended:** for stream and pause logic changes, run `sudo env AUTO=1 scripts/device-test.sh` on the deployment host. It discovers the room's coordinator, sends AVTransport Pause/Play directly, allows up to ten seconds after resume for both playback clocks to start, then checks speaker and LMS time advance by at least three seconds within the next six seconds, checks continued position and track changes, and monitors transport status and journal errors. It prints a PASS/FAIL table and exits nonzero if any scenario fails. This measures playback progress; it cannot hear audio or inspect app dialogs.
- **Level 2 — real Sonos app:** occasionally run `sudo scripts/device-test.sh` and follow the existing app prompts, especially to confirm audible playback and app behavior. `QUICK=1` keeps this manual flow with shorter defaults.

AUTO and QUICK default to `SCENARIOS="1 2 5 6 7" S2_ROUNDS=1 LONG_PAUSE=30`;
explicit environment values override these defaults. Normal manual mode retains
scenarios 1–7, three S2 rounds and a 120-second long pause. AUTO requires Python 3,
`./sonos-lms --list-rooms --details`, coordinator reachability on port 1400, LMS CLI
access and the room's journal; run it from the repository directory. The bridge
logs `speaker URI: stream=N session=<token>` independently of the displayed title.
Unknown or external URIs do not count as successful stream detection in AUTO.
Agents never SSH to or deploy on LXC 113; the owner runs physical tests there.

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

Stream URLs include a random token generated once per process start:
`/music/squeezebox.flac?session=<token>&stream=<N>`. The startup log prints
`Stream session: <token>`. GET and HEAD requests with a missing or different
token receive an empty 404 with `Connection: close`, before encoder ownership
or device-resume handling. Old URLs cannot match a reused stream ID after restart.

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
LMS position continues across reconnects. LMS stop (`strm q`) ends HTTP immediately
and sends Stop after 400 ms unless superseded by `strm s`; track changes send no
transport command. STOPPED itself never resumes LMS. Sonos-app Play sends one
LMS `play` per resume attempt and feeds the fresh GET; if LMS starts a new stream,
the waiting GET survives LMS's q/s flush and redirects when the new ID exists.
Only a GET classified as a device resume after Stop gets this protection;
ordinary q still ends the response. A held resume sends no bytes if the client
closes it, or returns the existing 503 after its five-second deadline. Each
outcome is logged as `held resume GET #N` followed by `-> 302 stream M`, `fed`,
`closed by client`, or `expired`. Explicit `pause=pause` retains deferred Pause.
The physical script defaults to scenarios 1–7; run S7 alone with
`SCENARIOS=7 scripts/device-test.sh`. The S7 fix still needs a physical recheck.
`LMS=<ip>` overrides host detection. Otherwise the script checks config, recent
unit journal, the unit's `ExecStart --server` (both argument forms), then the
full unit journal, and records the source in its step log.
LMS pause/play intent is retained while a transport call or stream restart is
busy; the latest state is applied once ready, with a deferred-transport log.
PlayStream failures retry up to three attempts, one second apart, then wait for
a new stream or transport command. A stream is complete only after success.
A device-resume request expires after five seconds without LMS `strm u`, allowing
another attempt; CLI errors retain that lease instead of retrying every poll.
No environment overrides are needed. `SONOS_LMS_PAUSE=pause` remains an
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

## The UPnP layer

### yeney — our own UPnP layer

**yeney** (pronounced **YEN-ee**) is our small UPnP discovery and SOAP control
layer. Its palindrome name nods to sonos and noson: the “yes” answer to noson.
It gradually replaces [noson](https://github.com/janbar/noson), the library by
Jean-Luc Barrière that made this bridge possible.

`SONOS_LMS_UPNP=yeney` selects yeney once at startup. `own` remains a permanent
alias so existing drop-ins keep working. **noson remains the default until yeney
has proven itself**; yeney is experimental in phase 1. For example:

```sh
SONOS_LMS_UPNP=yeney ./sonos-lms --room="Sonos Port" --server=192.0.2.10
```

For a service, set `Environment=SONOS_LMS_UPNP=yeney` in its systemd override.
yeney discovers speakers with SSDP, maps room/group topology, and sends SOAP
control directly. Transport state is also polled every 500 ms; slow/failed calls
can extend that interval. Group changes are logged only:
this phase does not coordinate grouped-room transport or LMS sync. The HTTP stream,
artwork and file server still use noson in both modes. Pause defaults to Stop and
all existing stream/session/resume rules remain in effect.

**Events (GENA).** yeney subscribes to the group coordinator's AVTransport
LastChange events, so Sonos-app Play can resume LMS even while the speaker holds
a stream GET and delays SOAP replies. Events update the cache immediately; older
in-flight polls cannot overwrite them. Subscriptions renew halfway through the
granted timeout, follow coordinator changes, and unsubscribe on clean shutdown.
Subscription failures are logged and leave polling available. The separate event
listener uses an ephemeral TCP port on `0.0.0.0`; set `SONOS_LMS_EVENT_PORT` to
choose a fixed callback port.

See [the UPnP layer inventory and wire fixtures](docs/upnp-layer.md). No SMAPI
library service or external-playback ownership policy is enabled by this switch.

## Related

[philippe44/LMS-uPnP](https://github.com/philippe44/LMS-uPnP) (UPnPBridge) is the
mature, general-purpose bridge between LMS and UPnP/DLNA renderers, Sonos being
one of many. It offers both a per-track mode and a continuous "flow" mode; in flow
mode it can show changing titles only with MP3/AAC, because that is the only case
in which Sonos accepts ICY metadata. sonos-lms is Sonos-only by design: always
lossless FLAC, error-free pause/resume from LMS and the Sonos app, Sonos-app
pause/play relayed back to LMS, and a per-room installer. If you need non-Sonos
UPnP renderers or Next/Previous in the Sonos app today, LMS-uPnP is worth a look.

## License

GPL-3.0. See [LICENSE](LICENSE). The vendored `noson` and `squeezelite` submodules
are GPL-3.0 as well; GPL-3.0's copyleft means a derivative or combined work needs a
GPL-compatible outbound license -- no additional, more restrictive terms (e.g. a
noncommercial clause) can be layered on top.

Copyright (C) 2026 Jaap van Vliet
