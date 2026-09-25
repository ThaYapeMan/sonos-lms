#ifndef POSITION_STATE_H
#define POSITION_STATE_H
#include <cstdint>

// Caller holds the position mutex. Time is monotonic milliseconds; explicit
// arguments let tests cover connection/poll races without wall-clock sleeps.
class ConnectionPosition {
public:
    void reset(unsigned id) {
        stream = id; request = 0; base = relative = lastFrames = 0;
        anchored = hadAudio = false; ++generation;
    }
    void connection(unsigned id, uint64_t req, uint64_t now) {
        if (id != stream) reset(id);
        request = req;
        // Hold the last reported position until the first PCM offset is known.
        base = lastFrames;
        relative = 0; anchored = false; started = now; ++generation;
        if (!hadAudio) base = 0;
    }
    void pcm(unsigned id, uint64_t req, uint64_t firstFrame, uint64_t now) {
        if (id != stream || req != request || anchored) return;
        base = hadAudio ? firstFrame : 0;
        relative = 0; anchored = hadAudio = true;
        started = now; ++generation;
    }
    uint64_t token() const { return generation; }
    void poll(uint64_t token, uint32_t ms, uint64_t now) {
        if (token != generation || !anchored || ms == 0) return;
        // Stop may report zero before the next GET; retain the last position.
        // A new connection/PCM anchor already clears relative to zero.
        // noson caches GetPositionInfo for one second. Ignore that cache after
        // a handoff, and reject an old RelTime larger than this GET's lifetime.
        if (now - started < 1100 || ms > now - started + 1000) return;
        relative = ms;
    }
    uint64_t frames(uint32_t rate) {
        lastFrames = base + uint64_t(relative) * rate / 1000;
        return lastFrames;
    }
private:
    unsigned stream = 0;
    uint64_t request = 0, generation = 0, base = 0, started = 0, lastFrames = 0;
    uint32_t relative = 0;
    bool anchored = false, hadAudio = false;
};
#endif
