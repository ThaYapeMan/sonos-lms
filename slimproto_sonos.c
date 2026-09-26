// Compile the upstream implementation unchanged, then replace its strm handler.
#define slimproto slimproto_original
#include "squeezelite/slimproto.c"
#undef slimproto

#include "audio_mode.h"
extern void sonos_lms_transport(char command);

/* Two transport state machines (LMS and device), plus independent stream state:
 * LMS strm p -> device Pause; q -> HTTP EOF, Pause deferred 400 ms (s cancels).
 * LMS strm u -> exactly one action, in priority order:
 *   (1) s arrived while paused (including p/q/s) -> new-stream path;
 *   (2) held GET / bridge-requested device resume -> feed it, no PlayStream;
 *   (3) neither -> PlayStream with the same current URL.
 * A GET must never end without audio unless the client closes it.
 * Exception: a held GET with no PCM after five seconds receives HTTP 503.
 * LMS strm s after stop/seek -> new stream + PlayStream, no Pause/Play.
 * Modern same-rate playlist continuation retains the current FLAC stream.
 * Silence/pause/resume -> same stream ID, NEVER transport intent from output.
 * Device pause -> LMS pause; device play -> LMS play, current GET -> fresh
 * FLAC connection in the SAME generation. Only older IDs redirect to current.
 * New IDs belong only to decoded strm s boundaries, never to HTTP requests.
 * Pause ends HTTP AFTER device Pause (q ends it immediately), retaining the ID.
 * A second same-ID GET never closes its predecessor; only the newest gets PCM.
 * Invariant: no HTTP connection stays silent for more than ~5 seconds in ANY
 * state. Only a new resume GET briefly waits for strm u/PCM; idle responses end.
 * Error recovery must not override either a pending new stream or a held GET.
 * Transport commands are suppressed throughout the PlayStream restart window.
 */
static void sonos_process_strm(u8_t *pkt, int len)
{
    if (len < sizeof(struct strm_packet)) return;
    struct strm_packet *strm = (struct strm_packet *)pkt;
    // A queued next track must not reset the current stream's PCM anchor or
    // interrupt its producer. q/s seeks and starts from pause still restart.
    bool continuous = false;
    if (strm->command == 's') {
        LOCK_O;
        continuous = !sonos_audio_legacy() && output.state == OUTPUT_RUNNING
            && output.frames_played != 0;
        UNLOCK_O;
        sonos_output_new_track(continuous);
    }
    // Nonzero p is a synchronisation delay, not a user pause.
    if (!continuous && (strm->command != 'p' || unpackN(&strm->replay_gain) == 0))
        sonos_lms_transport(strm->command);
    process_strm(pkt, len);
}

void slimproto(log_level level, char *server, u8_t mac[6], const char *name,
               const char *namefile, const char *modelname, int maxSampleRate)
{
    for (struct handler *h = handlers; h->handler; ++h)
        if (!strcmp(h->opcode, "strm")) h->handler = sonos_process_strm;
    slimproto_original(level, server, mac, name, namefile, modelname, maxSampleRate);
}
