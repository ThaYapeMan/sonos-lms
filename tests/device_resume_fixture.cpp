#include "resume_state.h"
#include "stop_debounce.h"
#include <atomic>
#include <cassert>
#include <cstdio>
#include <mutex>
#include <string>
#include <vector>

static std::atomic<bool> ourStreamStarted{true}, lmsPaused{false};
static std::atomic<unsigned> streamId{6}, lmsStreamSerial{0}, completedStream{6};
static std::mutex resumeMutex, stopMutex, transportMutex;
static ResumeState resumeState;
static StopDebounce deferredStop;
static bool stream_just_restarted() { return false; }
static bool responseOpen = false, responseEnded = false;
static unsigned cliPlays = 0, streamPlays = 0, transportPlays = 0;
static unsigned heldGetInvalidations = 0;
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
    bool Pause() { return true; }
    bool Play() { ++transportPlays; return true; }
} player;
static FakePlayer* gPlayer = &player;
static int gServer = 0, gMac = 0;
static bool squeezebox_response_open(unsigned id) { assert(id == 6); return responseOpen; }
static bool squeezebox_response_ended(unsigned id) { assert(id == 6); return responseEnded; }
static void end_squeezebox_response() { responseOpen = false; responseEnded = true; }
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

#include "production_resume.inc"

static void paused(const char* status) {
    resumeState = ResumeState{};
    deferredStop = StopDebounce{};
    cliPlays = streamPlays = transportPlays = heldGetInvalidations = 0;
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
    assert(heldGetInvalidations == 0 && callOrder.empty());
}

int main() {
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
