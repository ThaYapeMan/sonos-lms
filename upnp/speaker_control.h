#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace upnp {
struct Speaker {
    std::string ip, uuid, name, coordinator;
    std::vector<std::string> members;
    std::string model{}, location{};
};
struct TransportInfo {
    std::string state, status;
    // Display fields retained so the noson adapter preserves the existing table.
    std::string title, album, artist, duration;
    bool available = false;
};
class SpeakerControl {
public:
    virtual ~SpeakerControl() = default;
    virtual bool discover(const std::string& room, const std::string& seedIp = {}) = 0;
    // Discover visible physical rooms, including non-coordinator group members.
    // Does not select a playback target or issue transport commands.
    virtual std::vector<std::string> discoverRooms(const std::string& seedIp = {}) = 0;
    virtual std::vector<Speaker> discoverRoomDetails(const std::string& seedIp = {}) {
        std::vector<Speaker> result;
        for (const auto& name : discoverRooms(seedIp)) result.push_back({"", "", name, "", {}});
        return result;
    }
    virtual Speaker speaker() const = 0;
    virtual bool playStream(const std::string& url, const std::string& title,
                            const std::string& artUrl = {}) = 0;
    virtual bool play() = 0;
    virtual bool pause() = 0;
    virtual bool stop() = 0;
    // Cached, nonblocking: safe on the HTTP worker. poll() performs own-mode I/O.
    virtual TransportInfo transportInfo() = 0;
    virtual uint8_t displayVolume() { return 0; }
    virtual bool positionInfo(uint32_t& milliseconds, std::string* text = nullptr) = 0;
    virtual bool currentUri(std::string& uri) = 0;
    virtual std::string controllerUri() = 0;
    virtual void poll() {}
    virtual unsigned pollIntervalMs() const { return 0; }
};
}
