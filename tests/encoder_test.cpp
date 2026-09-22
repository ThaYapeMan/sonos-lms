#include "sbencoder.h"
#include <atomic>
#include <cassert>
#include <chrono>
#include <cstring>
#include <future>
#include <iostream>
#include <vector>

static std::atomic<unsigned> generation(1);
static std::atomic<bool> paused(false);
extern "C" unsigned get_squeezebox_stream_id() { return generation.load(); }
extern "C" int sonos_lms_is_paused() { return paused.load(); }
extern "C" uint32_t get_sb_time_ms()
{
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}

int main()
{
    SONOS::SBEncoder old(1);
    char data[16384];
    assert(old.open());
    assert(old.read(data, 4, 10) == 4);
    assert(std::memcmp(data, "fLaC", 4) == 0);
    // Drain metadata: an idle, unpaused encoder must actually time out.
    auto drain = std::async(std::launch::async, [&] {
        while (old.read(data, sizeof(data), 5) > 0) {}
    });
    assert(drain.wait_for(std::chrono::seconds(1)) == std::future_status::ready);
    drain.get();

    paused = true;
    auto held = std::async(std::launch::async, [&] { return old.read(data, sizeof(data), 5); });
    assert(held.wait_for(std::chrono::milliseconds(30)) == std::future_status::timeout);
    paused = false;
    assert(held.wait_for(std::chrono::seconds(1)) == std::future_status::ready);
    assert(held.get() == 0);

    // Invalidating the generation ends the old reader without closing an encoder
    // from another thread. The new generation starts with its own FLAC marker.
    generation = 2;
    assert(old.read(data, sizeof(data), 5) == 0);
    SONOS::SBEncoder fresh(2);
    assert(fresh.open());
    assert(fresh.read(data, 4, 10) == 4);
    assert(std::memcmp(data, "fLaC", 4) == 0);
    old.close();
    assert(fresh.read(data, sizeof(data), 10) > 0);
    std::vector<char> pcm(44100 * 4 * 3, 0);
    assert(fresh.write(pcm.data(), pcm.size(), 10) == (int)pcm.size());
    auto full = std::async(std::launch::async, [&] { return fresh.write(pcm.data(), 4, 5); });
    assert(full.wait_for(std::chrono::seconds(1)) == std::future_status::ready);
    assert(full.get() == 0);
    paused = true;
    auto waitingWriter = std::async(std::launch::async, [&] { return fresh.write(pcm.data(), 4, 5); });
    assert(waitingWriter.wait_for(std::chrono::milliseconds(30)) == std::future_status::timeout);
    fresh.cancel();
    assert(waitingWriter.wait_for(std::chrono::seconds(1)) == std::future_status::ready);
    assert(waitingWriter.get() == 0);
    SONOS::SBEncoder replacement(2);
    assert(replacement.open());
    assert(replacement.read(data, 4, 10, false) == 4);
    assert(std::memcmp(data, "fLaC", 4) == 0);
    // Ending the HTTP response is distinct from cancelling/closing FLAC.
    replacement.retireProducer();
    assert(!replacement.cancelled() && !replacement.responseEnded());
    assert(replacement.read(data, sizeof(data), 100, false) > 0); // header survives handoff
    assert(replacement.write(pcm.data(), 4, 100) == 0);
    replacement.endResponse();
    assert(replacement.responseEnded() && !replacement.cancelled());
    assert(replacement.read(data, sizeof(data), 100, false) == 0);
    assert(replacement.write(pcm.data(), 4, 100) == 0);
    paused = false;
    std::cout << "PASS: FLAC headers, timeouts, pause hold, cancellation, response EOF, same-ID replacement\n";
}
