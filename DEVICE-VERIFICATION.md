# Resume, double GET, and seek checks (physical Sonos required)

Build and test locally in WSL2 with `make` and `make test`. LXC 113 is deployment
only: the user pulls, builds, and restarts the service; agents never deploy or
SSH there. Capture a fresh journal throughout this single-track test:

```bash
journalctl -fu 'sonos-squeezebox@Sonos\x20Port.service' -o short-precise
```

A stream N is an HTTP delivery generation established by LMS `strm s`, not a
track. One track may legitimately use several IDs after rewinds, starts, and
seeks. Re-priming a pause-ended response uses the same current URL and ID.

1. **(a) LMS pause, rewind, play — defect I acceptance.** During playback,
   pause in LMS, rewind to the start while still paused, then press LMS play.
   Expect `strm q: superseded by strm s, no Pause` and
   `strm u: new stream pending from strm s, no same-URL resume`. Require exactly
   one new stream ID and one `PlaySqueezeBox` for that ID. There must be no
   PlayStream of the old URL, no `stream mismatch`, no empty GET response, and
   no error flag. Probe/real GETs for the new ID retain client-owned closure:
   the second GET must not make the server close the first.
2. **(b) LMS pause for 30 seconds, then resume — defect F acceptance.** During
   playback press LMS pause. Expect `strm p -> UPnP Pause`, immediate silence,
   and `stream N: done` within about five seconds. Wait a full 30 seconds and
   require `OK | PAUSED_PLAYBACK` throughout. Press LMS play once. Expect
   `strm u after ended response -> PlayStream(same URL)`, followed within one
   second by `Sonos requested stream N` and `stream N: serving current generation
   with fresh FLAC header`. Audio must resume without any action in the Sonos
   app. No new ID or 302 is caused by this re-prime. Repeat three times.

   **Known limitation:** resume restarts a few seconds ahead because Sonos drops
   its buffered audio when SetAVTransportURI is reissued. Refinement for later:
   record device RelTime at pause and seek LMS to that position before resuming.
   This change does not implement that compensation. Same-URL PlayStream is
   used when no new stream is pending and either no GET is open or the bridge
   detected a device-initiated resume, even if a GET is already open.
3. **(c) Seek by dragging the LMS bar — defect H acceptance.** During playback,
   drag forward and then backward within the same track. Each q/s burst should
   log `strm q: deferring Pause for 400 ms` followed by
   `strm q: superseded by strm s, no Pause`. Expect exactly one `Creating new
   stream (N)` per seek and probe/real GETs for it. Require playback at the new
   position without an intervening `UPnP Pause` or device stop. Separately issue
   a genuine LMS stop with no following s: expect
   `strm q -> UPnP Pause (400 ms elapsed)` after approximately 400 ms. The HTTP
   response ends immediately on q in both cases.
4. **(d) Sonos-app pause/play — defect J acceptance.** Run this both with an
   immediate play and with a full 30-second pause. Expect
   `Device-initiated pause -> LMS pause`, `LMS CLI: <mac> pause 1`,
   `strm p -> UPnP Pause`, and clean EOF of the response that was playing.
   Verify LMS and HueSync's follower show paused and status remains
   `OK | PAUSED_PLAYBACK`. The device's new GET waits for audio. Press play in
   the Sonos app: expect `Device-initiated resume: current stream N`, exactly
   one `LMS CLI: <mac> play`, and a `PlayStream(same URL)` reissue
   (SetAVTransportURI + Play), even if a GET was already held. Expect a fresh
   `Sonos requested stream N` connection carrying playable audio with the same
   ID. Before PlayStream, expect `stream N: invalidating held GET for same-URL
   resume` for a held request that has not produced audio. It closes through
   the existing HTTP 503 path on the next wait-loop poll (about 10 ms), rather
   than waiting for the five-second deadline. Require status `OK` throughout,
   no HEAD/RST corruption sequence, and no error flag. Repeat several cycles, explicitly
   waiting for a device-opened GET while paused before pressing Sonos play.

   **Known limitation:** a fresh HTTP connection is still used, so position
   drift remains possible. Held-GET cancellation makes the reconnect
   near-immediate at the server (bounded by the roughly 10 ms poll interval,
   not the five-second safety deadline); UPnP and device latency remain.
   Physical testing found that feeding the
   device's own pre-opened connection directly could trigger a HEAD probe,
   RST, and transient `ERROR_CORRUPT_FILE`. Physical retesting confirmed the
   SameURL reconnect removed that error; this cancellation change still needs
   a physical latency retest.
