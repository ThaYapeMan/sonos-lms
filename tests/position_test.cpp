#include "position_state.h"
#include <cassert>
#include <cstdio>
#include <initializer_list>
int main() {
    for (unsigned rate : {44100u, 48000u}) {
    ConnectionPosition p;
    p.connection(1, 1, 100);
    p.pcm(1, 1, 0, 100);
    assert(p.frames(rate) == 0);
    p.poll(p.token(), 17000, 18000);
    assert(p.frames(rate) == 17 * rate);
    const auto oldPoll = p.token();
    p.connection(1, 2, 20000);
    assert(p.frames(rate) == 17 * rate); // hold while waiting for PCM
    p.pcm(1, 2, 18 * rate, 20000);
    assert(p.frames(rate) == 18 * rate);
    p.poll(oldPoll, 17000, 21000); // request crossing the handoff
    p.poll(p.token(), 17000, 20500); // noson's old cached result
    p.poll(p.token(), 17000, 21500); // implausible old connection time
    assert(p.frames(rate) == 18 * rate);
    p.poll(p.token(), 0, 21500);
    assert(p.frames(rate) == 18 * rate);
    p.poll(p.token(), 10000, 31000);
    assert(p.frames(rate) == 28 * rate);
    p.poll(p.token(), 0, 31500); // Stop must not reset the paused position
    assert(p.frames(rate) == 28 * rate);
    p.pcm(1, 2, 99 * rate, 31000); // only the first batch anchors
    assert(p.frames(rate) == 28 * rate);
    p.connection(1, 3, 32000);
    p.pcm(1, 2, 99 * rate, 32000); // obsolete encoder cannot anchor successor
    p.pcm(1, 3, 29 * rate, 32000);
    assert(p.frames(rate) == 29 * rate);
    puts("PASS: same-stream GET anchors to its first PCM; stale/zero RelTime cannot dip below base; latency follows base + RelTime");
    auto old = p.token();
    p.connection(2, 4, 40000);
    p.pcm(2, 4, 0, 40000);
    p.poll(old, 10000, 42000);
    assert(p.frames(rate) == 0);
    p.poll(p.token(), 1000, 42000);
    assert(p.frames(rate) == rate);
    p.reset(2); // strm s / explicit resetPosition even with the same ID
    assert(p.frames(rate) == 0);
    p.connection(2, 5, 44000);
    p.pcm(2, 5, 50 * rate, 44000);
    assert(p.frames(rate) == 0); // first GET after reset always starts at zero
    puts("PASS: new stream and explicit reset discard the old base and polls");
    p.connection(3, 6, 50000);
    p.pcm(3, 6, 0, 50000);
    p.connection(3, 7, 51000);
    const uint64_t base = (uint64_t(1) << 32) + rate;
    p.pcm(3, 7, base, 51000);
    p.poll(p.token(), 1000, 53000);
    assert(p.frames(rate) == base + rate);
    printf("PASS: position anchoring at %u Hz beyond 2^32 frames\n", rate);
    }
}
