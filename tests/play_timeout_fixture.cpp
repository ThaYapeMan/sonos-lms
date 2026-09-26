#include "upnp/own_speaker_control.h"
#include "transport_intent.h"
#include <atomic>
#include <cassert>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <mutex>
#include <thread>
static std::shared_ptr<upnp::SpeakerControl> gPlayer;
static std::atomic<unsigned> streamId{7}, completedStream{0};
static std::atomic<bool> ourStreamStarted{false};
static unsigned retryStream = 0;
static uint64_t retryRevision = 0;
static RetryBudget streamStartRetry;
static TransportIntent transportIntent;
static std::mutex intentMutex, transportMutex;
static int gServer = 0, gMac = 0;
static const char* SBSTREAMER_CNAME = "test";
struct Resource { std::string iconUri = "/art"; };
struct Server { Resource resource(const char*) { return {}; } } server;
static auto gStreamServer = &server;
struct TrackInfo { std::string title = "Delayed track", artworkUrl; };
static TrackInfo fetchLmsTrackInfo(int, int) { return {}; }
static void reset_sonos_position(unsigned) {}
static void acknowledge_squeezebox_resume(unsigned) {}
static std::string SqueezeBoxURL(unsigned) {
    return "http://bridge/music/squeezebox.flac?session=test-session&stream=7";
}
#include "production_play_timeout.inc"
int main(int argc, char** argv) {
    assert(argc == 2);
    setvbuf(stdout, nullptr, _IOLBF, 0);
    gPlayer = std::make_shared<upnp::OwnSpeakerControl>([] { return 1450; }, std::strtoul(argv[1], nullptr, 10));
    assert(gPlayer->discover("Study", "127.0.0.1"));
    dispatchStreamStart();
    std::this_thread::sleep_for(std::chrono::milliseconds(1010));
    dispatchStreamStart();
    assert(completedStream == 7 && ourStreamStarted);
    puts("PASS: production PlayStream start completes with one command after delayed or lost acknowledgement");
}
