#include "upnp/own_speaker_control.h"
#include "upnp/http.h"
#include "upnp/backend.h"
#include <cassert>
#include <chrono>
#include <cstdlib>
#include <iostream>
#include <thread>
int main(int argc, char** argv) {
    setvbuf(stdout, nullptr, _IOLBF, 0);
    assert(argc == 2);
    unsigned port = std::strtoul(argv[1], nullptr, 10);
    upnp::OwnSpeakerControl control([] { return 1450; }, port);
    assert(control.discover("Study", "127.0.0.1"));
    assert(control.controllerUri() == "http://127.0.0.1:1450");
    assert(control.pollIntervalMs() == 500);
    // Cached transport reads must not send SOAP or wait behind a slow SOAP call.
    std::thread slow([&] { assert(!control.pause()); });
    auto begin = std::chrono::steady_clock::now();
    for (unsigned i = 0; i < 1000; ++i) assert(control.transportInfo().state == "PLAYING");
    assert(std::chrono::steady_clock::now() - begin < std::chrono::milliseconds(100));
    slow.join();
    begin = std::chrono::steady_clock::now();
    assert(control.playStream("http://bridge:1400/music/squeezebox.flac?session=0123456789abcdef&stream=7",
        "A & B <Live> \"Mix\" '26", "http://lms:9000/art?a=1&b=2"));
    assert(std::chrono::steady_clock::now() - begin >= std::chrono::seconds(6));
    assert(control.transportInfo().title == "A & B <Live> \"Mix\" '26");
    std::cout << "PASS: own Play accepts a six-second acknowledgement without timeout\n";
    for (const auto action : {"Play", "Pause", "Stop", "SetAVTransportURI"}) assert(upnp::OwnSpeakerControl::actionTimeoutMs(action) == 20000);
    for (const auto action : {"GetTransportInfo", "GetPositionInfo", "GetMediaInfo", "GetVolume", "GetZoneGroupState"}) assert(upnp::OwnSpeakerControl::actionTimeoutMs(action) == 5000);
    uint32_t ms = 0;
    assert(control.positionInfo(ms) && ms == 123000);
    assert(control.positionInfo(ms) && ms == 123000);
    std::string uri;
    assert(control.currentUri(uri) && uri == "http://external/?a=1&b=2");
    assert(control.stop());
    assert(!control.play()); // mock emits a SOAP fault with HTTP 500
    auto response = upnp::httpPost({"127.0.0.1", "/chunked", port}, {}, "");
    assert(response.error.empty() && response.body == "hello world");
    response = upnp::httpPost({"127.0.0.1", "/truncated", port}, {}, "");
    assert(response.error == "truncated HTTP body");
    response = upnp::httpPost({"127.0.0.1", "/bad-chunk", port}, {}, "");
    assert(response.error == "invalid chunk size");
    begin = std::chrono::steady_clock::now();
    response = upnp::httpPost({"127.0.0.1", "/slow", port}, {}, "", 100);
    assert(response.error == "timeout" && std::chrono::steady_clock::now() - begin < std::chrono::milliseconds(500));
    // Re-poll topology after the normal five-second interval, without changing target.
    std::this_thread::sleep_for(std::chrono::seconds(5));
    control.poll();
    assert(control.speaker().coordinator == "Sonos Port" && control.speaker().ip == "127.0.0.1");
    control.poll(); // a rapid second poll must reuse position and volume caches
    assert(control.transportInfo().title == "Title from speaker");
    assert(control.transportInfo().duration == "0:04:56");
    for (unsigned i = 0; i < 1000; ++i) assert(control.displayVolume() == 37);
    std::cout << "PASS: own title fallback, TrackMetaData title, duration and cached volume populated\n";
    std::cout << "PASS: own discovery, SOAP commands, CurrentURI, cached reads, faults, HTTP framing/deadline and group change\n";
}
