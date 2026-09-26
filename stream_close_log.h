#pragma once
#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <map>
#include <mutex>
#include <thread>
#include <vector>

namespace bridge {
// Logging only: socket cleanup never waits for a later device-state observation.
class StreamCloseLog {
    using Clock = std::chrono::steady_clock;
    struct Failure { unsigned stream; double seconds; size_t bytes; int error; Clock::time_point at; bool normal = false; };
    std::mutex mutex;
    std::condition_variable changed;
    std::vector<Failure> pending;
    std::map<unsigned, Clock::time_point> events;
    std::thread worker;
    bool stopping = false;
    static constexpr auto window = std::chrono::seconds(2);
    void run() {
        std::unique_lock<std::mutex> lock(mutex);
        for (;;) {
            if (pending.empty()) {
                if (stopping) return;
                changed.wait(lock);
                continue;
            }
            auto wake = Clock::time_point::max();
            for (auto i = pending.begin(); i != pending.end();) {
                const bool normal = i->normal;
                if (normal || Clock::now() >= i->at + window) {
                    if (normal)
                        printf("stream %u: client closed after %.1f s (%zu bytes confirmed sent; %s; errno=%d)\n",
                               i->stream, i->seconds, i->bytes, strerror(i->error), i->error);
                    else
                        printf("stream %u: send to Sonos failed after %.1f s (%s; errno=%d; "
                               "%zu bytes confirmed sent, partial failed write uncounted)\n",
                               i->stream, i->seconds, strerror(i->error), i->error, i->bytes);
                    i = pending.erase(i);
                } else {
                    wake = std::min(wake, i->at + window);
                    ++i;
                }
            }
            if (!pending.empty()) changed.wait_until(lock, wake);
        }
    }
public:
    ~StreamCloseLog() {
        { std::lock_guard<std::mutex> lock(mutex); stopping = true; }
        changed.notify_one();
        if (worker.joinable()) worker.join();
    }
    void observe(unsigned stream) {
        std::lock_guard<std::mutex> lock(mutex);
        auto now = Clock::now();
        for (auto i = events.begin(); i != events.end();)
            if (now - i->second > window) i = events.erase(i); else ++i;
        for (auto& failure : pending)
            if (failure.stream == stream && now <= failure.at + window) failure.normal = true;
        events[stream] = now;
        changed.notify_one();
    }
    void failed(unsigned stream, double seconds, size_t bytes, int error) {
        std::lock_guard<std::mutex> lock(mutex);
        auto now = Clock::now();
        auto event = events.find(stream);
        pending.push_back({stream, seconds, bytes, error, now,
                           event != events.end() && now - event->second <= window});
        if (!worker.joinable()) worker = std::thread([this] { run(); });
        changed.notify_one();
    }
};
}
