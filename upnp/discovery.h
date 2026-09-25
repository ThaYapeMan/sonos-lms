#pragma once
#include "speaker_control.h"
#include "http.h"
namespace upnp {
bool parseSsdpReply(const std::string& reply, HttpUrl& location);
std::vector<HttpUrl> discoverSsdp(unsigned timeoutMs = 3000);
// One entry per visible room; satellites/invisible bonded members are excluded.
std::vector<Speaker> parseTopology(const std::string& xml);
bool matchRoom(const std::vector<Speaker>& speakers, const std::string& room, Speaker& result);
std::string groupDescription(const Speaker& speaker);
}
