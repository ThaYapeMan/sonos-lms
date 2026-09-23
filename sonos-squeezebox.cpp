// sonos-squeezebox.cpp -- bridges a Sonos zone player into an LMS/squeezelite session
//
// Copyright (c) 2026 Jaap van Vliet
//
// Original implementation for the sonos-squeezebox project. Licensed under
// the GNU General Public License, version 3 or (at your option) any later
// version, matching the rest of this project. See LICENSE.
//
// THIS SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.

#include <contentdirectory.h>
#include <didlparser.h>
#include <filestreamer.h>
#include <imageservice.h>
#include <sonosplayer.h>
#include <sonossystem.h>

#include "resume_state.h"
#include "sbstreamer.h"
#include "sonos-position.h"
#include "sonos-status.h"
#include "stop_debounce.h"

extern "C" {
unsigned get_squeezebox_stream_id(void);
}

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <netdb.h>
#include <sstream>
#include <string>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>

// Defined in squeezelite.cpp; runs the squeezelite client loop on the
// calling thread until the LMS connection is torn down.
void squeezelite(const char* server, uint8_t* mac, const char* name);

namespace {
void onSonosEvent(void* handle);
const char* findFlag(int argc, char** argv, const std::string& flag);
const char* findOption(int argc, char** argv, const std::string& option);
}  // namespace

SONOS::System* gSonos = 0;
SONOS::PlayerPtr gPlayer;
uint8_t gMac[6];
volatile bool gEvent = true;
static std::string gServer;

// The restart window lasts from allocation until PlayStream completes. A separate
// mutex serializes network transport calls; output never waits for that mutex.
static std::atomic<unsigned> streamId(0);
static std::atomic<unsigned> lmsStreamSerial(0);
extern "C" unsigned get_lms_stream_serial(void) { return lmsStreamSerial.load(); }
static std::atomic<unsigned> completedStream(0);
static bool stream_just_restarted() { return streamId.load() != completedStream.load(); }
static std::atomic<bool> lmsPaused(false);
static std::atomic<bool> ourStreamStarted(false);
static std::mutex resumeMutex;
static ResumeState resumeState;
extern "C" int sonos_lms_is_paused(void) { return lmsPaused.load(); }
static std::mutex transportMutex;
extern "C" void end_squeezebox_response(void);
extern "C" int squeezebox_response_ended(unsigned stream);
extern "C" int squeezebox_response_open(unsigned stream);
extern "C" void acknowledge_squeezebox_resume(unsigned stream);
static std::mutex stopMutex;
static StopDebounce deferredStop;
static bool PlaySqueezeBoxLocked(unsigned stream_id, bool resetPosition);

extern "C" unsigned get_squeezebox_stream_id(void) { return streamId.load(); }
extern "C" void new_squeezebox_stream_id(void)
{
    unsigned id = streamId.fetch_add(1) + 1;
    printf("Creating new stream (%u) for Sonos\n", id);
}

