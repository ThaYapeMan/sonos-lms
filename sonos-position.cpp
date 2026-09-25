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

// Shared Sonos playback position: polled via UPnP in the main loop,
// consumed by the output thread to correct slimproto position reporting.

#include "sonos-position.h"

#include "position_state.h"
#include <chrono>
#include <mutex>

static std::mutex positionMutex;
static ConnectionPosition position;
static uint64_t positionNow() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}
void reset_sonos_position(unsigned stream) {
    std::lock_guard<std::mutex> lock(positionMutex);
    position.reset(stream);
}
void sonos_position_connection(unsigned stream, uint64_t request) {
    std::lock_guard<std::mutex> lock(positionMutex);
    position.connection(stream, request, positionNow());
}
void sonos_position_pcm(unsigned stream, uint64_t request, uint64_t firstFrame) {
    std::lock_guard<std::mutex> lock(positionMutex);
    position.pcm(stream, request, firstFrame, positionNow());
}
uint64_t sonos_position_poll_token(void) {
    std::lock_guard<std::mutex> lock(positionMutex);
    return position.token();
}
void set_sonos_position_ms(uint64_t token, uint32_t ms) {
    std::lock_guard<std::mutex> lock(positionMutex);
    position.poll(token, ms, positionNow());
}
uint64_t get_sonos_position_frames(uint32_t rate) {
    std::lock_guard<std::mutex> lock(positionMutex);
    return position.frames(rate);
}
