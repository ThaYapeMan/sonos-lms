#include "upnp/own_speaker_control.h"
#include "upnp/http.h"
#include <cassert>
#include <chrono>
#include <cstdio>
#include <map>
#include <thread>
static std::string state = "PLAYING", title = "Speaker title";
static bool timeoutPosition = false, closeRequestOnTimeout = false;
static upnp::StreamActivity activity;
static std::map<std::string, unsigned> calls;
namespace upnp {
bool parseHttpUrl(const std::string&, HttpUrl& url) { url = {"127.0.0.1", "/", 1400}; return true; }
HttpResponse httpGet(const HttpUrl&, unsigned) { return {}; }
HttpResponse httpPost(const HttpUrl&, const std::map<std::string, std::string>& headers,
                      const std::string&, unsigned) {
    auto header = headers.at("SOAPACTION");
    auto action = header.substr(header.find('#') + 1); action.pop_back();
    ++calls[action];
    if (action == "GetPositionInfo" && timeoutPosition) {
        if (closeRequestOnTimeout) activity = {};
        return {0, "", "127.0.0.1", "timeout"};
    }
    SoapArguments fields;
    if (action == "GetZoneGroupState") fields = {{"ZoneGroupState", "<ZoneGroups><ZoneGroup Coordinator=\"id\"><ZoneGroupMember UUID=\"id\" ZoneName=\"Study\" Location=\"http://127.0.0.1:1400/\"/></ZoneGroup></ZoneGroups>"}};
    if (action == "GetTransportInfo") fields = {{"CurrentTransportState", state}, {"CurrentTransportStatus", "OK"}};
    if (action == "GetPositionInfo") fields = {{"RelTime", "0:02:03"}, {"TrackDuration", "0:04:56"},
        {"TrackMetaData", "<DIDL-Lite><item><title>" + xmlEscape(title) + "</title></item></DIDL-Lite>"}};
    if (action == "GetMediaInfo") fields = {{"CurrentURI", "x-rincon-mp3radio://bridge/stream?session=fixture&stream=21"}};
    if (action == "GetVolume") fields = {{"CurrentVolume", "37"}};
    return {200, soapBody("AVTransport", action + "Response", fields), "127.0.0.1", ""};
}
}
int main() {
    upnp::OwnSpeakerControl control([] { return 1400u; }, 1400, [&] { return activity; });
    assert(control.discover("Study", "127.0.0.1"));
    control.poll();
    assert(control.transportInfo().uriKnown);
    assert(control.transportInfo().uri == "x-rincon-mp3radio://bridge/stream?session=fixture&stream=21");
    puts("PASS: own polling caches observed CurrentURI independently of TrackMetaData title");
    unsigned stream = 0;
    uint32_t ms;
    for (const auto variant : {"URL", "basename/query", "basename", "empty", "real title"}) {
        auto url = "http://bridge/music/squeezebox.flac?session=test&stream=" + std::to_string(++stream);
        assert(control.playStream(url, "The track we sent"));
        const std::string kind = variant;
        title = kind == "URL" ? url : kind == "basename/query" ? url.substr(url.rfind('/') + 1)
            : kind == "basename" ? "squeezebox.flac" : kind == "empty" ? "" : "A real device title";
        assert(control.positionInfo(ms) && ms == 123000);
        const auto expected = kind == "real title" ? "A real device title" : "The track we sent";
        assert(control.transportInfo().title == expected);
        printf("Title (%s): %s\n", variant, control.transportInfo().title.c_str());
    }
    puts("PASS: own title uses sent DIDL for URL, basename with/without query and empty; real title wins");
    auto positions = calls["GetPositionInfo"], transports = calls["GetTransportInfo"];
    for (const auto paused : {"STOPPED", "PAUSED_PLAYBACK"}) {
        state = paused;
        control.poll();
        assert(control.positionInfo(ms) && ms == 123000);
        assert(calls["GetPositionInfo"] == positions);
        assert(calls["GetTransportInfo"] == ++transports);
        printf("Polling %s: GetTransportInfo continues; position held at %u ms\n", paused, ms);
    }
    state = "TRANSITIONING";
    auto before = std::chrono::steady_clock::now();
    control.poll();
    assert(control.transportInfo().state == "TRANSITIONING");
    assert(calls["GetPositionInfo"] == ++positions);
    assert(std::chrono::steady_clock::now() - before < std::chrono::milliseconds(control.pollIntervalMs()));
    puts("PASS: paused position polling suppressed; transport polling detects resume in one interval");
    state = "STOPPED"; control.poll();
    assert(control.playStream("http://bridge/music/squeezebox.flac?session=test&stream=new", "New track"));
    assert(control.positionInfo(ms));
    assert(calls["GetPositionInfo"] == ++positions);
    puts("PASS: new stream refreshes position even with a previously stopped transport snapshot");
    // A read started while audio was flowing may time out as the request becomes held.
    activity = {true, true}; timeoutPosition = true;
    std::this_thread::sleep_for(std::chrono::milliseconds(1010));
    assert(!control.positionInfo(ms));
    assert(!control.positionInfo(ms));
    assert(calls["GetPositionInfo"] == positions + 2);
    activity.streaming = false;
    assert(control.positionInfo(ms) && ms == 123000);
    assert(calls["GetPositionInfo"] == positions + 2);
    puts("PASS: held paused request timeout logged once per pause; last position retained");
    state = "PLAYING"; timeoutPosition = false; control.poll();
    state = "PAUSED_PLAYBACK"; control.poll();
    activity.streaming = true; timeoutPosition = true; closeRequestOnTimeout = true;
    std::this_thread::sleep_for(std::chrono::milliseconds(1010));
    assert(!control.positionInfo(ms)); assert(control.positionInfo(ms));
    puts("PASS: request closing during timeout is classified using activity at read start");
    puts("PASS: timeout diagnostic rearms for a later pause");
}
