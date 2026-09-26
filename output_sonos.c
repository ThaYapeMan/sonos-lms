// output_sonos.c -- squeezelite output-driver entry points for the Sonos backend
//
// Copyright (c) 2026 Jaap van Vliet
//
// Original implementation for the sonos-lms project. Licensed under
// the GNU General Public License, version 3 or (at your option) any later
// version, matching the rest of this project. See LICENSE.
//
// THIS SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.

#include "squeezelite.h"
#include "output_sonos.h"
#include "sonos-position.h"
#include "audio_mode.h"
#include <stdatomic.h>

#if BYTES_PER_FRAME != 8
#error BYTES_PER_FRAME not 8 bytes
#endif

// One decode batch, in frames.
#define FRAME_BLOCK MAX_SILENCE_FRAMES

extern struct outputstate output;
extern struct buffer* outputbuf;

#define LOCK mutex_lock(outputbuf->mutex)
#define UNLOCK mutex_unlock(outputbuf->mutex)

void encode_squeezebox_audio(const char* data, int len, uint64_t first_frame);

static log_level loglevel;
static thread_type pump_thread;
static atomic_bool pump_running = false;
static bool pump_started = false;

// Encoder-facing PCM staging buffer: squeezelite hands us frames a batch at
// a time via _sonos_write_frames(), we accumulate them here, and the pump
// thread drains the batch out to the encoder once per loop iteration.
static u8_t* pcm_staging;
static unsigned pcm_staged_frames;
static uint64_t pcm_first_frame;
static int frame_size_bytes;

static bool backend_was_silent = true;
static bool stream_boundary_pending = false;
static atomic_bool next_track_continuous = false;
static bool boundary_continuous = false;
static unsigned stream_sample_rate;
static uint64_t stream_frames, track_stream_offset;
void sonos_output_new_track(int continuous) {
    atomic_store(&next_track_continuous, continuous != 0);
}

// Diagnostic escape hatch — set DISABLE_SONOS_POSITION_FIX=1 in the systemd
// unit to fall back to device_frames == 0 without rebuilding, in case the
// Sonos-position-derived value below ever needs to be ruled out as a cause
// of a playback-position report. No code path relies on this being unset.
static bool position_fix_disabled = false;

// new_squeezebox_stream_id() / get_squeezebox_stream_id() are defined in
// sonos-lms.cpp, which owns the shared stream-id counter the encoder
// and streamer also read; this file only signals a boundary, never mints
// the id itself.

// squeezelite calls this from its own output loop to hand us the next batch
// of decoded (or silence) frames. We only ever stage real audio here; the
// pump thread is what actually pushes bytes to the encoder.
static int _sonos_write_frames(frames_t out_frames, bool silence, s32_t gainL, s32_t gainR, u8_t flags,
    s32_t cross_gain_in, s32_t cross_gain_out, s32_t** cross_ptr)
{
    if (silence) {
        // A scheduled unpause/sync delay still needs the stream held open,
        // not torn down as if playback had genuinely stopped.
        if (output.state == OUTPUT_START_AT || output.state == OUTPUT_PAUSE_FRAMES)
            return 0;

        if (!backend_was_silent) {
            printf("From non-silent to silent\n");
            backend_was_silent = true;
        }
        return 0;
    }

    if (stream_boundary_pending) {
        const unsigned rate = sonos_audio_legacy() ? 44100 : output.current_sample_rate;
        if (!boundary_continuous || stream_sample_rate != rate || sonos_audio_legacy()) {
            set_squeezebox_audio_rate(get_squeezebox_stream_id() + 1, rate);
            new_squeezebox_stream_id();
            stream_sample_rate = rate;
            stream_frames = track_stream_offset = 0;
            printf("stream %u: FLAC %u-bit %u Hz\n", get_squeezebox_stream_id(),
                   sonos_audio_legacy() ? 16u : 24u, rate);
        } else {
            track_stream_offset = stream_frames;
        }
        stream_boundary_pending = false;
    }
    if (backend_was_silent) {
        printf("From silent to non-silent\n");
        backend_was_silent = false;
    }

    if (output.fade == FADE_ACTIVE && output.fade_dir == FADE_CROSS && *cross_ptr)
        _apply_cross(outputbuf, out_frames, cross_gain_in, cross_gain_out, cross_ptr);

    if (!pcm_staged_frames) pcm_first_frame = sonos_audio_legacy() ? output.frames_played : stream_frames;
    u8_t* decoded = outputbuf->readp;
    _scale_and_pack_frames(pcm_staging + pcm_staged_frames * frame_size_bytes,
        (s32_t*)(void*)decoded, out_frames, FIXED_ONE, FIXED_ONE, 0, output.format);
    pcm_staged_frames += out_frames;
    stream_frames += out_frames;

    return (int)out_frames;
}

