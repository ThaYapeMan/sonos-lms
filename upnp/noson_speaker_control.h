#pragma once
#include "speaker_control.h"
#include <memory>
namespace upnp {
class NosonStreamServer;
class NosonSpeakerControl : public SpeakerControl {
public:
    NosonSpeakerControl(NosonStreamServer&, void (*event)(void*));
    ~NosonSpeakerControl() override;
    bool discover(const std::string&, const std::string& = {}) override;
    std::vector<std::string> discoverRooms(const std::string& = {}) override;
    std::vector<Speaker> discoverRoomDetails(const std::string& = {}) override;
    Speaker speaker() const override;
    bool playStream(const std::string&, const std::string&, const std::string& = {}) override;
    bool play() override;
    bool pause() override;
    bool stop() override;
    TransportInfo transportInfo() override;
    uint8_t displayVolume() override;
    bool positionInfo(uint32_t&, std::string* = nullptr) override;
    bool currentUri(std::string&) override;
    std::string controllerUri() override;
private:
    struct Impl;
    std::unique_ptr<Impl> impl;
};
}
