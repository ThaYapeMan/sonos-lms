// sonos-lms -- deploy Sonos in a Logitech Media Server (LMS) streaming environment
//
// Copyright (C) 2026 Jaap van Vliet
//
// This file is part of sonos-lms.
//
// sonos-lms is free software: you can redistribute it and/or modify it under the terms of
// the GNU General Public License as published by the Free Software Foundation, either version 3
// of the License, or (at your option) any later version.
//
// THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND ANY EXPRESS OR
// IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND
// FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR ANY
// DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
// LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR
// BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT,
// STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
// OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.

#ifndef SONOS_POSITION_H
#define SONOS_POSITION_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// All shared connection/PCM/poll state is protected by one mutex in C++.
void reset_sonos_position(unsigned stream);
void sonos_position_connection(unsigned stream, uint64_t request);
void sonos_position_pcm(unsigned stream, uint64_t request, uint64_t first_frame);
uint64_t sonos_position_poll_token(void);
void set_sonos_position_ms(uint64_t token, uint32_t ms);
uint64_t get_sonos_position_frames(uint32_t sample_rate);

#ifdef __cplusplus
}
#endif

#endif /* SONOS_POSITION_H */