uint64_t get_sb_time_ms(void)
{
    struct timespec ts;
    if (!clock_gettime(CLOCK_MONOTONIC, &ts))
        return (uint64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (uint64_t)tv.tv_sec * 1000 + tv.tv_usec / 1000;
}

int sonos_output_running(void)
{
    return atomic_load(&pump_running);
}

// Derives output.device_frames from the Sonos device's own reported playback
// position, so slimproto's ms_played calculation
//   ms_played = (frames_played_dmp - device_frames) * 1000 / sample_rate + (now - updated)
// converges on what Sonos is actually audibly playing rather than on our
// internal decode position, which normally runs well over a second ahead of
// the speaker because of Sonos-side buffering.
static void update_device_frames_from_sonos_position(void)
{
    if (position_fix_disabled || (!sonos_audio_legacy() && stream_boundary_pending)) {
        output.device_frames = 0;
        return;
    }

    u32_t sample_rate = output.current_sample_rate;
    u64_t sonos_frames = get_sonos_position_frames(sample_rate);
    if (!sonos_audio_legacy())
        sonos_frames = sonos_frames > track_stream_offset ? sonos_frames - track_stream_offset : 0;
    u64_t decoded_frames = (u64_t)output.frames_played_dmp;
    output.device_frames = (decoded_frames > sonos_frames) ? (u32_t)(decoded_frames - sonos_frames) : 0;
}

static void pump_once(void)
{
    LOCK;

    output.updated = gettime_ms();
    output.frames_played_dmp = output.frames_played;

    // A transport pause holds the encoder and whatever is already
    // staged, ready for a later unpause, instead of decoding further.
    u8_t* track_start_before = output.track_start;
    if (output.state != OUTPUT_STOPPED)
        _output_frames(FRAME_BLOCK);

    // squeezelite clears track_start at a decoded-track boundary; a
    // prebuffering strm-s or a silence/pause/reconnect gap does not
    // clear it, so this only fires on an actual new track, which is
    // when the next PCM batch decides whether the format needs a new stream.
    if (track_start_before && !output.track_start) {
        stream_boundary_pending = true;
        boundary_continuous = atomic_exchange(&next_track_continuous, false);
        if (!sonos_audio_legacy()) output.frames_played_dmp = 0;
    }

    update_device_frames_from_sonos_position();

    UNLOCK;

    if (pcm_staged_frames) {
        encode_squeezebox_audio((const char*)pcm_staging, pcm_staged_frames * frame_size_bytes, pcm_first_frame);
        pcm_staged_frames = 0;
    }
}

static void* run_pump_thread(void* arg)
{
    (void)arg;

    while (atomic_load(&pump_running)) {
        pump_once();

        usleep(10);
    }

    return NULL;
}

void output_init_sonos(log_level level, unsigned output_buf_size, char* params, unsigned rates[], unsigned rate_delay)
{
    loglevel = level;

    position_fix_disabled = (getenv("DISABLE_SONOS_POSITION_FIX") != NULL);
    if (position_fix_disabled)
        printf("DISABLE_SONOS_POSITION_FIX set: UPnP position fix disabled\n");

    LOG_INFO("init output sonos");

    pcm_staging = malloc(FRAME_BLOCK * BYTES_PER_FRAME);
    if (!pcm_staging) {
        LOG_ERROR("unable to malloc pcm staging buffer");
        return;
    }
    pcm_staged_frames = 0;

    memset(&output, 0, sizeof(output));
    output.format = sonos_audio_legacy() ? S16_LE : S24_3LE;
    output.start_frames = FRAME_BLOCK * 2;
    output.write_cb = &_sonos_write_frames;
    output.rate_delay = rate_delay;

    switch (output.format) {
    case S24_3LE:
        frame_size_bytes = 3 * 2;
        break;
    case S16_LE:
        frame_size_bytes = 2 * 2;
        break;
    case S32_LE:
    default:
        frame_size_bytes = 4 * 2;
        break;
    }

    // Ordered largest first: slimproto advertises supported_rates[0] as MaxSampleRate.
    memset(rates, 0, MAX_SUPPORTED_SAMPLERATES * sizeof(*rates));
    rates[0] = sonos_audio_legacy() ? 44100 : 48000;
    if (!sonos_audio_legacy()) rates[1] = 44100;

    output_init_common(level, "-", output_buf_size, rates, 0);

    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr, PTHREAD_STACK_MIN + OUTPUT_THREAD_STACK_SIZE);
    atomic_store(&pump_running, true);
    int err = pthread_create(&pump_thread, &attr, run_pump_thread, NULL);
    pump_started = err == 0;
    if (err) {
        atomic_store(&pump_running, false);
        LOG_ERROR("unable to start output pump: %s", strerror(err));
    }
    pthread_attr_destroy(&attr);
}

void output_close_sonos(void)
{
    LOG_INFO("close output");

    atomic_store(&pump_running, false);
    if (pump_started) {
        pthread_join(pump_thread, NULL);
        pump_started = false;
    }
    free(pcm_staging);
    pcm_staging = NULL;
    pcm_staged_frames = 0;

    output_close_common();
}

bool test_open(const char* device, unsigned rates[], bool userdef_rates)
{
    (void)device;
    (void)userdef_rates;
    memset(rates, 0, MAX_SUPPORTED_SAMPLERATES * sizeof(*rates));
    rates[0] = sonos_audio_legacy() ? 44100 : 48000;
    if (!sonos_audio_legacy()) rates[1] = 44100;
    return true;
}

void set_volume(unsigned left, unsigned right)
{
    // Sonos volume is driven separately over UPnP (see sonos-status.cpp /
    // sonos-lms.cpp), not through squeezelite's own volume callback.
    (void)left;
    (void)right;
}
