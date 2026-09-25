# Resume, double GET, and seek checks (physical Sonos required)

Build and test locally in WSL2 with `make` and `make test`. LXC 113 is deployment
only: the user pulls, builds, and restarts the service; agents never deploy or
SSH there. Capture a fresh journal throughout this single-track test:

```bash
journalctl -fu 'sonos-lms@Sonos\x20Port.service' -o short-precise
```

A stream N is an HTTP delivery generation established by LMS `strm s`, not a
track. One track may legitimately use several IDs after rewinds, starts, and
seeks. URLs have `?session=<token>&stream=<N>` (or `&session=...` after existing
parameters); the random session is logged once at startup. After a bridge restart,
GET and HEAD of the previous URL, or a URL missing `session`, must return 404 with
`Content-Length: 0` and `Connection: close`, log `stale request: session ...`, and
produce no ACTIVE/STANDBY or device-resume activity. The current session still
serves audio normally. Re-priming a pause-ended response uses the same current URL and ID.

Verified on 2026-09-25: the native-FLAC relay test `120607` completed three
Stop-after-pause resumes with fresh FLAC GETs, status OK, and no reported dialog.
Bridge test `123027` verified LMS and Sonos-app pause/resume with `pause=stop`:
audio continued on the same stream with `strm u`, status OK, no dialog, and
approximately 0.9 s from Play to audio. The earlier `104917` frames/raw experiment
still reported ERROR_CORRUPT_FILE and dialogs. Evidence stays local in `logs/`.
Stop is now the default. The position regression observed in `123027` is addressed
by connection-based PCM anchoring; physically recheck position as described below.

1. **(a) LMS pause, rewind, play — defect I acceptance.** During playback,
   pause in LMS, rewind to the start while still paused, then press LMS play.
   Expect `strm q: superseded by strm s, no Pause` and
   `strm u: new stream pending from strm s, no same-URL resume`. Require exactly
   one new stream ID and one `PlaySqueezeBox` for that ID. There must be no
   PlayStream of the old URL, no `stream mismatch`, no empty GET response, and
   no error flag. Probe/real GETs for the new ID retain client-owned closure:
   the second GET must not make the server close the first.
2. **(b) LMS pause for 30 seconds, then resume — defect F acceptance.** Expect
   `strm p -> UPnP Stop (pause=stop)`, immediate silence, response closure, and
   `OK | STOPPED`. After 30 seconds press LMS play once. Without an open GET,
   expect `strm u after ended response -> PlayStream(same URL)` and a fresh
   FLAC GET for the same ID. Audio must continue without action in the Sonos
   app or a dialog. Repeat three times. Record LMS position before pause and
   after resume: it must continue from the paused position, not from zero.
   After ten seconds it should be approximately ten seconds further along,
   allowing for device startup and buffered-audio latency.
3. **(c) Seek by dragging the LMS bar — defect H acceptance.** During playback,
   drag forward and then backward within the same track. Each q/s burst should
   log `strm q: deferring Stop for 400 ms` followed by
   `strm q: superseded by strm s, no Pause`. Expect exactly one `Creating new
   stream (N)` per seek and probe/real GETs for it. Require playback at the new
   position without an intervening `UPnP Pause` or device stop. Separately issue
   a genuine LMS stop with no following s: expect
   `strm q -> UPnP Stop (400 ms elapsed, pause=stop)` after approximately 400 ms.
   With explicit `SONOS_LMS_PAUSE=pause`, expect UPnP Pause instead. The HTTP
   response ends immediately on q in both cases.
4. **(d) Sonos-app pause/play — defect J acceptance.** Test an immediate Play
   and a full 30-second pause. Expect `Device-initiated pause -> LMS pause`,
   `LMS CLI: <mac> pause 1`, `strm p -> UPnP Stop (pause=stop)`, and clean EOF.
   LMS must stay paused while Sonos reports STOPPED/OK; no restart or LMS play
   is triggered by STOPPED itself. Press Play in the app: the speaker moves to
   TRANSITIONING/PLAYING and opens a fresh GET. Expect one
   `Device-initiated resume: current stream N`, one `LMS CLI: <mac> play`, and
   `strm u: feeding held GET, no same-URL resume`. That GET gets a fresh FLAC
   header and chunked audio, with no 503, server-induced close, or second UPnP
   Play. Require status OK, continued audio and no app dialog. Verify LMS
   position continues from its pre-pause value even while Sonos RelTime is zero.
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
it does not mean STANDBY. In the default Stop mode a detected device resume feeds
that GET normally. It allows five seconds for play → LMS CLI → strm u → PCM,
then returns HTTP 503 if no audio arrives. Pause ends the active response and
silently disconnects existing standbys without promotion. Later GETs can wait
for resume audio on the same ID. With the explicit `SONOS_LMS_PAUSE=pause`
fallback, a detected device resume instead cancels an unfed held GET with 503
and reissues PlayStream with the same URL; active audio is never cancelled by
that hook. This fallback can reproduce the PAUSED FLAC-radio dialog.

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
| Open GET with default Stop mode, or ordinary LMS unpause | Feed that GET; do not issue PlayStream |
| Detected device resume with explicit Pause fallback | Cancel an unfed held GET, then PlayStream with the same URL |
| No open GET | PlayStream with the same URL |

Never more than one action per `u`. An ACTIVE GET must never end without audio
unless its client closes it, except for HTTP 503 after five seconds without PCM
or prompt cancellation of a speculative held GET before a SameURL resume.
STANDBY requests may disconnect silently on client close, pause, or the
30-second safety limit; obsolete stream IDs redirect as described above. An
ended-response flag must not override a pending new stream or an ordinary LMS
unpause into an open GET.

Local regressions cover paused seeks, response-before-Stop/Pause ordering,
STOPPED-to-PLAYING device resume, same-URL fallback, current GET feeding, and
connection position anchoring with stale/zero RelTime. The fallback's cancelled
held GET closes with HTTP 503 within 300 ms before a fresh same-ID GET carries
audio. Wrong-stream invalidation is ignored and active audio is preserved.
Repeat physical checks after changes; local tests cannot establish audible
continuity, app dialogs, or device timing.

**S7: LMS stop, then Sonos-app Play** (`SCENARIOS=7 scripts/device-test.sh`):
expect STOPPED and LMS mode stop after five seconds, with no automatic restart.
Press Play in the app: expect one LMS `play`, fresh FLAC audio on the held GET
(or a redirect to the new stream if LMS restarts the track), no 503, and no app
dialog. Record speaker state, LMS mode and now-playing after 15 seconds. S7 is
included in the default scenarios 1–7; the fix still needs physical verification.
LMS host selection is logged: explicit `LMS`, config, recent unit journal,
unit `ExecStart --server`, then the full unit journal.

S7 evidence from `sonos-test-20260925-160545` (9e47596) showed q-Stop worked,
but LMS's restart q ended held GET #58 with 503 before s allocated stream 11;
audio resumed with ERROR_NO_RESOURCE and an app dialog. Recheck with the fix:
the classified held resume must survive q/s, then log `held resume GET #N -> 302
stream M` or `closed by client`. Same-stream u logs `fed`. Only an unanswered
five-second hold logs `expired` and retains the prior 503 fallback. Local tests
reproduce the gap between q, s and ID allocation for both p-Stop and q-Stop;
physical status and absence of a dialog still require S7 on the speaker.
