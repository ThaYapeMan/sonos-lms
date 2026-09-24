#include "resume_state.h"
#include "device_resume.h"
#include "stop_debounce.h"
#include <atomic>
#include <cassert>
#include <chrono>
#include <cstdarg>
#include <cstdio>
#include <mutex>
#include <string>
#include <vector>
#include <thread>
static std::atomic<bool> lockHeld{false}, releaseLock{false};
static std::atomic<bool> releasePlay{true}, playFinished{false};
static std::string playCompletion;

template<typename Predicate>
static void waitFor(Predicate ready, unsigned timeoutMs = 2000) {
    auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
    while (!ready()) {
        assert(std::chrono::steady_clock::now() < deadline);
        std::this_thread::yield();
    }
}

static std::atomic<bool> ourStreamStarted{true}, lmsPaused{false};
static std::atomic<unsigned> streamId{6}, lmsStreamSerial{0}, completedStream{6};
static std::mutex resumeMutex, stopMutex, transportMutex;
static ResumeState resumeState;
static StopDebounce deferredStop;
static bool stream_just_restarted() { return streamId.load() != completedStream.load(); }
static bool responseOpen = false, responseEnded = false;
static bool pauseResult = true;
static unsigned pauseCalls = 0, responseEnds = 0;
static unsigned cliPlays = 0, streamPlays = 0;
static std::atomic<unsigned> transportPlays{0};
static unsigned heldGetInvalidations = 0, framesResumeMarks = 0;
static void prepare_squeezebox_frames_resume(unsigned id) { assert(id == 6); ++framesResumeMarks; }
static std::vector<std::string> callOrder;
struct Transport { std::string TransportState, TransportStatus; };
struct FakePlayer {
    Transport property;
    Transport GetTransportProperty() { return property; }
    std::string GetControllerUri() { return "http://bridge"; }
    bool PlayStream(const std::string& url, const std::string&, const std::string&) {
        assert(url == "http://bridge/music/squeezebox.flac?stream=6");
        callOrder.push_back("PlayStream");
        assert(heldGetInvalidations == streamPlays + 1);
        assert(callOrder.size() == 2 && callOrder[0] == "invalidate" && callOrder[1] == "PlayStream");
        ++streamPlays;
        return true;
    }
    bool Pause() {
        // Inspect state inside the blocking call, before its result is known.
        assert(!responseOpen && responseEnded && responseEnds == 1);
        ++pauseCalls;
        return pauseResult;
    }
    bool Play() {
        ++transportPlays;
        // Model a device withholding its SOAP reply until PCM is released.
        waitFor([] { return releasePlay.load(); });
        return true;
    }
} player;
static FakePlayer* gPlayer = &player;
static int gServer = 0, gMac = 0;
static bool squeezebox_response_open(unsigned id) { assert(id == 6); return responseOpen; }
static bool squeezebox_response_ended(unsigned id) { assert(id == 6); return responseEnded; }
static void end_squeezebox_response() {
    ++responseEnds;
    responseOpen = false;
    responseEnded = true;
}
static void acknowledge_squeezebox_resume(unsigned id) { assert(id == 6); responseEnded = false; }
static void invalidate_squeezebox_held_get(unsigned id) {
    assert(id == 6);
    ++heldGetInvalidations;
    callOrder.push_back("invalidate");
    responseOpen = false;
}
static bool sendLmsCommand(int, int, const char* command) {
    if (std::string(command) == "play") ++cliPlays;
    return true;
}
// The real PlaySqueezeBoxLocked runs too; only its network/data sources are fake.
#define SBSTREAMER_CNAME "squeezebox"
struct Resource { std::string iconUri = "/icon.png"; };
struct FakeBroker {
    Resource resource;
    Resource* GetResource(const char*) { return &resource; }
};
struct FakeSystem {
    FakeBroker broker;
    FakeBroker* GetRequestBroker(const char*) { return &broker; }
} systemStub;
static FakeSystem* gSonos = &systemStub;
struct TrackInfo { std::string title, artworkUrl; };
static TrackInfo fetchLmsTrackInfo(int, int) { return {"Test track", "http://bridge/art"}; }
static std::string SqueezeBoxURL(unsigned id) {
    assert(id == 6);
    return "http://bridge/music/squeezebox.flac?stream=" + std::to_string(id);
}
static void set_sonos_position_ms(unsigned) { assert(false); } // same-URL resume must not reset
namespace SONOS {
struct Status {
    void update() {}
    std::string getTransportState() { return player.property.TransportState; }
    bool changed() { return false; }
    void print() {}
};
}

