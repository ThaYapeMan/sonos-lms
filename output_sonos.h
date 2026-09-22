// output_sonos.h -- squeezelite output-driver entry points for the Sonos backend
//
// Copyright (c) 2026 Jaap van Vliet
//
// Original implementation for the sonos-squeezebox project. Licensed under
// the GNU General Public License, version 3 or (at your option) any later
// version, matching the rest of this project. See LICENSE.
//
// THIS SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.

#ifndef OUTPUT_SONOS_H
#define OUTPUT_SONOS_H

// Registers the Sonos output backend with squeezelite and starts its
// background pump thread. Mirrors the signature squeezelite expects from
// every output_init_<backend>() implementation.
void output_init_sonos(log_level level, unsigned output_buf_size, char* params, unsigned rates[], unsigned rate_delay);

// Stops the pump thread and releases the backend's resources.
void output_close_sonos(void);

// Allocates a fresh stream id for the encoder, marking a track boundary.
void new_squeezebox_stream_id(void);

// Returns the stream id currently in use by the encoder.
unsigned get_squeezebox_stream_id(void);

#endif  // OUTPUT_SONOS_H
