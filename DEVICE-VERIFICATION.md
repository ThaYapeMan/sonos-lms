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

Only the newest same-ID connection receives PCM. A genuine older reader stays
open until the client closes or the existing sub-five-second idle limit expires.
Exception: SameURL resume cancels the current held GET before PlayStream when
it has produced no audio and its response has not ended. Active audio is never
cancelled by this hook, and the five-second resume safety deadline is unchanged.
Both normal GETs get headers promptly; a GET arriving while LMS is still paused
allows five seconds for the play → LMS CLI → strm u → PCM round trip. If no
audio arrives in those five seconds, return HTTP 503 as the explicit timeout
exception. Pause ends all current-ID responses cleanly, preserving the ID.
Only an older stream ID redirects directly to the current URL after a restart.

A detected device resume reissues the current URL via PlayStream regardless
of whether a GET is open, unless a new stream is still pending. This also
applies in `ERROR_LOST_CONNECTION`. Verify audio and `OK` without a second
pause/play cycle.

Local tests exercise the real HTTP broker with simulated sockets and decode its
FLAC; they cover both headers, client-owned probe closure, continued new-reader
audio, retained EOF state across reconnects, 30-second pause, idle limits, and
q/s cancellation versus delayed genuine stop. They cannot prove physical Sonos
status, audible stutter, or network command latency.

Resume decision table:

| State at `strm u`, in priority order | Sole action |
| --- | --- |
| New stream still pending after a paused `strm s`, including `p/q/s` | Let the decoded new-stream boundary allocate an ID and call PlaySqueezeBox |
| Bridge requested LMS play for a detected device resume | PlayStream with the same URL, even with a held GET |
| Open GET present without a detected device resume | Feed that GET; do not issue PlayStream |
| No open GET | PlayStream with the same URL |

Never more than one action per `u`. A GET must never end without audio unless
its client closes it, except for HTTP 503 after five seconds without PCM or
prompt cancellation of a speculative held GET before a SameURL resume. An
ended-response flag must not override a pending new stream or an ordinary LMS
unpause into an open GET.

Local regressions cover both paused-seek sequences and device resume with the
old response's EOF flag retained, including a 4.3-second LMS response delay.
Device-resume tests verify that the cancelled held GET closes with HTTP 503
within 300 ms before opening and decoding a fresh same-ID GET. They also
verify wrong-stream invalidation is ignored and active audio is preserved.
Physical acceptance (a)–(e) must still be run by the user; local tests cannot
establish the device's status or confirm audible playback.
