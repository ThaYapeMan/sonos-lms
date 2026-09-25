#include "resume_state.h"
#include "transport_intent.h"
#include "pause_mode.h"
#include "stop_debounce.h"
#include "stream_session.h"
#include <atomic>
#include <cassert>
#include <chrono>
#include <cstdarg>
#include <cstdio>
#include <mutex>
#include <string>
#include <vector>
#include <thread>
static std::atomic<bool> ourStreamStarted{true}, lmsPaused{false};
static std::atomic<unsigned> streamId{6}, lmsStreamSerial{0}, completedStream{6};
static std::mutex resumeMutex, stopMutex, transportMutex, intentMutex;
static TransportIntent transportIntent;
static RetryBudget streamStartRetry;
static unsigned retryStream = 0;
static uint64_t retryRevision = 0;
static ResumeState resumeState;
static StopDebounce deferredStop;
static bool stream_just_restarted() { return streamId.load() != completedStream.load(); }
static bool responseOpen = false, responseEnded = false;
static bool pauseResult = true, cliResult = true, testingStreamStart = false;
static unsigned playStreamFailures = 0;
static unsigned pauseCalls = 0, responseEnds = 0;
static unsigned cliPlays = 0, cliPauses = 0, stopCalls = 0;
static std::atomic<unsigned> streamPlays{0};
static std::atomic<unsigned> transportPlays{0};
static unsigned heldGetInvalidations = 0;
static std::vector<std::string> callOrder;
struct Transport { std::string TransportState, TransportStatus; };
struct FakePlayer {
    Transport property;
    Transport GetTransportProperty() { return property; }
    std::string GetControllerUri() { return "http://bridge"; }
    bool PlayStream(const std::string& url, const std::string&, const std::string&) {
        assert(url == "http://bridge/music/squeezebox.flac?session=" + streamSessionToken() + "&stream=6");
        callOrder.push_back("PlayStream");
        if (!testingStreamStart) {
            assert(heldGetInvalidations == streamPlays + 1);
            assert(callOrder.size() >= 2 && callOrder[callOrder.size() - 2] == "invalidate"
                && callOrder.back() == "PlayStream");
        }
        ++streamPlays;
        if (playStreamFailures) { --playStreamFailures; return false; }
        return true;
    }
    bool Stop() {
        assert(!responseOpen && responseEnded && responseEnds == 1);
        assert(resumeState.stoppedForPause(6));
        ++stopCalls;
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
        return true;
    }
} player;
static FakePlayer* gPlayer = &player;
static int gServer = 0, gMac = 0;
static bool squeezebox_response_open(unsigned id) { assert(id == streamId.load()); return responseOpen; }
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
    if (std::string(command) == "pause 1") ++cliPauses;
    return cliResult;
}
// The real PlaySqueezeBoxLocked runs too; only its network/data sources are fake.
#define SBSTREAMER_CNAME "squeezebox"
struct Resource { std::string iconUri = "/icon.png", uri = "/music/squeezebox.flac"; };
namespace SONOS { struct RequestBroker { using ResourcePtr = Resource*; }; }
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
static void reset_sonos_position(unsigned) {}
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
    return result;
}
#define printf productionPrintf
#include "production_resume.inc"
#undef printf

static void paused(const char* status) {
    resumeState = ResumeState{};
    deferredStop = StopDebounce{};
    cliPlays = streamPlays = transportPlays = heldGetInvalidations = 0;
    pauseCalls = responseEnds = 0;
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

#include "stop_pause_cases.h"
#include "transport_intent_cases.h"
#include "retry_cases.h"

int main() {
    assert(SqueezeBoxURL(6) == "http://bridge/music/squeezebox.flac?session=" + streamSessionToken() + "&stream=6");
    systemStub.broker.resource.uri += "?existing=1";
    assert(SqueezeBoxURL(7) == "http://bridge/music/squeezebox.flac?existing=1&session=" + streamSessionToken() + "&stream=7");
    systemStub.broker.resource.uri = "/music/squeezebox.flac";
    puts("PASS: production SqueezeBoxURL includes the process token and preserves existing query parameters");
    transportIntentCases();
    retryCases();
    deferredStopCases();
    if (pauseMode() == PauseMode::Stop) { stopPauseCases(); return 0; }
    // Isolate logging from device I/O, and check heartbeats between every
    // supported transport command rather than only at startup.
    ourStreamStarted = false;
    streamId = 0;
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
    streamId = 6;
    puts("PASS: heartbeat skips the decision log; a/f/p/q/s/u each retain it");
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
