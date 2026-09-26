#include "own_speaker_control.h"
#include "discovery.h"
#include "http.h"
#include <cstdio>
#include <limits>
#include <cstdlib>
namespace upnp {
OwnSpeakerControl::OwnSpeakerControl(std::function<unsigned()> port, unsigned controlPort,
                                     std::function<StreamActivity()> activity, std::function<void()> callback)
    : streamPort(std::move(port)), speakerPort(controlPort), streamActivity(std::move(activity)),
      eventCallback(std::move(callback)) {}
OwnSpeakerControl::~OwnSpeakerControl() { shutdownEvents(); }
void OwnSpeakerControl::shutdownEvents() {
    { std::lock_guard<std::mutex> lock(eventMutex); eventsStopping = true; }
    eventWake.notify_all();
    if (subscriptionThread.joinable()) subscriptionThread.join();
    eventListener.reset(); // join all callbacks before cached state is destroyed
}
void OwnSpeakerControl::startEvents() {
    unsigned port = 0;
    if (const char* value = std::getenv("SONOS_LMS_EVENT_PORT")) {
        char* end = nullptr; const auto number = std::strtoul(value, &end, 10);
        if (!*value || *end || number > 65535) printf("UPnP events: invalid SONOS_LMS_EVENT_PORT; using an ephemeral port\n");
        else port = number;
    }
    try {
        eventListener.reset(new GenaListener([this](const GenaEvent& event) { return receiveEvent(event); }, port));
        subscriptionThread = std::thread(&OwnSpeakerControl::subscriptions, this);
    } catch (const std::exception& error) {
        eventListener.reset();
        printf("UPnP events: %s; polling only\n", error.what());
    }
}
void OwnSpeakerControl::subscriptions() {
    std::string host, sid;
    unsigned long grantedTimeout = 0;
    uint64_t target = 0;
    auto due = Clock::now();
    auto unsubscribe = [&] {
        if (!sid.empty()) httpRequest("UNSUBSCRIBE", {host, "/MediaRenderer/AVTransport/Event", speakerPort}, {{"SID", sid}}, "", 500);
        sid.clear();
    };
    for (;;) {
        std::unique_lock<std::mutex> lock(eventMutex);
        eventWake.wait_until(lock, due, [&] { return eventsStopping || target != eventTarget; });
        if (eventsStopping) { lock.unlock(); unsubscribe(); return; }
        if (target != eventTarget) {
            lock.unlock(); unsubscribe(); lock.lock();
            host = eventHost; target = eventTarget;
        }
        const bool renewal = !sid.empty();
        subscribing = !renewal;
        lock.unlock();
        std::string address;
        { std::lock_guard<std::mutex> cache(cacheMutex); address = localAddress; }
        std::map<std::string, std::string> headers{{"TIMEOUT", "Second-3600"}};
        if (renewal) headers["SID"] = sid;
        else {
            headers["NT"] = "upnp:event";
            headers["CALLBACK"] = "<http://" + address + ":" + std::to_string(eventListener->port()) + "/avt>";
        }
        const auto response = httpRequest("SUBSCRIBE", {host, "/MediaRenderer/AVTransport/Event", speakerPort}, headers, "", 2000);
        const auto id = response.headers.find("sid"), timeout = response.headers.find("timeout");
        unsigned long seconds = 0;
        if (timeout != response.headers.end()) {
            const auto text = timeout->second;
            if (text == "Second-infinite") seconds = 3600; // periodically verify even indefinite subscriptions
            else if (text.compare(0, 7, "Second-") == 0) {
                char* end = nullptr; seconds = std::strtoul(text.c_str() + 7, &end, 10);
                if (!*(text.c_str() + 7) || *end || seconds > UINT32_MAX) seconds = 0;
            }
        }
        const bool ok = response.error.empty() && response.status == 200 && id != response.headers.end() && !id->second.empty() && seconds;
        lock.lock();
        subscribing = false;
        if (ok) {
            sid = id->second;
            grantedTimeout = seconds;
            if (target == eventTarget) eventSid = sid;
            due = Clock::now() + std::chrono::milliseconds(uint64_t(grantedTimeout) * 500);
            printf("UPnP events: %s SID=%s timeout=%lu s\n", renewal ? "renewed" : "subscribed", sid.c_str(), seconds);
        } else {
            printf("UPnP events: SUBSCRIBE %s (HTTP %u); polling only%s\n", response.error.empty() ? "failed" : response.error.c_str(),
                   response.status, renewal ? "; subscribing afresh" : "");
            sid.clear();
            if (target == eventTarget) eventSid.clear();
            due = Clock::now() + (renewal ? std::chrono::seconds(0) : std::chrono::seconds(30));
        }
        lock.unlock(); eventWake.notify_all();
    }
}
bool OwnSpeakerControl::receiveEvent(const GenaEvent& event) {
    std::string state;
    {
        std::unique_lock<std::mutex> lock(eventMutex);
        // The initial NOTIFY can race the SUBSCRIBE response's SID publication.
        if (subscribing && eventSid.empty())
            eventWake.wait_for(lock, std::chrono::milliseconds(500), [&] { return !subscribing || eventsStopping; });
        if (eventsStopping || eventSid.empty() || event.sid != eventSid) return false;
        std::lock_guard<std::mutex> cache(cacheMutex);
        ++eventRevision;
        updateTransport(event.state, event.status);
        state = cachedTransport.state;
    }
    printf("UPnP event: TransportState=%s seq=%u\n", state.c_str(), event.sequence);
    if (eventCallback) eventCallback();
    return true;
}
void OwnSpeakerControl::updateTransport(const std::string& state, const std::string& status) {
    const bool wasPaused = paused();
    if (!state.empty()) cachedTransport.state = state;
    if (wasPaused && !paused()) freshStreamPosition = true;
    if (!paused() || !wasPaused) pauseTimeoutLogged = false;
    if (!status.empty()) cachedTransport.status = status;
    cachedTransport.available = true;
}
Speaker OwnSpeakerControl::speaker() const { std::lock_guard<std::mutex> lock(cacheMutex); return selected; }
TransportInfo OwnSpeakerControl::transportInfo() { std::lock_guard<std::mutex> lock(cacheMutex); return cachedTransport; }
std::string OwnSpeakerControl::controllerUri() {
    std::lock_guard<std::mutex> lock(cacheMutex);
    auto port = streamPort();
    return localAddress.empty() || !port ? "" : "http://" + localAddress + ":" + std::to_string(port);
}
unsigned OwnSpeakerControl::actionTimeoutMs(const std::string& action) {
    return action == "Play" || action == "SetAVTransportURI" || action == "Stop" || action == "Pause"
        ? 20000 : 5000;
}
uint8_t OwnSpeakerControl::displayVolume() {
    std::lock_guard<std::mutex> lock(cacheMutex); return volume;
}
bool OwnSpeakerControl::paused() const {
    return cachedTransport.state == "STOPPED" || cachedTransport.state == "PAUSED_PLAYBACK";
}
SoapResult OwnSpeakerControl::call(const std::string& action, const SoapArguments& args,
                                  const std::string& host, const std::string& service) {
    const std::string address = host.empty() ? speaker().ip : host;
    const auto path = service == "ZoneGroupTopology" ? "/ZoneGroupTopology/Control" :
        service == "RenderingControl" ? "/MediaRenderer/RenderingControl/Control" : "/MediaRenderer/AVTransport/Control";
    const auto activityBefore = action == "GetPositionInfo" && streamActivity ? streamActivity() : StreamActivity{};
    auto http = httpPost({address, path, speakerPort}, {
        {"Content-Type", "text/xml"},
        {"SOAPACTION", "\"urn:schemas-upnp-org:service:" + service + ":1#" + action + "\""}
    }, soapBody(service, action, args), actionTimeoutMs(action));
    // Only a connection to the selected speaker determines its callback address.
    if (!http.localAddress.empty() && (host.empty() || speaker().ip.empty())) {
        std::lock_guard<std::mutex> lock(cacheMutex); localAddress = http.localAddress;
    }
    auto result = parseSoap(http.body, action);
    if (!result.faultCode.empty() || !result.faultDescription.empty()) {
        printf("UPnP %s fault %s: %s\n", action.c_str(), result.faultCode.c_str(), result.faultDescription.c_str());
    } else if (!http.error.empty() || http.status != 200 || !result.ok) {
        const auto activity = streamActivity ? streamActivity() : StreamActivity{};
        std::lock_guard<std::mutex> lock(cacheMutex);
        if (action == "GetPositionInfo" && http.error == "timeout" && paused() && (activityBefore.requestOpen || activity.requestOpen)) {
            if (!pauseTimeoutLogged) printf("UPnP GetPositionInfo: no reply while speaker holds a stream request (paused)\n");
            pauseTimeoutLogged = true;
        } else {
            printf("UPnP %s failed: %s (HTTP %u)\n", action.c_str(),
                http.error.empty() ? "invalid SOAP response" : http.error.c_str(), http.status);
        }
    }
    result.ok = result.ok && http.error.empty() && http.status == 200;
    return result;
}
bool OwnSpeakerControl::topology(const std::string& host, bool initial) {
    auto response = call("GetZoneGroupState", {}, host, "ZoneGroupTopology");
    if (!response.ok) return false;
    auto speakers = parseTopology(response.response.value("ZoneGroupState"));
    Speaker next;
    if (initial) {
        if (!matchRoom(speakers, room, next)) return false;
    } else {
        const auto previous = speaker();
        bool found = false;
        for (const auto& candidate : speakers) if (candidate.uuid == previous.uuid) { next = candidate; found = true; break; }
        if (!found) return false;
    }
    bool changed;
    {
        std::lock_guard<std::mutex> lock(cacheMutex);
        changed = selected.name != next.name || selected.coordinator != next.coordinator || selected.members != next.members;
        selected = next;
    }
    std::string coordinatorIp, coordinatorId;
    for (const auto& candidate : speakers) if (candidate.name == next.coordinator) {
        coordinatorIp = candidate.ip; coordinatorId = candidate.uuid; break;
    }
    {
        std::lock_guard<std::mutex> lock(eventMutex);
        if (eventHost != coordinatorIp || eventCoordinator != coordinatorId) {
            eventHost = coordinatorIp; eventCoordinator = coordinatorId;
            ++eventTarget; eventSid.clear(); eventWake.notify_all();
        }
    }
    if (changed) printf("%s\n", groupDescription(next).c_str());
    return true;
}
bool OwnSpeakerControl::discover(const std::string& requestedRoom, const std::string& seed) {
    room = requestedRoom;
    std::vector<HttpUrl> locations;
    if (!seed.empty()) {
        HttpUrl url;
        if (!parseHttpUrl("http://" + seed + ":" + std::to_string(speakerPort), url)) return false;
        locations.push_back(url);
    } else locations = discoverSsdp();
    for (const auto& location : locations) {
        if (!topology(location.host, true)) continue;
        // Probe selected endpoint as well: seed may be reachable on another NIC.
        poll();
        if (!transportInfo().available || controllerUri().empty()) return false;
        if (!eventListener) startEvents();
        return true;
    }
    printf("UPnP: failed to discover room '%s'\n", room.c_str());
    return false;
}
std::vector<std::string> OwnSpeakerControl::discoverRooms(const std::string& seed) {
    std::vector<HttpUrl> locations;
    if (!seed.empty()) {
        HttpUrl url;
        if (!parseHttpUrl("http://" + seed + ":" + std::to_string(speakerPort), url)) return {};
        locations.push_back(url);
    } else locations = discoverSsdp();
    std::vector<std::string> rooms;
    for (const auto& location : locations) {
        const auto result = call("GetZoneGroupState", {}, location.host, "ZoneGroupTopology");
        if (!result.ok) continue;
        for (const auto& speaker : parseTopology(result.response.value("ZoneGroupState")))
            rooms.push_back(speaker.name);
    }
    return rooms;
}
std::vector<Speaker> OwnSpeakerControl::discoverRoomDetails(const std::string& seed) {
    std::vector<HttpUrl> locations;
    if (seed.empty()) locations = discoverSsdp();
    else locations.push_back({seed, "/", speakerPort});
    for (const auto& location : locations) {
        auto result = call("GetZoneGroupState", {}, location.host, "ZoneGroupTopology");
        if (!result.ok) continue;
        auto rooms = parseTopology(result.response.value("ZoneGroupState"));
        if (rooms.empty()) continue;
        for (auto& room : rooms) room.model = deviceModel(room.location);
        return rooms;
    }
    return {};
}
bool OwnSpeakerControl::playStream(const std::string& url, const std::string& title, const std::string& art) {
    if (url.find(':') == std::string::npos) return false;
    const auto metadata = streamDidl(url, title, art);
    XmlNode item;
    if (!parseXml(metadata, item) || !item.child("item")) return false;
    const auto uri = item.child("item")->value("res");
    {
        std::lock_guard<std::mutex> lock(cacheMutex);
        freshStreamPosition = sentUrl != url;
        sentTitle = title; sentUri = uri; sentUrl = url; cachedTransport.title = title;
    }
    return call("SetAVTransportURI", {{"InstanceID", "0"}, {"CurrentURI", uri}, {"CurrentURIMetaData", metadata}}).ok && play();
}
bool OwnSpeakerControl::play() { return call("Play", {{"InstanceID", "0"}, {"Speed", "1"}}).ok; }
bool OwnSpeakerControl::pause() { return call("Pause", {{"InstanceID", "0"}, {"Speed", "1"}}).ok; }
bool OwnSpeakerControl::stop() { return call("Stop", {{"InstanceID", "0"}, {"Speed", "1"}}).ok; }
bool OwnSpeakerControl::currentUri(std::string& uri) {
    auto result = call("GetMediaInfo", {{"InstanceID", "0"}});
    const auto current = result.response.child("CurrentURI");
    if (!result.ok || !current) return false;
    uri = current->text;
    { std::lock_guard<std::mutex> lock(cacheMutex); cachedTransport.uri = uri; cachedTransport.uriKnown = true; }
    return true;
}
bool OwnSpeakerControl::positionInfo(uint32_t& ms, std::string* text) {
    std::lock_guard<std::mutex> lock(positionMutex);
    const auto activity = streamActivity ? streamActivity() : StreamActivity{};
    bool fresh;
    {
        std::lock_guard<std::mutex> cache(cacheMutex);
        fresh = freshStreamPosition;
        if (paused() && !activity.streaming && !fresh) {
            ms = positionMs; if (text && positionKnown) *text = positionText;
            return positionKnown;
        }
        freshStreamPosition = false;
    }
    if (fresh || Clock::now() >= positionAt) {
        auto result = call("GetPositionInfo", {{"InstanceID", "0"}});
        if (!result.ok) return false;
        const auto time = result.response.value("RelTime");
        unsigned long long h, m, s; char tail;
        if (sscanf(time.c_str(), "%llu:%llu:%llu%c", &h, &m, &s, &tail) != 3 || m >= 60 || s >= 60
            || h > std::numeric_limits<uint32_t>::max() / 3600000u
            || (h * 3600 + m * 60 + s) * 1000 > std::numeric_limits<uint32_t>::max()) return false;
        {
            std::lock_guard<std::mutex> lock(cacheMutex);
            cachedTransport.duration = result.response.value("TrackDuration");
            XmlNode metadata;
            const auto xml = result.response.value("TrackMetaData");
            const auto valid = parseXml(xml, metadata);
            const auto item = valid ? metadata.child("item") : nullptr;
            const auto title = item ? item->value("title") : "";
            const auto query = sentUrl.find('?');
            const auto baseStart = sentUrl.rfind('/', query);
            const auto basename = sentUrl.substr(baseStart == std::string::npos ? 0 : baseStart + 1);
            const auto bare = basename.substr(0, basename.find('?'));
            cachedTransport.title = title.empty() || title == sentUrl || title == sentUri
                || title == basename || title == bare ? sentTitle : title;
        }
        positionKnown = true;
        positionMs = (h * 3600 + m * 60 + s) * 1000; positionText = time;
        positionAt = Clock::now() + std::chrono::seconds(1);
    }
    ms = positionMs; if (text) *text = positionText; return true;
}
bool OwnSpeakerControl::readTransportInfo(TransportInfo& info) {
    uint64_t revision;
    { std::lock_guard<std::mutex> lock(cacheMutex); revision = eventRevision; }
    auto result = call("GetTransportInfo", {{"InstanceID", "0"}});
    std::lock_guard<std::mutex> lock(cacheMutex);
    if (revision == eventRevision) {
        cachedTransport.available = result.ok && result.response.child("CurrentTransportState") && result.response.child("CurrentTransportStatus");
        if (cachedTransport.available)
            updateTransport(result.response.value("CurrentTransportState"), result.response.value("CurrentTransportStatus"));
    }
    info = cachedTransport;
    return info.available;
}
void OwnSpeakerControl::poll() {
    TransportInfo info;
    readTransportInfo(info);
    uint32_t ms;
    positionInfo(ms);
    if (Clock::now() >= volumeAt) {
        volumeAt = Clock::now() + std::chrono::seconds(1);
        std::string uri;
        const auto activity = streamActivity ? streamActivity() : StreamActivity{};
        // A held paused GET can stall SOAP replies. Retain the last observed URI
        // until the probe closes; transport-state polling still runs above.
        if (!((info.state == "STOPPED" || info.state == "PAUSED_PLAYBACK") && activity.requestOpen))
            currentUri(uri);
        auto result = call("GetVolume", {{"InstanceID", "0"}, {"Channel", "Master"}}, "", "RenderingControl");
        const auto value = result.response.value("CurrentVolume");
        unsigned parsed; char tail;
        if (result.ok && sscanf(value.c_str(), "%u%c", &parsed, &tail) == 1 && parsed <= 100) {
            std::lock_guard<std::mutex> lock(cacheMutex); volume = parsed;
        }
    }
    if (Clock::now() >= topologyAt) {
        topologyAt = Clock::now() + std::chrono::seconds(5);
        topology(speaker().ip, false);
    }
}
}
