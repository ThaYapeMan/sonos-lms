#include "squeezelite.h"
// Exercise the same unity-gain, stereo conversion as output_sonos.c.
void pack_audio_test(void *dst, s32_t *src, unsigned frames, unsigned bits) {
    _scale_and_pack_frames(dst, src, frames, FIXED_ONE, FIXED_ONE, 0,
                           bits == 24 ? S24_3LE : S16_LE);
}
