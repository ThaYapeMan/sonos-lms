// SONOS::Squeezebox -- deploy Sonos in a Logitech Media Server (LMS) streaming environment
//
// Copyright (C) 2026 Jaap van Vliet
//
// This file is part of SONOS::Squeezebox.
//
// SONOS::Squeezebox is free software: you can redistribute it and/or modify it under the terms of
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

#include <atomic>
#include <cstdint>

static std::atomic<uint32_t> g_sonos_position_ms{0};

void set_sonos_position_ms(uint32_t ms)
{
    g_sonos_position_ms.store(ms, std::memory_order_relaxed);
}

uint32_t get_sonos_position_ms(void)
{
    return g_sonos_position_ms.load(std::memory_order_relaxed);
}
