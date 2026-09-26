#include "discovery.h"
#include "xml.h"
#include <algorithm>
#include <arpa/inet.h>
#include <chrono>
#include <cerrno>
#include <poll.h>
#include <set>
#include <sys/socket.h>
#include <unistd.h>
namespace upnp {
namespace {
std::string trim(const std::string& s) {
    auto first = s.find_first_not_of(" \t\r\n"), last = s.find_last_not_of(" \t\r\n");
    return first == std::string::npos ? "" : s.substr(first, last - first + 1);
}
std::string lower(std::string s) { for (char& c : s) if (c >= 'A' && c <= 'Z') c += 'a' - 'A'; return s; }
constexpr const char* st = "urn:schemas-upnp-org:device:ZonePlayer:1";
}
bool parseSsdpReply(const std::string& reply, HttpUrl& location) {
    if (reply.size() > 65536 || (reply.compare(0, 12, "HTTP/1.1 200") != 0 || reply.size() < 13 || reply[12] != ' ')) return false;
    std::string target, url;
    size_t p = reply.find("\r\n");
    if (p == std::string::npos) return false;
    p += 2;
    while (p < reply.size()) {
        auto end = reply.find("\r\n", p);
        if (end == std::string::npos) return false;
        if (end == p) return target == st && parseHttpUrl(url, location);
        auto colon = reply.find(':', p);
        if (colon == std::string::npos || colon >= end) return false;
        auto key = lower(reply.substr(p, colon - p));
        auto value = trim(reply.substr(colon + 1, end - colon - 1));
        if (key == "st") { if (!target.empty()) return false; target = value; }
        if (key == "location") { if (!url.empty()) return false; url = value; }
        p = end + 2;
    }
    return false;
}
std::vector<HttpUrl> discoverSsdp(unsigned timeoutMs) {
    std::vector<HttpUrl> found;
    if (!timeoutMs) return found;
    int fd = socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
    if (fd < 0) return found;
    sockaddr_in destination{}; destination.sin_family = AF_INET; destination.sin_port = htons(1900);
    inet_pton(AF_INET, "239.255.255.250", &destination.sin_addr);
    const std::string request = "M-SEARCH * HTTP/1.1\r\nHOST: 239.255.255.250:1900\r\nMAN: \"ssdp:discover\"\r\nMX: 2\r\nST: "
        + std::string(st) + "\r\n\r\n";
    std::set<std::string> seen;
    for (unsigned attempt = 0; attempt < 2 && found.empty(); ++attempt) {
        if (sendto(fd, request.data(), request.size(), 0, reinterpret_cast<sockaddr*>(&destination), sizeof(destination)) < 0) break;
        auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
        for (;;) {
            auto left = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now()).count();
            if (left <= 0) break;
            pollfd p{fd, POLLIN, 0};
            int ready = poll(&p, 1, static_cast<int>(left));
            if (ready < 0 && errno == EINTR) continue;
            if (ready <= 0) break;
            char data[65536]; ssize_t n = recv(fd, data, sizeof(data), 0);
            HttpUrl url;
            if (n > 0 && parseSsdpReply(std::string(data, n), url) && seen.insert(url.host).second) found.push_back(url);
        }
    }
    close(fd); return found;
}
std::vector<Speaker> parseTopology(const std::string& xml) {
    XmlNode root;
    if (!parseXml(xml, root)) return {};
    const XmlNode* groups = root.name == "ZoneGroups" ? &root : root.child("ZoneGroups");
    if (!groups) return {};
    std::vector<Speaker> result;
    std::set<std::string> uuids;
    for (const auto& group : groups->children) {
        if (group.name != "ZoneGroup") continue;
        std::vector<Speaker> members;
        std::string coordinator;
        for (const auto& member : group.children) {
            if (member.name != "ZoneGroupMember") continue;
            auto uuid = member.attribute("UUID"), name = member.attribute("ZoneName");
            if (uuid == group.attribute("Coordinator")) coordinator = name;
            if (member.attribute("Invisible") == "1") continue;
            HttpUrl url;
            if (uuid.empty() || name.empty() || !parseHttpUrl(member.attribute("Location"), url) || !uuids.insert(uuid).second) return {};
            members.push_back({url.host, uuid, name, "", {}, "", member.attribute("Location")});
        }
        if (coordinator.empty() || members.empty()) continue;
        std::vector<std::string> names;
        for (const auto& member : members) names.push_back(member.name);
        std::sort(names.begin(), names.end());
        auto c = std::find(names.begin(), names.end(), coordinator);
        if (c != names.end()) std::rotate(names.begin(), c, c + 1);
        for (auto& member : members) {
            member.coordinator = coordinator; member.members = names; result.push_back(member);
        }
    }
    return result;
}
std::string deviceModel(const std::string& location) {
    HttpUrl url;
    if (!parseHttpUrl(location, url)) return {};
    auto response = httpGet(url);
    XmlNode root;
    if (response.status != 200 || !response.error.empty() || !parseXml(response.body, root)) return {};
    const auto device = root.name == "device" ? &root : root.child("device");
    if (!device) return {};
    auto name = device->value("displayName");
    return name.empty() ? device->value("modelName") : name;
}
bool matchRoom(const std::vector<Speaker>& speakers, const std::string& room, Speaker& result) {
    const Speaker* selected = nullptr;
    for (const auto& speaker : speakers) if (speaker.name == room) {
        if (selected) return false; // ambiguous names must not select an arbitrary device
        selected = &speaker;
    }
    if (!selected) for (const auto& speaker : speakers) {
        if (speaker.name != speaker.coordinator) continue;
        std::string combined;
        for (const auto& name : speaker.members) { if (!combined.empty()) combined += " + "; combined += name; }
        if (combined == room) { if (selected) return false; selected = &speaker; }
    }
    if (!selected) return false;
    result = *selected; return true;
}
std::string groupDescription(const Speaker& speaker) {
    if (speaker.name != speaker.coordinator) return "room " + speaker.name + ": member of " + speaker.coordinator + "'s group";
    std::string members;
    for (const auto& name : speaker.members) { if (!members.empty()) members += ", "; members += name; }
    return "room " + speaker.name + ": coordinator of [" + members + "]";
}
}
