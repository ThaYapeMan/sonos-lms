#include "sonos-status.h"
#include "speaker_uri.h"
#include "stream_session.h"
#include <cassert>

class Speaker : public upnp::SpeakerControl {
public:
    upnp::TransportInfo info;
    bool discover(const std::string&, const std::string&) override { return true; }
    std::vector<std::string> discoverRooms(const std::string&) override { return {}; }
    upnp::Speaker speaker() const override { return {}; }
    bool playStream(const std::string&, const std::string&, const std::string&) override { return true; }
    bool play() override { return true; }
    bool pause() override { return true; }
    bool stop() override { return true; }
    upnp::TransportInfo transportInfo() override { return info; }
    bool positionInfo(uint32_t&, std::string*) override { return false; }
    bool currentUri(std::string&) override { assert(false); return false; }
    std::string controllerUri() override { return {}; }
};
int main() {
    const auto token = streamSessionToken();
    assert(speakerUriDescription("http://bridge/stream?stream=21&session=" + token, token) == "stream=21 session=" + token);
    assert(speakerUriDescription("http://bridge/stream?session=old&stream=21", token) == "other http");
    assert(speakerUriDescription("spotify:secret?session=abc", token) == "other spotify");
    assert(speakerUriDescription("bad\n:secret", token) == "other unknown");
    auto speaker = std::make_shared<Speaker>();
    bridge::Status status(speaker);
    status.update(); // unavailable URI: no invented observation
    speaker->info.uriKnown = true;
    speaker->info.uri = "x-rincon-mp3radio://bridge/stream?session=" + token + "&stream=21";
    status.update(); status.update();
    speaker->info.title = "The real track title";
    status.update(); // title changes must not fabricate a URI change
    speaker->info.uri = "spotify:private";
    status.update(); status.update();
    speaker->info.uri = "";
    status.update(); status.update();
}
