#pragma once
#include "speaker_control.h"
#include <algorithm>
#include <ostream>
namespace upnp {
inline int listRooms(SpeakerControl& control, const std::string& seed,
                     std::ostream& output, std::ostream& errors, bool details = false) {
    if (details) {
        auto rooms = control.discoverRoomDetails(seed);
        std::stable_sort(rooms.begin(), rooms.end(), [](const Speaker& a, const Speaker& b) { return a.name < b.name; });
        auto field = [](std::string value) {
            for (char& c : value) if (c == '\t' || c == '\r' || c == '\n') c = ' ';
            return value.empty() ? std::string("-") : value;
        };
        std::string previous;
        for (const auto& room : rooms) {
            if (room.name.empty() || room.name == previous) continue;
            previous = room.name;
            std::string members;
            for (const auto& member : room.members) { if (!members.empty()) members += ","; members += member; }
            output << field(room.name) << '\t' << field(room.model) << '\t' << field(room.ip)
                   << '\t' << field(room.coordinator) << '\t' << field(members) << '\n';
        }
        if (!previous.empty()) return 0;
        errors << "No Sonos rooms found.\n";
        return 2;
    }
    auto rooms = control.discoverRooms(seed);
    rooms.erase(std::remove(rooms.begin(), rooms.end(), ""), rooms.end());
    std::sort(rooms.begin(), rooms.end());
    rooms.erase(std::unique(rooms.begin(), rooms.end()), rooms.end());
    if (rooms.empty()) {
        errors << "No Sonos rooms found.\n";
        return 2;
    }
    for (const auto& room : rooms) output << room << '\n';
    return 0;
}
}