extern "C" void sonos_lms_transport(char command)
{
    if (command == 's' || command == 'p' || command == 'u') {
        std::lock_guard<std::mutex> lock(stopMutex);
        bool pending = deferredStop.cancel();
        if (pending && command == 's')
            printf("strm q: superseded by strm s, no Pause\n");
    }
    ResumeState::Unpause unpause;
    {
        std::lock_guard<std::mutex> lock(resumeMutex);
        unpause = resumeState.command(command,
            command == 'u' && squeezebox_response_open(streamId.load()));
    }
    if (command == 's') {
        ++lmsStreamSerial; // release a producer waiting on an obsolete HTTP request
        lmsPaused.store(false);
        return;
    }
    if (command != 'p' && command != 'q' && command != 'u') return;
    bool pause = command != 'u';
    lmsPaused.store(pause);
    if (!ourStreamStarted.load()) {
        printf("strm %c: transport ignored before first bridge stream\n", command);
        return;
    }
    if (unpause == ResumeState::Unpause::NewStream) {
        printf("strm u: new stream pending from strm s, no same-URL resume\n");
        return; // decoded track boundary allocates the ID and calls PlaySqueezeBox
    }
    if (unpause == ResumeState::Unpause::FeedHeldGet) {
        printf("strm u: feeding held GET, no same-URL resume\n");
        acknowledge_squeezebox_resume(streamId.load());
        return; // process_strm releases PCM into the existing encoder
    }
    if (command == 'q') {
        end_squeezebox_response();
        std::lock_guard<std::mutex> lock(stopMutex);
        deferredStop.schedule(StopDebounce::Clock::now());
        printf("strm q: deferring Pause for 400 ms\n");
        return; // receive the next strm s without waiting on UPnP
    }
    std::unique_lock<std::mutex> lock(transportMutex, std::try_to_lock);
    if (!lock.owns_lock() || stream_just_restarted()) {
        printf("strm %c: transport suppressed during stream restart\n", command);
        return;
    }
    if (gPlayer) {
        bool ended = !pause && squeezebox_response_ended(streamId.load());
        bool missing = unpause == ResumeState::Unpause::SameURL;
        bool error = !pause && gPlayer->GetTransportProperty().TransportStatus == "ERROR_LOST_CONNECTION";
        if (ended)
            printf("strm u after ended response -> PlayStream(same URL)\n");
        else if (missing)
            printf("strm u without open response -> PlayStream(same URL)\n");
        else
            printf("strm %c -> UPnP %s\n", command, pause ? "Pause" : "Play");
        bool ok;
        if (ended || missing || error) {
            if (error) printf("device in error state -> re-issuing PlayStream\n");
            ok = PlaySqueezeBoxLocked(streamId.load(), false);
        } else {
            ok = pause ? gPlayer->Pause() : gPlayer->Play();
        }
        // End the response AFTER Pause, even if the device rejected the command.
        if (pause) end_squeezebox_response();
        if (!ok) printf("strm %c: device transport command failed\n", command);
    }
}

// Independent of the main loop's network/status queries and of slimproto input.
// Serialize the eventual command with PlayStream, and recheck cancellation only
// after acquiring that lock so a q/s burst cannot pause a freshly installed URI.
static void dispatchDeferredStop()
{
    {
        std::lock_guard<std::mutex> lock(stopMutex);
        if (!deferredStop.isDue(StopDebounce::Clock::now())) return;
    }
    std::unique_lock<std::mutex> transport(transportMutex, std::try_to_lock);
    if (!transport.owns_lock()) return;
    {
        std::lock_guard<std::mutex> lock(stopMutex);
        if (!deferredStop.takeDue(StopDebounce::Clock::now())) return;
    }
    if (!ourStreamStarted.load() || stream_just_restarted() || !lmsPaused.load()) return;
    printf("strm q -> UPnP Pause (400 ms elapsed)\n");
    if (!gPlayer->Pause()) printf("strm q: device transport command failed\n");
    end_squeezebox_response();
}

