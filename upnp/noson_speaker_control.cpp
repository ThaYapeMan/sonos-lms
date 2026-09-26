#include "noson_speaker_control.h"
#include "noson_stream_server.h"
#include "sonossystem.h"
#include "discovery.h"
#include <algorithm>
#include <cstdio>
namespace upnp {
struct NosonSpeakerControl::Impl {
    SONOS::System& system;
    SONOS::PlayerPtr player;
    Speaker speaker;
    void (*event)(void*);
    Impl(SONOS::System& s, void (*e)(void*)) : system(s), event(e) {}
};
NosonSpeakerControl::NosonSpeakerControl(NosonStreamServer& server, void (*event)(void*))
    : impl(new Impl(server.system(), event)) {}
NosonSpeakerControl::~NosonSpeakerControl() = default;
bool NosonSpeakerControl::discover(const std::string& room, const std::string& ip) {
    if (!(ip.empty() ? impl->system.Discover() : impl->system.Discover("http://" + ip + ":1400"))) {
        printf("No Sonos devices found\n"); return false;
    }
    for (const auto& entry : impl->system.GetZonePlayerList())
        printf("player %s: %s %s:%u\n", entry.first.c_str(), entry.second->GetUUID().c_str(),
            entry.second->GetHost().c_str(), entry.second->GetPort());
    for (const auto& entry : impl->system.GetZoneList()) {
        const auto zone = entry.second;
        printf("room %s: %s\n", zone->GetZoneName().c_str(), zone->GetCoordinator()->c_str());
        if (zone->GetZoneName() != room) continue;
        impl->player = impl->system.GetPlayer(zone, nullptr, impl->event);
        if (!impl->player) return false;
        auto coordinator = zone->GetCoordinator();
        impl->speaker = {coordinator->GetHost(), coordinator->GetUUID(), zone->GetZoneName(), *coordinator, {}};
        for (const auto& member : *zone) impl->speaker.members.push_back(*member);
    }
    return !!impl->player;
}
std::vector<std::string> NosonSpeakerControl::discoverRooms(const std::string& ip) {
    if (!(ip.empty() ? impl->system.Discover() : impl->system.Discover("http://" + ip + ":1400"))) return {};
    std::vector<std::string> rooms;
    for (const auto& entry : impl->system.GetZonePlayerList()) rooms.push_back(*entry.second);
    return rooms;
}
std::vector<Speaker> NosonSpeakerControl::discoverRoomDetails(const std::string& ip) {
    if (!(ip.empty() ? impl->system.Discover() : impl->system.Discover("http://" + ip + ":1400"))) return {};
    std::vector<Speaker> rooms;
    const auto zones = impl->system.GetZoneList();
    for (const auto& entry : impl->system.GetZonePlayerList()) {
        const auto player = entry.second;
        Speaker room{player->GetHost(), player->GetUUID(), *player, "", {}};
        room.location = player->GetLocation();
        room.model = deviceModel(room.location);
        for (const auto& zone : zones) {
            bool contains = false;
            for (const auto& member : *zone.second) if (member->GetUUID() == room.uuid) contains = true;
            if (!contains) continue;
            room.coordinator = *zone.second->GetCoordinator();
            for (const auto& member : *zone.second) room.members.push_back(*member);
            std::sort(room.members.begin(), room.members.end());
            const auto c = std::find(room.members.begin(), room.members.end(), room.coordinator);
            if (c != room.members.end()) std::rotate(room.members.begin(), c, c + 1);
            break;
        }
        rooms.push_back(room);
    }
    return rooms;
}
Speaker NosonSpeakerControl::speaker() const { return impl->speaker; }
bool NosonSpeakerControl::playStream(const std::string& url, const std::string& title, const std::string& art) { return impl->player->PlayStream(url, title, art); }
bool NosonSpeakerControl::play() { return impl->player->Play(); }
bool NosonSpeakerControl::pause() { return impl->player->Pause(); }
bool NosonSpeakerControl::stop() { return impl->player->Stop(); }
TransportInfo NosonSpeakerControl::transportInfo() {
    TransportInfo info;
    if (!impl->player || impl->player->TransportPropertyEmpty()) return info;
    const auto transport = impl->player->GetTransportProperty();
    info.available = true; info.state = transport.TransportState; info.status = transport.TransportStatus;
    info.duration = transport.CurrentTrackDuration;
    if (transport.CurrentTrackMetaData) {
        info.title = transport.CurrentTrackMetaData->GetValue("dc:title");
        info.album = transport.CurrentTrackMetaData->GetValue("upnp:album");
        info.artist = transport.CurrentTrackMetaData->GetValue("dc:creator");
    }
    return info;
}
uint8_t NosonSpeakerControl::displayVolume() {
    uint8_t volume = 0;
    impl->player->GetVolume(impl->speaker.uuid, &volume);
    return volume;
}
bool NosonSpeakerControl::positionInfo(uint32_t& ms, std::string* text) {
    SONOS::ElementList vars;
    if (!impl->player->GetPositionInfo(vars)) return false;
    const auto rt = vars.GetValue("RelTime");
    unsigned h = 0, m = 0, s = 0;
    ms = sscanf(rt.c_str(), "%u:%u:%u", &h, &m, &s) == 3 ? (h * 3600u + m * 60u + s) * 1000u : 0;
    if (text) *text = rt;
    return true;
}
bool NosonSpeakerControl::currentUri(std::string& uri) {
    SONOS::ElementList vars;
    if (!impl->player->GetMediaInfo(vars)) return false;
    uri = vars.GetValue("CurrentURI"); return true;
}
std::string NosonSpeakerControl::controllerUri() { return impl->player->GetControllerUri(); }
}
