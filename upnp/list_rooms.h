#pragma once
#include "speaker_control.h"
#include <algorithm>
#include <ostream>
namespace upnp {
inline int listRooms(SpeakerControl& control, const std::string& seed,
                     std::ostream& output, std::ostream& errors) {
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