class StopTimer {
public:
    StopTimer() : worker([this] {
        while (running.load()) {
            dispatchDeferredStop();
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
    }) {}
    ~StopTimer() { running.store(false); worker.join(); }
private:
    std::atomic<bool> running{true};
    std::thread worker;
};

namespace {
// Percent-encodes everything outside the RFC 3986 "unreserved" set.
std::string percentEncode(const std::string& raw)
{
    static const char* hexDigits = "0123456789abcdef";
    std::string out;
    out.reserve(raw.size());
    for (unsigned char c : raw) {
        if (isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') {
            out += (char)c;
        } else {
            out += '%';
            out += hexDigits[c >> 4];
            out += hexDigits[c & 0x0f];
        }
    }
    return out;
}
}  // namespace

struct TrackInfo {
    std::string id;
    std::string title;
    std::string artist;
    std::string album;
    std::string artworkUrl;
};

static std::string lmsUrldecode(const std::string& s)
{
    std::string result;
    for (size_t i = 0; i < s.size(); ) {
        if (s[i] == '%' && i + 2 < s.size()) {
            char hex[3] = { s[i + 1], s[i + 2], '\0' };
            result += (char)strtol(hex, nullptr, 16);
            i += 3;
        } else if (s[i] == '+') {
            result += ' ';
            ++i;
        } else {
            result += s[i++];
        }
    }
    return result;
}

// Query LMS CLI (port 9090) for current track metadata of player identified by mac.
// Returns empty fields on any error so the caller can gracefully fall back.
static TrackInfo fetchLmsTrackInfo(const std::string& server, const uint8_t* mac)
{
    TrackInfo info;
    if (server.empty()) return info;

    // Strip optional :port suffix to get the bare hostname / IP
    std::string host = server;
    size_t col = host.rfind(':');
    if (col != std::string::npos) {
        std::string tail = host.substr(col + 1);
        bool allDigits = !tail.empty();
        for (char c : tail) allDigits = allDigits && isdigit((unsigned char)c);
        if (allDigits) host = host.substr(0, col);
    }

    struct addrinfo hints = {}, *ai = nullptr;
    hints.ai_family   = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    if (getaddrinfo(host.c_str(), "9090", &hints, &ai) != 0) return info;

    int fd = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
    if (fd < 0) { freeaddrinfo(ai); return info; }

    struct timeval tv { 2, 0 };
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

    bool ok = (connect(fd, ai->ai_addr, ai->ai_addrlen) == 0);
    freeaddrinfo(ai);
    if (!ok) { close(fd); return info; }

    // LMS CLI expects the raw MAC with literal colons (xx:xx:xx:xx:xx:xx)
    char macFmt[18];
    snprintf(macFmt, sizeof(macFmt), "%02x:%02x:%02x:%02x:%02x:%02x",
        mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);

    // tags: a=artist, l=album; title and id are always returned
    std::string query = std::string(macFmt) + " status - 1 tags:aAl\n";
    send(fd, query.c_str(), query.size(), 0);

    std::string response;
    char ch;
    while (response.size() < 8192 && recv(fd, &ch, 1, 0) == 1 && ch != '\n')
        response += ch;
    close(fd);

    printf("LMS response: %s\n", response.c_str());

    // Each space-separated token is URL-encoded "key:value"
    std::istringstream ss(response);
    std::string token;
    while (ss >> token) {
        std::string decoded = lmsUrldecode(token);
        size_t sep = decoded.find(':');
        if (sep == std::string::npos) continue;
        std::string key = decoded.substr(0, sep);
        std::string val = decoded.substr(sep + 1);
        if      (key == "title")  info.title = val;
        else if (key == "artist") info.artist = val;
        else if (key == "album")  info.album = val;
        else if (key == "id")     info.id = val;
    }

    // Build cover art URL from track id (works for local library and most streams)
    if (!info.id.empty())
        info.artworkUrl = "http://" + host + ":9000/music/" + info.id + "/cover.jpg";

    return info;
}

// Relay device transport intent using the raw player MAC.
static bool sendLmsCommand(const std::string& server, const uint8_t* mac, const char* command)
{
    if (server.empty()) {
        printf("LMS CLI: no server configured\n");
        return false;
    }

    std::string host = server;
    size_t col = host.rfind(':');
    if (col != std::string::npos) {
        std::string tail = host.substr(col + 1);
        bool allDigits = !tail.empty();
        for (char c : tail) allDigits = allDigits && isdigit((unsigned char)c);
        if (allDigits) host = host.substr(0, col);
    }

    struct addrinfo hints = {}, *ai = nullptr;
    hints.ai_family   = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    if (getaddrinfo(host.c_str(), "9090", &hints, &ai) != 0) {
        printf("LMS CLI: cannot resolve server\n");
        return false;
    }

    int fd = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
    if (fd < 0) { freeaddrinfo(ai); return false; }

    struct timeval tv { 2, 0 };
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

    bool ok = (connect(fd, ai->ai_addr, ai->ai_addrlen) == 0);
    freeaddrinfo(ai);
    if (!ok) {
        printf("LMS CLI: connection failed\n");
        close(fd);
        return false;
    }

    char macFmt[18];
    snprintf(macFmt, sizeof(macFmt), "%02x:%02x:%02x:%02x:%02x:%02x",
        mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);

    std::string cmd = std::string(macFmt) + " " + command + "\n";
    ok = send(fd, cmd.c_str(), cmd.size(), MSG_NOSIGNAL) == (ssize_t)cmd.size();
    if (ok)
        printf("LMS CLI: %s", cmd.c_str());
    else
        printf("LMS CLI: failed to send %s\n", command);
    close(fd);
    return ok;
}

// Runs squeezelite's own client loop on a dedicated thread; returns when the
// LMS connection is torn down (e.g. process shutdown).
static void runSqueezeliteClient(const char* server, std::string playerName)
{
    squeezelite(server, gMac, playerName.c_str());
    printf("squeezelite client thread stopped\n");
}

// Shared by SetAVTransportURI and HTTP redirects, including resource parameters.
std::string SqueezeBoxURL(unsigned stream_id)
{
    auto rb = gSonos->GetRequestBroker(SBSTREAMER_CNAME);
    auto res = rb ? rb->GetResource(SBSTREAMER_CNAME) : SONOS::RequestBroker::ResourcePtr();
    if (!res) return "";
    return gPlayer->GetControllerUri() + res->uri
        + (res->uri.find('?') == std::string::npos ? "?" : "&")
        + "stream=" + std::to_string(stream_id);
}

static void ObserveDeviceTransport(const std::string& state)
{
    bool relay;
    {
        std::lock_guard<std::mutex> lock(resumeMutex);
        relay = resumeState.observe(state);
    }
    if (relay) {
        printf("Device-initiated pause -> LMS pause\n");
        // The device has already paused: promptly deliver EOF even if LMS CLI
        // is slow or unavailable. Its later strm p repeats this idempotently.
        end_squeezebox_response();
        sendLmsCommand(gServer, gMac, "pause 1");
    }
}

// Called while a current-ID GET waits for PCM. A probe/reconnect is not itself
// a resume: require observed PAUSED -> PLAYING/TRANSITIONING and strm p without
// a subsequent strm u. Poll cached device state because GET and event callbacks
// can arrive in either order. Do not hold transportMutex across LMS CLI I/O:
// the resulting strm u feeds the held GET without another device command.
void ResumeSqueezeBox(unsigned requested)
{
    if (!ourStreamStarted.load() || stream_just_restarted()) return;
    ObserveDeviceTransport(gPlayer->GetTransportProperty().TransportState);
    bool resume;
    {
        std::lock_guard<std::mutex> lock(resumeMutex);
        resume = resumeState.takeResume(requested, streamId.load());
    }
    if (resume) {
        printf("Device-initiated resume: current stream %u\n", requested);
        if (!sendLmsCommand(gServer, gMac, "play")) {
            std::lock_guard<std::mutex> lock(resumeMutex);
            resumeState.retryResume();
        }
    }
}

static bool PlaySqueezeBoxLocked(unsigned stream_id, bool resetPosition)
{
    if (stream_id != streamId.load()) return false;
    std::string streamURL = SqueezeBoxURL(stream_id);
    bool ok = false;
    if (!streamURL.empty()) {
        auto rb = gSonos->GetRequestBroker(SBSTREAMER_CNAME);
        auto res = rb->GetResource(SBSTREAMER_CNAME);
        TrackInfo track = fetchLmsTrackInfo(gServer, gMac);
        std::string title = track.title.empty() ? "Squeezebox" : track.title;
        std::string artUrl = track.artworkUrl.empty()
            ? gPlayer->GetControllerUri() + res->iconUri : track.artworkUrl;
        printf("PlaySqueezeBox: title='%s' art='%s'\n", title.c_str(), artUrl.c_str());
        if (resetPosition) set_sonos_position_ms(0);
        ok = gPlayer->PlayStream(streamURL, title, artUrl);
        if (ok) {
            ourStreamStarted.store(true);
            acknowledge_squeezebox_resume(stream_id);
        }
    }
    if (stream_id == streamId.load()) completedStream.store(stream_id);
    if (!ok) printf("PlaySqueezeBox: stream %u failed\n", stream_id);
    return ok;
}

bool PlaySqueezeBox(unsigned stream_id)
{
    std::lock_guard<std::mutex> lock(transportMutex);
    return PlaySqueezeBoxLocked(stream_id, true);
}

// Parse UPnP RelTime "H:MM:SS" -> milliseconds; returns 0 on parse failure.
static uint32_t parse_reltime_ms(const std::string& rt)
{
    unsigned h = 0, m = 0, s = 0;
    if (sscanf(rt.c_str(), "%u:%u:%u", &h, &m, &s) == 3)
        return (h * 3600u + m * 60u + s) * 1000u;
    return 0;
}

namespace {

void printDiscoveredPlayers(const SONOS::ZonePlayerList& players)
{
    printf("+----------------------------------------------------------------------- devices / players ---+\n");
    printf("| %-35s | %-24s | %-18s | %5s |\n", "player name", "uuid", "host", "port");
    printf("+---------------------------------------------------------------------------------------------+\n");
    for (const auto& entry : players) {
        printf("| %-35s | %-24s | %-18s | %5d |\n",
            entry.first.c_str(), entry.second->GetUUID().c_str(),
            entry.second->GetHost().c_str(), entry.second->GetPort());
    }
    printf("+---------------------------------------------------------------------------------------------+\n\n");
}

void printDiscoveredZones(const SONOS::ZoneList& zones)
{
    printf("+--------------------------------------------------------------------------- zones / rooms ---+\n");
    printf("| %-35s | %-53s |\n", "room name", "coordinating player");
    printf("+---------------------------------------------------------------------------------------------+\n");
    for (const auto& entry : zones) {
        printf("| %-35s | %-53s |\n",
            entry.second->GetZoneName().c_str(), entry.second->GetCoordinator()->c_str());
    }
    printf("+---------------------------------------------------------------------------------------------+\n\n");
}

// Finds the zone matching --room and connects a controller to its coordinator.
// Returns false (with a message already printed) on any failure.
bool connectToRoom(const std::string& roomName, const SONOS::ZoneList& zones)
{
    printf("Connecting to room %s ... ", roomName.c_str());
    for (const auto& entry : zones) {
        if (entry.second->GetZoneName() != roomName)
            continue;
        gPlayer = gSonos->GetPlayer(entry.second, 0, onSonosEvent);
        if (!gPlayer) {
            printf("FAILED to connect\n");
            return false;
        }
        printf("SUCCESS");
        return true;
    }
    printf("FAILED to find room\n");
    return false;
}

bool connectToSonos(const char* explicitIp)
{
    if (!explicitIp) {
        printf("Connecting to Sonos ... ");
        fflush(stdout);
        if (!gSonos->Discover()) {
            printf("No devices found (try specifying known Sonos player ip-address).\n");
            return false;
        }
        printf("SUCCESS\n\n");
        return true;
    }

    std::string deviceUrl = "http://" + std::string(explicitIp) + ":1400";
    printf("Connecting to Sonos (through player %s) ... ", explicitIp);
    fflush(stdout);
    if (!gSonos->Discover(deviceUrl)) {
        printf("Device is unreachable.\n");
        return false;
    }
    printf("SUCCESS\n\n");
    return true;
}

// One-shot mode: play a local file straight from the filesystem instead of
// bridging an LMS/squeezelite session. Used for ad hoc testing, not the
// normal service path.
void playLocalFileOnce(const std::string& filePath)
{
    std::string extension = "none";
    auto dot = filePath.rfind('.');
    if (dot != std::string::npos)
        extension = filePath.substr(dot + 1);

    std::string url = gPlayer->GetControllerUri() + "/music/track." + extension
        + "?path=" + percentEncode(filePath);

    if (gPlayer->PlayStream(url, ""))
        printf("Started playing URL %s\n", url.c_str());
    else
        printf("Failed to start URL %s\n", url.c_str());
}

// Polls the actual Sonos playback position once per loop iteration. noson
// caches GetPositionInfo() for 1s internally, so this costs about one
// network round trip per second, not one per iteration.
void pollSonosPosition()
{
    SONOS::ElementList posVars;
    if (!gPlayer->GetPositionInfo(posVars))
        return;
    uint32_t ms = parse_reltime_ms(posVars.GetValue("RelTime"));
    if (ms > 0)
        set_sonos_position_ms(ms);
    // ms == 0: Sonos stopped/buffering; leave the atomic at whatever
    // PlaySqueezeBox last set it to (normally 0 on a fresh stream).
}

// Runs the periodic (every ~30s, or immediately on a Sonos event) status
// refresh: transport-state mirroring, error-state resume, and the printed
// status table when anything actually changed.
void refreshStatus(SONOS::Status& status)
{
    status.update();
    std::string transportState = status.getTransportState();

    if (ourStreamStarted.load() && !stream_just_restarted()) {
        ObserveDeviceTransport(transportState);
        // Without a live GET, relay the observed device resume here. With
        // a live GET, its HTTP worker owns the same exclusive resume decision.
        if (!squeezebox_response_open(streamId.load()))
            ResumeSqueezeBox(streamId.load());
    }

    if (status.changed())
        status.print();
}

void runBridgeLoop(SONOS::Status& status)
{
    unsigned lastAppliedStreamId = 0;
    unsigned idleTicks = 0;
    constexpr unsigned kStatusRefreshTicks = 3000;  // ~30s at the 10ms sleep below

    status.update();

    for (;;) {
        unsigned currentStreamId = get_squeezebox_stream_id();
        if (currentStreamId != lastAppliedStreamId) {
            lastAppliedStreamId = currentStreamId;
            PlaySqueezeBox(currentStreamId);
        }

        pollSonosPosition();

        if (idleTicks >= kStatusRefreshTicks || gEvent) {
            gEvent = false;
            refreshStatus(status);
            idleTicks = 0;
        } else {
            ++idleTicks;
        }

        usleep(10000);  // 10ms
    }
}

}  // namespace

int main(int argc, char** argv)
{
    setvbuf(stdout, nullptr, _IOLBF, 0);

    int debugLevel = findFlag(argc, argv, "--debug") ? 4 : 0;
    const char* ip = findOption(argc, argv, "--ip");
    const char* room = findOption(argc, argv, "--room");
    const char* filename = findOption(argc, argv, "--file");
    const char* server = findOption(argc, argv, "--server");

    printf("\n\n| sonos-squeezebox -- bridges a Sonos zone player into an LMS/squeezelite session\n\n\n");

    SONOS::System::Debug(debugLevel);
    gSonos = new SONOS::System(0, onSonosEvent);

    if (!connectToSonos(ip))
        return EXIT_FAILURE;

    {
        SONOS::RequestBrokerPtr imageService(new SONOS::ImageService());
        gSonos->RegisterRequestBroker(imageService);
        gSonos->RegisterRequestBroker(SONOS::RequestBrokerPtr(new SONOS::SBStreamer(imageService.get())));
        gSonos->RegisterRequestBroker(SONOS::RequestBrokerPtr(new SONOS::FileStreamer()));
    }

    SONOS::ZonePlayerList players = gSonos->GetZonePlayerList();
    printDiscoveredPlayers(players);

    SONOS::ZoneList zones = gSonos->GetZoneList();
    printDiscoveredZones(zones);

    if (!room) {
        printf("Please specify a room to join with the --room option\n");
        return EXIT_FAILURE;
    }
    if (!connectToRoom(room, zones))
        return EXIT_FAILURE;

    SONOS::Status status(gPlayer);
    status.get_mac(gMac);
    printf(" (MAC = %02X:%02X:%02X:%02X:%02X:%02X)\n\n", gMac[0], gMac[1], gMac[2], gMac[3], gMac[4], gMac[5]);
    gServer = server ? server : "";

    static StopTimer stopTimer;

    std::thread* squeezeliteThread = nullptr;
    if (filename) {
        playLocalFileOnce(filename);
    } else {
        squeezeliteThread = new std::thread(runSqueezeliteClient, server, std::string(room) + " (Sonos)");
    }

    runBridgeLoop(status);

    if (squeezeliteThread)
        squeezeliteThread->join();
    return 0;
}

namespace {

void onSonosEvent(void* handle)
{
    (void)handle;
    gEvent = true;
}

const char* findFlag(int argc, char** argv, const std::string& flag)
{
    char** end = argv + argc;
    char** hit = std::find(argv, end, flag);
    return (hit != end) ? *hit : nullptr;
}

const char* findOption(int argc, char** argv, const std::string& option)
{
    for (char** it = argv; it != argv + argc; ++it) {
        if (strncmp(*it, option.c_str(), option.length()) == 0 && (*it)[option.length()] == '=')
            return &((*it)[option.length() + 1]);
    }
    return nullptr;
}

}  // namespace
