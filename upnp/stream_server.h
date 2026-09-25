#pragma once
#include <functional>
#include <string>

namespace upnp {
class StreamRequest {
public:
    enum class Method { Get, Head, Other };
    virtual ~StreamRequest() = default;
    virtual std::string path() const = 0;
    virtual Method method() const = 0;
    virtual std::string parameter(const std::string& name) const = 0;
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
