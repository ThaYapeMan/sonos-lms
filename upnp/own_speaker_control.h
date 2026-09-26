#pragma once
#include "speaker_control.h"
#include "soap.h"
#include <chrono>
#include <functional>
#include <mutex>
namespace upnp {
struct StreamActivity { bool streaming = false, requestOpen = false; };
class OwnSpeakerControl : public SpeakerControl {
public:
    explicit OwnSpeakerControl(std::function<unsigned()> streamPort, unsigned speakerPort = 1400,
                               std::function<StreamActivity()> activity = {});
    bool discover(const std::string&, const std::string& = {}) override;
    std::vector<std::string> discoverRooms(const std::string& = {}) override;
    std::vector<Speaker> discoverRoomDetails(const std::string& = {}) override;
    Speaker speaker() const override;
    bool playStream(const std::string&, const std::string&, const std::string& = {}) override;
    bool play() override;
    bool pause() override;
    bool stop() override;
    TransportInfo transportInfo() override;
    bool readTransportInfo(TransportInfo&) override;
    uint8_t displayVolume() override;
    static unsigned actionTimeoutMs(const std::string& action);
    bool positionInfo(uint32_t&, std::string* = nullptr) override;
    bool currentUri(std::string&) override;
    std::string controllerUri() override;
    void poll() override;
    unsigned pollIntervalMs() const override { return 500; }
private:
    using Clock = std::chrono::steady_clock;
    std::function<unsigned()> streamPort;
    unsigned speakerPort;
    std::function<StreamActivity()> streamActivity;
    mutable std::mutex cacheMutex;
    // SOAP is called only by status/control threads; HTTP readers take cacheMutex
    // briefly, never a mutex held across socket I/O.
    std::mutex positionMutex;
    Speaker selected;
    TransportInfo cachedTransport;
    std::string localAddress, room, positionText;
    uint32_t positionMs = 0;
    Clock::time_point positionAt{}, topologyAt{}, volumeAt{};
    uint8_t volume = 0;
    std::string sentTitle, sentUri, sentUrl;
    bool freshStreamPosition = false, pauseTimeoutLogged = false;
    bool positionKnown = false;
    bool paused() const;
    SoapResult call(const std::string& action, const SoapArguments& args,
                    const std::string& host = {}, const std::string& service = "AVTransport");
    bool topology(const std::string& host, bool initial);
};
}
