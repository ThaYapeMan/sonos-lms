#include "audio_mode.h"
extern "C" int sonos_audio_legacy(void) { return audioMode() == AudioMode::Legacy; }
