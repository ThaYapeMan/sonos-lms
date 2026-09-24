#ifndef FEED_RESTART_H
#define FEED_RESTART_H

#include <atomic>
#include <chrono>
#include <string>
#include <thread>

// Separate from ResumeState. Caller serializes access; explicit timestamps keep
// the observed radio-resume sequence and all cancellation boundaries testable.
class FeedRestartWatch {
public:
    using Clock = std::chrono::steady_clock;
    using Time = Clock::time_point;

    void arm(unsigned stream, Time now) {
        ++token_;
        stream_ = stream;
        active_ = true;
        stopped_ = playing_ = false;
        deadline_ = now + std::chrono::seconds(5);
    }
    bool active() const { return active_; }
    unsigned long long token() const { return token_; }
    unsigned stream() const { return stream_; }
    Time stoppedAt() const { return stoppedAt_; }
    bool ready(Time now) const {
        return active_ && stopped_ && now >= stoppedAt_ + std::chrono::milliseconds(200);
    }
    const char* cancel(const char* reason) {
        if (!active_) return nullptr;
        active_ = false;
        return reason;
    }
    const char* check(unsigned stream, Time now) {
        if (!active_) return nullptr;
        if (stream != stream_) return cancel("stream id changed");
        if (now >= deadline_) return cancel("5 s deadline expired");
        if (stopped_ && now >= stoppedAt_ + std::chrono::milliseconds(3200))
            return cancel("transport retry expired after 3 s");
        return nullptr;
    }
    const char* observe(const std::string& state, unsigned stream, Time now) {
        if (const char* reason = check(stream, now)) return reason;
        if (!active_) return nullptr;
        if (state == "PAUSED_PLAYBACK") return cancel("device-initiated pause");
        if (state == "PLAYING") {
            if (!playing_) { playing_ = true; playingAt_ = now; }
            else if (now - playingAt_ >= std::chrono::seconds(2))
                return cancel("speaker kept playing");
        } else {
            playing_ = false;
        }
        if (state == "STOPPED" && !stopped_) {
            stopped_ = true;
            stoppedAt_ = now;
        }
        return nullptr;
    }
    // Claim under the watch mutex immediately before issuing the network call.
    // Later STOPPED reports cannot trigger another restart for this arm.
    void claim() { active_ = false; }

private:
    unsigned long long token_ = 0;
    unsigned stream_ = 0;
    bool active_ = false, stopped_ = false, playing_ = false;
    Time deadline_{}, stoppedAt_{}, playingAt_{};
};

// Status events can be sparse once the speaker stops. Poll cached transport
// state and deadlines independently of the main loop's blocking network I/O.
// The worker also retries transportMutex every 10 ms without blocking PCM.
class FeedRestartWorker {
public:
    explicit FeedRestartWorker(void (*tick)()) : worker_([this, tick] {
        while (!stop_.load()) {
            tick();
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
    }) {}
    ~FeedRestartWorker() { stop_ = true; worker_.join(); }
    FeedRestartWorker(const FeedRestartWorker&) = delete;
    FeedRestartWorker& operator=(const FeedRestartWorker&) = delete;
private:
    std::atomic<bool> stop_{false};
    std::thread worker_;
};
#endif
