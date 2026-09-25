#pragma once
#include "stream_server.h"
#include <memory>
namespace SONOS { class System; class WSRequestBroker; }
namespace upnp {
// Testable request adapter; no noson types cross StreamServer/StreamRequest.
std::unique_ptr<StreamRequest> nosonRequest(SONOS::WSRequestBroker& request);
class NosonSpeakerControl;
class NosonStreamServer : public StreamServer {
public:
    NosonStreamServer(int debug, void (*event)(void*));
    ~NosonStreamServer() override;
    StreamResource registerStream(const std::string&, const std::string&, const std::string&,
        const std::string&, const std::string&, Handler) override;
    StreamResource resource(const std::string&) override;
    unsigned port() override;
private:
    friend class NosonSpeakerControl;
    struct Impl;
    std::unique_ptr<Impl> impl;
    SONOS::System& system();
};
}
