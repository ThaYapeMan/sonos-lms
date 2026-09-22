#ifndef STOP_DEBOUNCE_H
#define STOP_DEBOUNCE_H

#include <chrono>

// Caller serializes access. An LMS seek is q followed by s, not a device pause.
class StopDebounce {
public:
    using Clock = std::chrono::steady_clock;
    void schedule(Clock::time_point now) {
        pending = true;
        deadline = now + std::chrono::milliseconds(400);
    }
    bool cancel() {
        bool wasPending = pending;
        pending = false;
        return wasPending;
    }
    bool isDue(Clock::time_point now) const { return pending && now >= deadline; }
    bool takeDue(Clock::time_point now) {
        if (!isDue(now)) return false;
        pending = false;
        return true;
    }
private:
    bool pending = false;
    Clock::time_point deadline;
};
#endif