// Observe completion, including skipped calls, before resetting fixture state.
static unsigned decisionLogs = 0;
static int productionPrintf(const char* format, ...) {
    char message[1024];
    va_list args;
    va_start(args, format);
    int result = vsnprintf(message, sizeof(message), format, args);
    va_end(args);
    fputs(message, stdout);
    if (std::string(message).find(": decision=") != std::string::npos)
        ++decisionLogs;
    if (std::string(message).find("device resume: strategy=") == 0
        && std::string(message).find(" Play() ") != std::string::npos) {
        playCompletion = message;
        playFinished = true;
    }
    return result;
}
#define printf productionPrintf
#include "production_resume.inc"
#undef printf

static void finishPlay() {
    waitFor([] { return playFinished.load(); });
    // The completion log is inside the transport lock; wait for its release.
    std::lock_guard<std::mutex> lock(transportMutex);
}

static void paused(const char* status) {
    resumeState = ResumeState{};
    deferredStop = StopDebounce{};
    cliPlays = streamPlays = transportPlays = heldGetInvalidations = 0;
    pauseCalls = responseEnds = framesResumeMarks = 0;
    callOrder.clear();
    responseOpen = true;
    responseEnded = false;
    lmsPaused = false;
    resumeState.command('s');
    resumeState.observe("PLAYING");
    sonos_lms_transport('p');
    player.property = {"PAUSED_PLAYBACK", status};
    SONOS::Status snapshot;
    refreshStatus(snapshot);
    assert(lmsPaused && !responseOpen && responseEnded && cliPlays == 0);
    assert(pauseCalls == 1 && responseEnds == 1);
    assert(heldGetInvalidations == 0 && callOrder.empty());
}

