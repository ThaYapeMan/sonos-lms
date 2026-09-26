#pragma once
#include <functional>
#include <string>
#include <vector>
#include <utility>
#include <cctype>

namespace upnp {
using RequestHeaders = std::vector<std::pair<std::string, std::string>>;
inline std::string streamHeaderLog(const RequestHeaders& headers) {
    std::string result;
    for (const auto& header : headers) {
        std::string name = header.first;
        for (auto& c : name) c = std::tolower(static_cast<unsigned char>(c));
        if (name != "user-agent" && name != "range" && name != "icy-metadata" && name != "connection"
            && name.compare(0, 2, "x-") != 0 && name.find("sonos") == std::string::npos) continue;
        auto value = header.second.substr(0, 80);
        // Keep arbitrary client input on one journal line.
        for (auto& c : value) if (static_cast<unsigned char>(c) < 32 || c == 127) c = '?';
        if (!result.empty()) result += "; ";
        result += header.first + "=" + value;
    }
    return result.empty() ? "(none)" : result;
}

class StreamRequest {
public:
    enum class Method { Get, Head, Other };
    virtual ~StreamRequest() = default;
    virtual std::string path() const = 0;
    virtual Method method() const = 0;
    virtual std::string parameter(const std::string& name) const = 0;
    virtual RequestHeaders headers() const { return {}; }
    virtual bool send(const char* data, size_t size) = 0;
    virtual bool peerClosed() = 0;
    virtual void sendTimeout(unsigned milliseconds) = 0;
    virtual void disconnect() = 0;
    virtual void reply(unsigned status, const std::string& contentType = {}) = 0;
    virtual bool aborted() const { return false; }
    virtual std::string serverName() const = 0;
};
struct StreamResource { std::string uri, iconUri; };
class StreamServer {
public:
    using Handler = std::function<bool(StreamRequest&)>;
    virtual ~StreamServer() = default;
    virtual StreamResource registerStream(const std::string& name, const std::string& path,
        const std::string& description, const std::string& contentType,
        const std::string& iconPath, Handler handler) = 0;
    virtual StreamResource resource(const std::string& name) = 0;
    virtual unsigned port() = 0;
};
}
