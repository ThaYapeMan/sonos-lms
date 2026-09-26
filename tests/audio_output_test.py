"""Drive the production output pump and upstream track-boundary handling without sockets."""
from pathlib import Path
import subprocess
import tempfile
root = Path(__file__).resolve().parents[1]
fixture = r'''
#include "output_sonos.c"
#include <assert.h>
struct outputstate output;
struct buffer storage, *outputbuf = &storage;
static unsigned id, published_rate, batches, last_id;
static uint64_t position, last_anchor;
static int legacy;
void wake_controller(void) {}
int sonos_audio_legacy(void) { return legacy; }
unsigned get_squeezebox_stream_id(void) { return id; }
void new_squeezebox_stream_id(void) { ++id; }
void set_squeezebox_audio_rate(unsigned next, unsigned rate) { assert(next == id+1); published_rate = rate; }
uint64_t get_sonos_position_frames(unsigned rate) { return position; }
void encode_squeezebox_audio(const char* data, int len, uint64_t first) {
    assert(len > 0); ++batches; last_id = id; last_anchor = first;
}
// Unused production entry points are discarded by the linker.
int main(int argc, char** argv) {
    legacy = argc > 1;
    unsigned rates[MAX_SUPPORTED_SAMPLERATES] = {0};
    assert(test_open("-", rates, false));
    assert(rates[0] == (legacy ? 44100 : 48000));
    assert(rates[1] == (legacy ? 0 : 44100) && rates[2] == 0);
    buf_init(outputbuf, 1024);
    pcm_staging = malloc(FRAME_BLOCK * BYTES_PER_FRAME);
    frame_size_bytes = legacy ? 4 : 6;
    output.format = legacy ? S16_LE : S24_3LE;
    output.write_cb = _sonos_write_frames;
    output.state = OUTPUT_RUNNING;
    output.current_sample_rate = 44100;
    output.next_sample_rate = 44100;
    output.gainL = output.gainR = FIXED_ONE;
    // Start first track, then consume its four stereo frames.
    memset(outputbuf->buf, 0, 1024);
    outputbuf->writep = outputbuf->readp + 32;
    output.track_start = outputbuf->readp;
    pump_once(); assert(stream_boundary_pending && !id);
    pump_once(); assert(id == 1 && published_rate == 44100 && last_anchor == 0);
    // Normal playlist continuation, same rate: modern preserves ID and anchor.
    sonos_output_new_track(1);
    output.track_start = outputbuf->readp;
    outputbuf->writep += 32;
    pump_once(); pump_once();
    assert(id == (legacy ? 2 : 1));
    assert(last_anchor == (legacy ? 0 : 4));
    // New rate: a distinct stream, rate published before the first PCM.
    sonos_output_new_track(1);
    output.next_sample_rate = 48000;
    output.track_start = outputbuf->readp + 16;
    outputbuf->writep += 48;
    pump_once();
    assert(last_id == (legacy ? 2 : 1) && published_rate == 44100);
    pump_once();
    assert(id == (legacy ? 3 : 2) && published_rate == (legacy ? 44100 : 48000) && last_anchor == 0);
    // Explicit restart at the same rate is never made gapless.
    sonos_output_new_track(0);
    output.track_start = outputbuf->readp;
    outputbuf->writep += 32;
    pump_once(); pump_once(); assert(id == (legacy ? 4 : 3));
    for (unsigned rate = 44100; rate <= 48000; rate += 3900) {
        output.current_sample_rate = rate;
        track_stream_offset = (uint64_t)1 << 32;
        position = (legacy ? 0 : track_stream_offset) + rate;
        output.frames_played_dmp = rate * 2;
        update_device_frames_from_sonos_position();
        assert(output.device_frames == rate);
    }
    puts("PASS: production pump: same-rate continuation, rate-change/new ID, explicit restart, absolute PCM anchors and per-track positions");
    free(pcm_staging); buf_destroy(outputbuf);
}
'''
# Use the real upstream output loop. Its globals replace the fixture declarations.
fixture = fixture.replace('struct outputstate output;\nstruct buffer storage, *outputbuf = &storage;', '')
with tempfile.TemporaryDirectory(prefix='sonos-audio-output-') as tmp:
    source = Path(tmp) / 'output.c'
    source.write_text(fixture)
    exe = Path(tmp) / 'output'
    subprocess.run(['gcc', '-std=gnu11', '-O2', '-ffunction-sections', '-fdata-sections',
                    '-I'+str(root), '-I'+str(root/'squeezelite'), str(source),
                    str(root/'squeezelite/output.c'), str(root/'squeezelite/output_pack.c'),
                    str(root/'squeezelite/buffer.c'), str(root/'squeezelite/utils.c'),
                    '-Wl,--gc-sections', '-lpthread', '-lm', '-o', str(exe)], check=True)
    for args in ([], ['legacy']):
        subprocess.run([str(exe), *args], check=True)

# Exercise the actual Slimproto wrapper's classification of prefetch versus restart.
source_text = (root / 'slimproto_sonos.c').read_text()
start = source_text.index('static void sonos_process_strm(')
end = source_text.index('\nvoid slimproto(', start)
wrapper = source_text[start:end]
with tempfile.TemporaryDirectory(prefix='sonos-audio-strm-') as tmp:
    source = Path(tmp) / 'strm.c'
    exe = Path(tmp) / 'strm'
    source.write_text(r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
typedef unsigned char u8_t;
struct strm_packet { char command; uint32_t replay_gain; };
enum { OUTPUT_RUNNING, OUTPUT_STOPPED };
struct { int state; unsigned frames_played; } output;
#define LOCK_O ((void)0)
#define UNLOCK_O ((void)0)
static int legacy, continuous, notified, processed;
int sonos_audio_legacy(void) { return legacy; }
void sonos_output_new_track(int value) { continuous = value; }
void sonos_lms_transport(char command) { ++notified; }
void process_strm(u8_t* data, int length) { ++processed; }
unsigned unpackN(const void* p) { return *(const uint32_t*)p; }
''' + wrapper + r'''
int main(void) {
    struct strm_packet s = {'s', 0};
    output.state = OUTPUT_RUNNING; output.frames_played = 100;
    sonos_process_strm((u8_t*)&s, sizeof(s));
    assert(continuous && notified == 0 && processed == 1);
    output.state = OUTPUT_STOPPED;
    sonos_process_strm((u8_t*)&s, sizeof(s));
    assert(!continuous && notified == 1 && processed == 2);
    output.state = OUTPUT_RUNNING; legacy = 1;
    sonos_process_strm((u8_t*)&s, sizeof(s));
    assert(!continuous && notified == 2 && processed == 3);
    puts("PASS: Slimproto prefetch keeps PCM anchor; explicit stopped start and legacy retain restart intent");
}
''')
    subprocess.run(['gcc', '-Wall', str(source), '-o', str(exe)], check=True)
    subprocess.run([str(exe)], check=True)
