#ifndef RETRY_BUDGET_H
#define RETRY_BUDGET_H
#include <chrono>

// Caller serializes access. Busy transport locks do not spend an attempt.
class RetryBudget {
public:
    using Clock = std::chrono::steady_clock;
    bool ready(Clock::time_point now) const {
        return !done && attempts < 3 && now >= next;
    }
    void begin() { ++attempts; }
    void failed(Clock::time_point now) { next = now + std::chrono::seconds(1); }
    void succeeded() { done = true; }
    bool exhausted() const { return !done && attempts >= 3; }
    unsigned count() const { return attempts; }
private:
    unsigned attempts = 0;
    bool done = false;
    Clock::time_point next{};
};
#endif
