#include "upnp/own_speaker_control.h"
#include "upnp/http.h"
#include <atomic>
#include <cassert>
#include <fstream>
#include <iostream>
#include <thread>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <unistd.h>
using namespace upnp;
static std::string sample;
static std::map<std::string, std::string> headers{{"SID", "uuid:test"}, {"NT", "upnp:event"}, {"NTS", "upnp:propchange"}, {"SEQ", "3"}};
static void listenerTest() {
    GenaEvent e;
    assert(parseLastChange(sample, e) && e.state == "PLAYING");
    auto missing = sample;
    auto at = missing.find("InstanceID val=&quot;0&quot;"); assert(at != std::string::npos);
    missing.replace(at, std::string("InstanceID val=&quot;0&quot;").size(), "InstanceID val=&quot;9&quot;");
    assert(!parseLastChange(missing, e));
    assert(!parseLastChange("<broken>", e));
    auto malformedInner = sample;
    const auto eventEnd = malformedInner.find("&lt;/Event&gt;"); assert(eventEnd != std::string::npos);
    malformedInner.erase(eventEnd, std::string("&lt;/Event&gt;").size());
    assert(!parseLastChange(malformedInner, e));
    const std::string delta = "<e:propertyset xmlns:e=\"urn:schemas-upnp-org:event-1-0\"><e:property><LastChange>"
        "&lt;Event&gt;&lt;InstanceID val=\"0\"&gt;&lt;CurrentTransportStatus val=\"OK\"/&gt;&lt;/InstanceID&gt;&lt;/Event&gt;"
        "</LastChange></e:property></e:propertyset>";
    assert(parseLastChange(delta, e) && e.state.empty() && e.status == "OK");
    GenaListener listener([](const GenaEvent& e) { return e.sid == "uuid:test" && e.state == "PLAYING" && e.sequence == 3; });
    const HttpUrl url{"127.0.0.1", "/avt", listener.port()};
    assert(httpRequest("NOTIFY", url, headers, sample).status == 200);
    headers["SID"] = "uuid:wrong";
    assert(httpRequest("NOTIFY", url, headers, sample).status == 412);
    headers["SID"] = "uuid:test";
    assert(httpRequest("NOTIFY", url, headers, missing).status == 412);
    assert(httpRequest("NOTIFY", url, headers, "<broken>").status == 412);
    assert(httpRequest("POST", url, headers, sample).status == 405);
    const int slow = socket(AF_INET, SOCK_STREAM, 0); assert(slow >= 0);
    sockaddr_in address{}; address.sin_family = AF_INET; address.sin_port = htons(listener.port());
    inet_pton(AF_INET, "127.0.0.1", &address.sin_addr);
    timeval timeout{2, 0}; setsockopt(slow, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
    assert(connect(slow, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == 0);
    assert(send(slow, "NOTIFY /avt HTTP/1.1\r\n", 22, 0) == 22);
    auto begin = std::chrono::steady_clock::now();
    char byte; assert(recv(slow, &byte, 1, 0) == 0);
    assert(std::chrono::steady_clock::now() - begin < std::chrono::milliseconds(1500));
    close(slow);
    assert(httpRequest("NOTIFY", url, headers, sample).status == 200);
    std::cout << "PASS: GENA incomplete request deadline releases listener for the next NOTIFY\n";
    std::cout << "PASS: GENA real Sonos LastChange, namespaces, escaped XML, SID, InstanceID, malformed XML and HTTP method\n";
}
int main(int argc, char** argv) {
    setvbuf(stdout, nullptr, _IOLBF, 0);
    std::ifstream file("tests/fixtures/sonos-lastchange.xml");
    sample.assign(std::istreambuf_iterator<char>(file), {});
    assert(!sample.empty());
    if (argc == 1) { listenerTest(); return 0; }
    const std::string mode = argv[1]; const unsigned port = std::stoul(argv[2]);
    std::atomic<unsigned> events{0};
    OwnSpeakerControl control([] { return 1450u; }, port, {}, [&] { ++events; });
    assert(control.discover("Study", "127.0.0.1"));
    if (mode == "lifecycle" || mode == "fallback") {
        std::this_thread::sleep_for(std::chrono::milliseconds(mode == "lifecycle" ? 2600 : 200));
        TransportInfo info; assert(control.readTransportInfo(info) && info.state == "PLAYING");
        if (mode == "lifecycle") {
            std::this_thread::sleep_for(std::chrono::seconds(3));
            control.poll(); // topology change: follow the other coordinator
            std::this_thread::sleep_for(std::chrono::milliseconds(200));
        }
        std::cout << "PASS: GENA " << mode << ": discovery and polling remain available\n";
    } else {
        std::this_thread::sleep_for(std::chrono::milliseconds(150));
        TransportInfo info;
        assert(control.readTransportInfo(info)); // mock sends event while this read is blocked
        assert(events == 1 && info.state == "TRANSITIONING" && info.status == "OK");
        assert(control.transportInfo().state == "TRANSITIONING");
        std::cout << "PASS: GENA event callback and cached TRANSITIONING survive a stale STOPPED SOAP reply\n";
    }
}
