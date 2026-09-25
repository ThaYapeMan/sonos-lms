// sbstreamer.h -- HTTP request broker that serves the FLAC stream to Sonos
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

#ifndef SBSTREAMER_H
#define SBSTREAMER_H

#include "upnp/stream_server.h"
#include <atomic>

#include <vector>

#define SBSTREAMER_CNAME "squeezebox"
#define SBSTREAMER_URI "/music/squeezebox.flac"

namespace bridge {

// Registers "/music/squeezebox.flac" as a Sonos-facing HTTP resource and
// serves the live FLAC-encoded PCM squeezelite hands to encode_squeezebox_audio().
class SBStreamer {
public:
    SBStreamer() = default;
    void registerWith(upnp::StreamServer& server);
    bool HandleRequest(upnp::StreamRequest* handle);
    void Abort() { aborted = true; }
    bool IsAborted() const { return aborted.load(); }
private:
    std::atomic<bool> aborted{false};
    void streamSqueezeBox(upnp::StreamRequest* handle, int stream);
};
} // namespace bridge
#endif