int main() {
    // Isolate logging from device I/O, and check heartbeats between every
    // supported transport command rather than only at startup.
    ourStreamStarted = false;
    for (char command : std::string("afpqsu")) {
        unsigned before = decisionLogs;
        sonos_lms_transport('t');
        assert(decisionLogs == before);
        sonos_lms_transport(command);
        assert(decisionLogs == before + 1);
    }
    resumeState = ResumeState{};
    lmsPaused = false;
    ourStreamStarted = true;
    puts("PASS: heartbeat skips the decision log; a/f/p/q/s/u each retain it");
    if (isPlayOnly(deviceResumeStrategy())) {
        for (bool held : {true, false}) {
            paused("OK");
            responseOpen = held;
            player.property.TransportState = "TRANSITIONING";
            ResumeSqueezeBox(6);
            ResumeSqueezeBox(6);
            playFinished = false;
            releasePlay = !held;
            sonos_lms_transport('u');
            assert(cliPlays == 1 && !lmsPaused && !responseEnded);
            if (held) {
                waitFor([] { return transportPlays.load() == 1; });
                assert(!playFinished); // callback returned while Play still awaits PCM
                releasePlay = true; // process_strm can now release PCM into the held GET
                finishPlay();
            }
            assert(transportPlays == (held ? 1u : 0u));
            assert(streamPlays == (held ? 0u : 1u));
            assert(heldGetInvalidations == (held ? 0u : 1u));
            assert(responseOpen == held);
            assert(framesResumeMarks == (held && deviceResumeStrategy() == DeviceResume::PlayOnlyFrames ? 1u : 0u));
        }
        for (const std::string outcome : {"released", "changed", "expired"}) {
            paused("OK");
            responseOpen = true;
            player.property.TransportState = "TRANSITIONING";
            ResumeSqueezeBox(6);
            lockHeld = releaseLock = false;
            std::thread owner([] {
                std::lock_guard<std::mutex> lock(transportMutex);
                lockHeld = true;
                while (!releaseLock) std::this_thread::yield();
            });
            waitFor([] { return lockHeld.load(); });
            playFinished = false;
            auto dispatch = std::chrono::steady_clock::now();
            sonos_lms_transport('u');
            assert(responseOpen && !lmsPaused && transportPlays == 0 && streamPlays == 0 && heldGetInvalidations == 0);
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
            assert(!playFinished && transportPlays == 0);
            if (outcome == "released") {
                releaseLock = true;
            } else {
                if (outcome == "changed") streamId = 7;
                waitFor([] { return playFinished.load(); }, 4000);
                assert(transportPlays == 0);
                if (outcome == "expired")
                    assert(std::chrono::steady_clock::now() - dispatch >= std::chrono::seconds(3));
                releaseLock = true;
            }
            owner.join();
            finishPlay();
            streamId = 6;
            assert(transportPlays == (outcome == "released" ? 1u : 0u));
            const char* expected = outcome == "released" ? "sent after " :
                outcome == "changed" ? "skipped: stream changed" : "skipped: 3 s expired";
            assert(playCompletion.find(expected) != std::string::npos);
            printf("PASS: playonly busy transport %s; callback releases PCM immediately\n", outcome.c_str());
        }
        paused("OK");
        responseOpen = true;
        player.property.TransportState = "TRANSITIONING";
        ResumeSqueezeBox(6);
        completedStream = 5; // same ID, but PlayStream has not finished yet
        playFinished = false;
        sonos_lms_transport('u');
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        assert(!playFinished && transportPlays == 0);
        completedStream = 6;
        finishPlay();
        assert(transportPlays == 1);
        puts("PASS: playonly waits for restart completion, then sends exactly one Play");
        puts("PASS: playonly returns before the Play reply so PCM can flow; sends one Play with held GET; missing GET falls back");
        paused("OK");
        responseOpen = true;
        sonos_lms_transport('u');
        assert(framesResumeMarks == 0 && transportPlays == 0 && streamPlays == 0);
        puts("PASS: ordinary LMS held-GET resume never marks metadata for skipping");
        return 0;
    }

    for (bool success : {true, false}) {
        pauseResult = success;
        paused("OK");
        printf("PASS: ordinary pause ends the response before Pause returns %s\n",
            success ? "success" : "failure");
    }
    pauseResult = true;
    SONOS::Status snapshot;
    for (const char* status : {"ERROR_NO_RESOURCE", "ERROR_LOST_CONNECTION", "OK"}) {
        paused(status);
        player.property.TransportState = "TRANSITIONING";
        refreshStatus(snapshot);
        refreshStatus(snapshot); // repeated event must not send a second play
        assert(cliPlays == 1 && lmsPaused && !responseOpen);
        assert(!resumeState.takeResume(6, 6)); // requested guard is unchanged
        sonos_lms_transport('u');
        assert(streamPlays == 1 && transportPlays == 0 && !lmsPaused);
        assert(!responseEnded && streamId == 6);
        refreshStatus(snapshot);
        assert(cliPlays == 1 && streamPlays == 1);
        printf("PASS: %s without GET sends exactly one LMS play; strm u re-primes the same URL\n", status);
    }

    paused("ERROR_NO_RESOURCE");
    responseOpen = true; // a held HTTP worker now owns the resume
    player.property.TransportState = "TRANSITIONING";
    refreshStatus(snapshot);
    assert(cliPlays == 0);
    ResumeSqueezeBox(6); // production HTTP callback
    ResumeSqueezeBox(6);
    assert(cliPlays == 1);
    sonos_lms_transport('u');
    assert(!responseOpen && !lmsPaused && !responseEnded);
    assert(streamPlays == 1 && transportPlays == 0);
    puts("PASS: open GET does not stop a device resume from reissuing PlayStream");

    paused("ERROR_NO_RESOURCE");
    player.property.TransportState = "TRANSITIONING";
    refreshStatus(snapshot);
    assert(cliPlays == 1);
    responseOpen = true; // GET arrives before the asynchronous LMS strm u
    sonos_lms_transport('u');
    assert(streamPlays == 1 && !responseOpen && !lmsPaused);
    puts("PASS: GET arriving after status resume but before strm u still reissues PlayStream");

    paused("ERROR_NO_RESOURCE");
    responseOpen = true;
    player.property.TransportState = "TRANSITIONING";
    ResumeSqueezeBox(6);
    assert(cliPlays == 1);
    responseOpen = false; // client closes before the asynchronous LMS strm u
    sonos_lms_transport('u');
    assert(streamPlays == 1 && !lmsPaused);
    puts("PASS: GET closed before strm u re-primes instead of feeding a missing response");
    puts("PASS: every SameURL resume invalidates the held GET exactly once before PlayStream");

    paused("OK");
    responseOpen = true; // ordinary LMS unpause, no takeResume
    sonos_lms_transport('u');
    assert(responseOpen && !lmsPaused && streamPlays == 0);
    assert(heldGetInvalidations == 0 && callOrder.empty());
    puts("PASS: ordinary held-GET unpause does not invalidate or reissue PlayStream");

    paused("OK");
    sonos_lms_transport('s');
    sonos_lms_transport('u');
    assert(streamPlays == 0 && heldGetInvalidations == 0 && callOrder.empty());
    puts("PASS: pending new stream does not invalidate a held GET");

}