5. **(e) Status and startup isolation.** Require status `OK` throughout (a)-(d),
   with no `ERROR_*` flag at any point, including `ERROR_NO_PLAYABLE_CONTENT`,
   `ERROR_NO_RESOURCE`, `ERROR_LOST_CONNECTION`, and `ERROR_CORRUPT_FILE`.
   Require no `server error (500)` or `device transport command failed`.
   Also restart the bridge while Sonos plays
   its own radio, without starting LMS playback. Startup q commands must log
   `transport ignored before first bridge stream`; radio must not be paused.

Each GET has a unique request ID. For the current stream, exactly one GET is
ACTIVE; extras are STANDBY. Expect `stream N: GET #id ACTIVE` for the first
request and `stream N: GET #id STANDBY` for extras. STANDBY sends no headers or
audio and never replaces the active encoder. If Sonos closes a standby, expect
`standby closed by client` and no bytes written; ACTIVE must continue without
an audio gap. If ACTIVE ends by client close, idle timeout, send error, or
encoder error, the newest standby is `promoted` to ACTIVE with a fresh encoder,
HTTP headers, FLAC header, and audio. Verify both close orders. After 30 seconds
still on standby while ACTIVE streams, expect `standby timeout` and a silent
disconnect. A stream change redirects remaining standbys to the new URL (302)
and cancels the old active stream as before.

This replaces the initial import's (`f1eb715`) unsupported requirement that
both GETs receive headers promptly. Physical tests on 2026-09-24 showed Sonos
opening two same-stream GETs and closing either one within milliseconds; in one
case it closed the newer GET before our response arrived. The original upstream
README also records that rejecting the second request and serving the first
works. These observations support retaining the first active request and keeping
extras on standby until needed, rather than inferring ownership from socket state.

A **held GET** still means an ACTIVE request waiting for PCM while LMS is paused;
it does not mean STANDBY. SameURL resume cancels that held GET before PlayStream
when it has produced no audio and its response has not ended. Active audio is
never cancelled by this hook. The held GET still allows five seconds for the
play → LMS CLI → strm u → PCM round trip, then returns HTTP 503 if no audio
arrives. Pause ends the active response cleanly and disconnects existing
standbys without bytes or promotion, preserving the ID and pause-ended state.
Later GETs may become active and wait for resume audio as before.

A detected device resume reissues the current URL via PlayStream regardless
of whether a GET is open, unless a new stream is still pending. This also
applies in `ERROR_LOST_CONNECTION`. Verify audio and `OK` without a second
pause/play cycle.

Local tests exercise the real HTTP broker with simulated sockets and decode its
FLAC; they cover silent standby closure, uninterrupted active audio, promotion
with fresh FLAC after client close, idle timeout or send failure, newest-request
selection, stream changes, retained EOF state across reconnects, 30-second pause,
standby safety limits, idle limits, and q/s cancellation versus delayed genuine
stop. They cannot prove physical Sonos
status, audible stutter, or network command latency.

Resume decision table:

| State at `strm u`, in priority order | Sole action |
| --- | --- |
| New stream still pending after a paused `strm s`, including `p/q/s` | Let the decoded new-stream boundary allocate an ID and call PlaySqueezeBox |
| Bridge requested LMS play for a detected device resume | PlayStream with the same URL, even with a held GET |
| Open GET present without a detected device resume | Feed that GET; do not issue PlayStream |
| No open GET | PlayStream with the same URL |

Never more than one action per `u`. An ACTIVE GET must never end without audio
unless its client closes it, except for HTTP 503 after five seconds without PCM
or prompt cancellation of a speculative held GET before a SameURL resume.
STANDBY requests may disconnect silently on client close, pause, or the
30-second safety limit; obsolete stream IDs redirect as described above. An
ended-response flag must not override a pending new stream or an ordinary LMS
unpause into an open GET.

Local regressions cover both paused-seek sequences and device resume with the
old response's EOF flag retained, including a 4.3-second LMS response delay.
Device-resume tests verify that the cancelled held GET closes with HTTP 503
within 300 ms before opening and decoding a fresh same-ID GET. They also
verify wrong-stream invalidation is ignored and active audio is preserved.
Physical acceptance (a)–(e) must still be run by the user; local tests cannot
establish the device's status or confirm audible playback.
