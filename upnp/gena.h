#pragma once
#include <atomic>
#include <cstdint>
#include <functional>
#include <string>
#include <thread>
namespace upnp {
struct GenaEvent {
    std::string sid, state, status;
    uint32_t sequence = 0;
};
// LastChange is XML text inside a namespaced propertyset. No bridge state.
bool parseLastChange(const std::string& body, GenaEvent& event);
class GenaListener {
public:
    using Handler = std::function<bool(const GenaEvent&)>;
    explicit GenaListener(Handler handler, unsigned port = 0);
    ~GenaListener();
    unsigned port() const { return boundPort; }
    GenaListener(const GenaListener&) = delete;
    GenaListener& operator=(const GenaListener&) = delete;
private:
    void run();
    void serve(int client);
    Handler handler;
    int fd = -1;
    unsigned boundPort = 0;
    std::atomic<bool> stopping{false};
    std::thread worker;
};
}
