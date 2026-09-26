#include "gena.h"
#include "xml.h"
#include <arpa/inet.h>
#include <chrono>
#include <cerrno>
#include <cstring>
#include <map>
#include <poll.h>
#include <stdexcept>
#include <sys/socket.h>
#include <unistd.h>
namespace upnp {
namespace {
std::string localName(const XmlNode& node) {
    const auto colon = node.name.find(':');
    return colon == std::string::npos ? node.name : node.name.substr(colon + 1);
}
}
bool parseLastChange(const std::string& body, GenaEvent& event) {
    XmlNode properties, change;
    if (!parseXml(body, properties) || localName(properties) != "propertyset") return false;
    for (const auto& property : properties.children) {
        if (localName(property) != "property") continue;
        const auto last = property.child("LastChange");
        if (!last || !parseXml(last->text, change) || localName(change) != "Event") continue;
        for (const auto& instance : change.children) {
            if (localName(instance) != "InstanceID" || instance.attribute("val") != "0") continue;
            const auto state = instance.child("TransportState");
            if (state && state->attribute("val").empty()) return false;
            // LastChange is a delta: metadata-only updates are valid NOTIFYs too.
            event.state = state ? state->attribute("val") : "";
            auto status = instance.child("CurrentTransportStatus");
            if (!status) status = instance.child("TransportStatus");
            event.status = status ? status->attribute("val") : "";
            return event.state.find_first_of("\r\n") == std::string::npos
                && event.status.find_first_of("\r\n") == std::string::npos;
        }
    }
    return false;
}
GenaListener::GenaListener(Handler fn, unsigned port) : handler(std::move(fn)) {
    if (port > 65535) throw std::runtime_error("invalid event port");
    fd = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
    if (fd < 0) throw std::runtime_error(strerror(errno));
    sockaddr_in address{}; address.sin_family = AF_INET; address.sin_port = htons(port);
    int reuse = 1; setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
    socklen_t size = sizeof(address);
    if (bind(fd, reinterpret_cast<sockaddr*>(&address), size) || listen(fd, 8)
        || getsockname(fd, reinterpret_cast<sockaddr*>(&address), &size)) {
        const std::string error = strerror(errno); close(fd); fd = -1;
        throw std::runtime_error(error);
    }
    boundPort = ntohs(address.sin_port);
    try { worker = std::thread(&GenaListener::run, this); }
    catch (...) { close(fd); fd = -1; throw; }
}
GenaListener::~GenaListener() {
    stopping = true;
    if (worker.joinable()) worker.join();
    if (fd >= 0) close(fd);
}
void GenaListener::run() {
    while (!stopping) {
        pollfd p{fd, POLLIN, 0};
        if (::poll(&p, 1, 50) <= 0) continue;
        int client = accept4(fd, nullptr, nullptr, SOCK_CLOEXEC | SOCK_NONBLOCK);
        if (client < 0) continue;
        serve(client); close(client);
    }
}
namespace {
bool decimal(const std::string& text, uint64_t& value, uint64_t max) {
    if (text.empty()) return false;
    value = 0;
    for (const char c : text) {
        if (c < '0' || c > '9' || value > (max - (c - '0')) / 10) return false;
        value = value * 10 + c - '0';
    }
    return value <= max;
}
}
void GenaListener::serve(int client) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(750);
    std::string wire;
    auto receive = [&] {
        while (!stopping && std::chrono::steady_clock::now() < deadline) {
            pollfd p{client, POLLIN, 0};
            if (::poll(&p, 1, 25) <= 0) continue;
            char bytes[4096]; const auto n = recv(client, bytes, sizeof(bytes), 0);
            if (n < 0 && (errno == EINTR || errno == EAGAIN)) continue;
            if (n <= 0 || wire.size() + n > 1024 * 1024) return false;
            wire.append(bytes, n); return true;
        }
        return false;
    };
    unsigned status = 412;
    size_t split;
    while ((split = wire.find("\r\n\r\n")) == std::string::npos) {
        if (wire.size() > 16384 || !receive()) return;
    }
    const auto first = wire.find("\r\n");
    if (wire.compare(0, 7, "NOTIFY ")) status = 405;
    else if (wire.substr(0, first) == "NOTIFY /avt HTTP/1.1") {
        std::map<std::string, std::string> headers;
        bool valid = true;
        for (size_t pos = first + 2; pos < split;) {
            const auto end = wire.find("\r\n", pos), colon = wire.find(':', pos);
            if (colon == std::string::npos || colon >= end) { valid = false; break; }
            auto key = wire.substr(pos, colon - pos), value = wire.substr(colon + 1, end - colon - 1);
            for (char& c : key) if (c >= 'A' && c <= 'Z') c += 'a' - 'A';
            auto a = value.find_first_not_of(" \t"), b = value.find_last_not_of(" \t");
            value = a == std::string::npos ? "" : value.substr(a, b - a + 1);
            if (!headers.emplace(key, value).second) valid = false;
            pos = end + 2;
        }
        uint64_t length = 0, seq = 0;
        valid = valid && headers["nt"] == "upnp:event" && headers["nts"] == "upnp:propchange"
            && !headers["sid"].empty() && !headers.count("transfer-encoding")
            && decimal(headers["content-length"], length, 1024 * 1024)
            && decimal(headers["seq"], seq, UINT32_MAX);
        if (valid) {
            while (wire.size() < split + 4 + length) if (!receive()) return;
            GenaEvent event; event.sid = headers["sid"]; event.sequence = seq;
            if (parseLastChange(wire.substr(split + 4, length), event) && handler(event)) status = 200;
        }
    }
    const std::string response = "HTTP/1.1 " + std::to_string(status) + (status == 200 ? " OK" : status == 405 ? " Method Not Allowed" : " Precondition Failed")
        + "\r\nContent-Length: 0\r\nConnection: close\r\n\r\n";
    send(client, response.data(), response.size(), MSG_NOSIGNAL);
}
}
