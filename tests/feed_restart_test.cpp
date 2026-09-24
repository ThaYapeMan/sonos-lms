#include "feed_restart.h"
#include <cassert>
#include <cstdio>
using namespace std::chrono;
int main() {
    FeedRestartWatch watch;
    const FeedRestartWatch::Time start{};
    // Reference: GET at t=0, first bytes t=59ms, HEAD t=70ms,
    // client FIN t=72ms, observed STOPPED t=502ms (round 1).
    watch.arm(6, start);
    assert(!watch.observe("TRANSITIONING", 6, start + milliseconds(59)));
    assert(!watch.observe("STOPPED", 6, start + milliseconds(502)));
    assert(!watch.ready(start + milliseconds(701)));
    assert(watch.ready(start + milliseconds(702)));
    watch.claim();
    assert(!watch.observe("STOPPED", 6, start + seconds(1)));
    assert(!watch.ready(start + seconds(1)));
    puts("PASS: captured resume order arms exactly one restart 200ms after STOPPED");

    watch.arm(6, start);
    watch.observe("PLAYING", 6, start);
    assert(!watch.observe("PLAYING", 6, start + milliseconds(1999)));
    assert(watch.observe("PLAYING", 6, start + seconds(2)));
    assert(!watch.active());
    watch.arm(6, start);
    watch.observe("PLAYING", 6, start);
    watch.observe("TRANSITIONING", 6, start + seconds(1));
    watch.observe("PLAYING", 6, start + milliseconds(1100));
    assert(!watch.observe("PLAYING", 6, start + seconds(2)));
    assert(watch.observe("PLAYING", 6, start + milliseconds(3100)));
    puts("PASS: only two continuous seconds of PLAYING disarm the watch");

    watch.arm(6, start);
    assert(watch.observe("STOPPED", 7, start + milliseconds(1)));
    assert(!watch.active());
    watch.arm(6, start);
    assert(watch.observe("PAUSED_PLAYBACK", 6, start + milliseconds(1)));
    watch.arm(6, start);
    assert(!watch.observe("TRANSITIONING", 6, start + milliseconds(4999)));
    assert(watch.observe("STOPPED", 6, start + seconds(5)));
    assert(!watch.active());
    watch.arm(6, start);
    watch.observe("STOPPED", 6, start + milliseconds(50));
    assert(!watch.check(6, start + milliseconds(3249)));
    assert(watch.check(6, start + milliseconds(3250))); // 200 ms delay + 3 s retries
    assert(!watch.active());
    puts("PASS: stream change, device pause, five-second deadline, and three-second retry limit disarm");
}
