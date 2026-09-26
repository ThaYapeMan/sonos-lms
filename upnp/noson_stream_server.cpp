#include "noson_stream_server.h"
#include "sonossystem.h"
#include "imageservice.h"
#include "filestreamer.h"
#include "data/datareader.h"
#include "private/wsrequestbroker.h"
#include "private/wsrequestreply.h"
#include "private/socket.h"
#include "private/tokenizer.h"
#include "private/uriencoder.h"
#include <cerrno>
#include <mutex>
#include <sys/socket.h>

namespace upnp {
namespace {
class Request : public StreamRequest {
    SONOS::WSRequestBroker& broker;
    SONOS::RequestBroker* route;
public:
    explicit Request(SONOS::WSRequestBroker& b, SONOS::RequestBroker* r = nullptr) : broker(b), route(r) {}
    std::string path() const override { return broker.GetRequestPath(); }
    Method method() const override {
        return broker.GetRequestMethod() == WS_METHOD_Get ? Method::Get :
            broker.GetRequestMethod() == WS_METHOD_Head ? Method::Head : Method::Other;
    }
    std::string parameter(const std::string& name) const override {
        std::vector<std::string> params;
        tokenize(broker.GetURIParams(), "&", "", params, true);
        for (const auto& p : params)
            if (p.size() > name.size() + 1 && p.compare(0, name.size() + 1, name + "=") == 0)
                return urldecode(p.substr(name.size() + 1));
        return {};
    }
    RequestHeaders headers() const override {
        RequestHeaders result;
        for (auto header : broker.GetRequestHeaders())
            for (auto value = header.second.cbegin(); value != header.second.cend(); ++value)
                result.emplace_back(header.second.Name(), *value);
        return result;
    }
    bool send(const char* data, size_t size) override { return broker.ReplyData(data, size); }
    bool peerClosed() override {
        auto socket = broker.Socket();
        if (!socket->IsValid()) return true;
        if (socket->GetHandle() < 0) return false;
        char byte;
        int result = recv(socket->GetHandle(), &byte, 1, MSG_PEEK | MSG_DONTWAIT);
        return result == 0 || (result < 0 && errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR);
    }
    void sendTimeout(unsigned ms) override {
        timeval timeout{static_cast<time_t>(ms / 1000), static_cast<suseconds_t>((ms % 1000) * 1000)};
        setsockopt(broker.Socket()->GetHandle(), SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));
    }
    void disconnect() override { broker.Socket()->Disconnect(); }
    void reply(unsigned status, const std::string& type) override {
        SONOS::WSRequestReply reply(broker);
        if (!type.empty()) reply.AddHeader(WS_HEADER_Content_Type, type);
        reply.PostReply(status == 200 ? WS_STATUS_200_OK : WS_STATUS_400_Bad_Request);
    }
    bool aborted() const override { return route && route->IsAborted(); }
    std::string serverName() const override { return "libnoson/" LIBVERSION; }
};
class Route : public SONOS::RequestBroker {
    std::string name;
    ResourcePtr res;
    StreamServer::Handler handler;
public:
    Route(const std::string& n, ResourcePtr r, StreamServer::Handler h) : name(n), res(r), handler(h) {}
    bool HandleRequest(handle* h) override {
        if (IsAborted()) return false;
        Request request(*h->broker, this);
        return handler(request);
    }
    const char* CommonName() override { return name.c_str(); }
    ResourcePtr GetResource(const std::string&) override { return res; }
    ResourceList GetResourceList() override { return {res}; }
    ResourcePtr RegisterResource(const std::string&, const std::string&, const std::string&, SONOS::StreamReader*) override { return {}; }
    void UnregisterResource(const std::string&) override {}
};
}
struct NosonStreamServer::Impl {
    SONOS::System system;
    std::mutex uriMutex;
    SONOS::RequestBrokerPtr images;
    explicit Impl(void (*event)(void*)) : system(nullptr, event), images(new SONOS::ImageService()) {}
};
NosonStreamServer::NosonStreamServer(int debug, void (*event)(void*)) {
    SONOS::System::Debug(debug);
    impl.reset(new Impl(event));
}
NosonStreamServer::~NosonStreamServer() = default;
SONOS::System& NosonStreamServer::system() { return impl->system; }
StreamResource NosonStreamServer::registerStream(const std::string& name, const std::string& path,
    const std::string& description, const std::string& type, const std::string& iconPath, Handler handler) {
    impl->system.RegisterRequestBroker(impl->images);
    auto icon = impl->images->RegisterResource(name, "Icon for " + name, iconPath, SONOS::DataReader::Instance());
    SONOS::RequestBroker::ResourcePtr r(new SONOS::RequestBroker::Resource());
    r->uri = path; r->title = name; r->description = description; r->contentType = type;
    if (icon) r->iconUri = icon->uri + "?id=" LIBVERSION;
    impl->system.RegisterRequestBroker(SONOS::RequestBrokerPtr(new Route(name, r, handler)));
    impl->system.RegisterRequestBroker(SONOS::RequestBrokerPtr(new SONOS::FileStreamer()));
    return {r->uri, r->iconUri};
}
StreamResource NosonStreamServer::resource(const std::string& name) {
    auto broker = impl->system.GetRequestBroker(name);
    auto r = broker ? broker->GetResource(name) : SONOS::RequestBroker::ResourcePtr();
    return r ? StreamResource{r->uri, r->iconUri} : StreamResource{};
}
unsigned NosonStreamServer::port() {
    std::lock_guard<std::mutex> lock(impl->uriMutex);
    const auto& uri = impl->system.GetSystemLocalUri();
    auto colon = uri.rfind(':');
    return colon == std::string::npos ? 0 : std::strtoul(uri.c_str() + colon + 1, nullptr, 10);
}
std::unique_ptr<StreamRequest> nosonRequest(SONOS::WSRequestBroker& request) {
    return std::unique_ptr<StreamRequest>(new Request(request));
}
}
