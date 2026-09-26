// sonos-lms.cpp -- bridges a Sonos zone player into an LMS/squeezelite session
//
// Copyright (c) 2026 Jaap van Vliet
//
// Original implementation for the sonos-lms project. Licensed under
// the GNU General Public License, version 3 or (at your option) any later
// version, matching the rest of this project. See LICENSE.
//
// THIS SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.

#include "upnp/noson_speaker_control.h"
#include "upnp/own_speaker_control.h"
#include "upnp/backend.h"
#include "upnp/list_rooms.h"
#include <iostream>
#include "upnp/noson_stream_server.h"

#include "resume_state.h"
#include "transport_intent.h"
#include "pause_mode.h"
#include "sbstreamer.h"
#include "sonos-position.h"
#include "sonos-status.h"
#include "stop_debounce.h"
#include "stream_session.h"

extern "C" {
unsigned get_squeezebox_stream_id(void);
}

#include <algorithm>
#include <atomic>
#include <arpa/inet.h>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <mutex>
#include <memory>
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

std::unique_ptr<upnp::StreamServer> gStreamServer;
std::shared_ptr<upnp::SpeakerControl> gPlayer;
uint8_t gMac[6];
std::atomic<bool> gEvent{true};
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
static std::mutex intentMutex;
static TransportIntent transportIntent;
static RetryBudget streamStartRetry; // protected by transportMutex
static unsigned retryStream = 0;
static uint64_t retryRevision = 0;
extern "C" void end_squeezebox_response(void);
extern "C" void flush_squeezebox_response(void);
extern "C" void hold_squeezebox_resume(unsigned stream);
extern "C" int squeezebox_response_ended(unsigned stream);
extern "C" int squeezebox_response_open(unsigned stream);
extern "C" void acknowledge_squeezebox_resume(unsigned stream);
extern "C" void invalidate_squeezebox_held_get(unsigned stream);
static std::mutex stopMutex;
static StopDebounce deferredStop;
static bool PlaySqueezeBoxLocked(unsigned stream_id, bool resetPosition);

extern "C" unsigned get_squeezebox_stream_id(void) { return streamId.load(); }
extern "C" void new_squeezebox_stream_id(void)
{
    unsigned id = streamId.fetch_add(1) + 1;
    reset_sonos_position(id);
    {
        std::lock_guard<std::mutex> lock(resumeMutex);
        resumeState.streamStarted();
    }
    printf("Creating new stream (%u) for Sonos\n", id);
}

static void dispatchTransportIntent()
{
    std::unique_lock<std::mutex> transport(transportMutex, std::try_to_lock);
    if (!transport.owns_lock() || stream_just_restarted() || !ourStreamStarted.load() || !gPlayer)
        return;
    TransportIntent intent;
    {
        std::lock_guard<std::mutex> lock(intentMutex);
        if (!transportIntent.pending || !transportIntent.retry.ready(RetryBudget::Clock::now())) return;
        intent = transportIntent;
        transportIntent.pending = false; // a newer revision can arrive during I/O
    }
    const char command = intent.command;
    const bool pause = command == 'p';
    auto unpause = intent.unpause;
    // A new stream's successful PlayStream already satisfies a deferred play.
    if (!pause && (intent.stream != streamId.load() || intent.restartPending)) {
        if (intent.deferred) printf("strm u: deferred intent applied by new stream\n");
        return;
    }
    if (unpause == ResumeState::Unpause::FeedHeldGet) {
        if (squeezebox_response_open(streamId.load())) {
            printf("strm u: feeding held GET, no same-URL resume\n");
            acknowledge_squeezebox_resume(streamId.load());
            if (intent.deferred) printf("strm u: deferred transport applied\n");
            return;
        }
        // A preceding command may have ended the GET while this intent waited.
        unpause = ResumeState::Unpause::SameURL;
    }
    {
        bool ended = !pause && squeezebox_response_ended(streamId.load());
        bool missing = unpause == ResumeState::Unpause::SameURL;
        bool error = !pause && gPlayer->transportInfo().status == "ERROR_LOST_CONNECTION";
        if (ended)
            printf("strm u after ended response -> PlayStream(same URL)\n");
        else if (missing)
            printf("strm u without open response -> PlayStream(same URL)\n");
        else if (pause && pauseMode() == PauseMode::Stop)
            printf("strm p -> UPnP Stop (pause=stop)\n");
        else
            printf("strm %c -> UPnP %s\n", command, pause ? "Pause" : "Play");
        bool ok;
        const bool playStreamAttempt = ended || missing || error;
        if (playStreamAttempt) {
            if (error) printf("device in error state -> re-issuing PlayStream\n");
            if (missing) invalidate_squeezebox_held_get(streamId.load());
            intent.retry.begin();
            ok = PlaySqueezeBoxLocked(streamId.load(), false);
        } else {
            // Sonos withholds its Pause acknowledgement until the stream closes.
            // End the response first, regardless of whether Pause succeeds.
            if (pause) end_squeezebox_response();
            auto upnpStart = std::chrono::steady_clock::now();
            const bool stopForPause = pause && pauseMode() == PauseMode::Stop;
            if (stopForPause) {
                std::lock_guard<std::mutex> stateLock(resumeMutex);
                resumeState.stopForPause(streamId.load());
            }
            ok = pause ? (stopForPause ? gPlayer->stop() : gPlayer->pause()) : gPlayer->play();
            auto upnpMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - upnpStart).count();
            printf("gPlayer->%s took %lldms\n", pause ? (stopForPause ? "Stop" : "Pause") : "Play", (long long)upnpMs);
        }
        if (ok && intent.deferred) printf("strm %c: deferred transport applied\n", command);
        if (!ok) {
            printf("strm %c: device transport command failed\n", command);
            if (playStreamAttempt) {
                std::lock_guard<std::mutex> lock(intentMutex);
                if (transportIntent.revision == intent.revision) {
                    intent.retry.failed(RetryBudget::Clock::now());
                    transportIntent.retry = intent.retry;
                    transportIntent.unpause = ResumeState::Unpause::SameURL;
                    transportIntent.pending = !intent.retry.exhausted();
                    transportIntent.deferred = true;
                    printf("PlayStream(same URL): attempt %u/3 failed; %s\n",
                        intent.retry.count(), transportIntent.pending ? "retry in 1 s" : "giving up until next stream/command");
                }
            }
        }
    }
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
    bool responseOpen;
    {
        std::lock_guard<std::mutex> lock(resumeMutex);
        responseOpen = command == 'u' && squeezebox_response_open(streamId.load());
        unpause = resumeState.command(command, responseOpen);
        if (pauseMode() == PauseMode::Stop && responseOpen
            && unpause == ResumeState::Unpause::SameURL)
            unpause = ResumeState::Unpause::FeedHeldGet;
    }
    uint64_t revision = 0;
    if (command == 'p' || command == 'u' || command == 's' || command == 'q') {
        std::lock_guard<std::mutex> lock(intentMutex);
        revision = ++transportIntent.revision;
        transportIntent.command = command;
        transportIntent.stream = streamId.load();
        transportIntent.unpause = unpause;
        transportIntent.deferred = false;
        transportIntent.retry = RetryBudget{};
        transportIntent.restartPending = stream_just_restarted();
        lmsPaused.store(command == 'p' || command == 'q');
        transportIntent.pending = (command == 'p' || command == 'u')
            && unpause != ResumeState::Unpause::NewStream && streamId.load() != 0;
    }
    // Heartbeats still pass through ResumeState, but need no decision log.
    if (command != 't') {
        const char* decision = "None";
        switch (unpause) {
        case ResumeState::Unpause::NewStream: decision = "NewStream"; break;
        case ResumeState::Unpause::FeedHeldGet: decision = "FeedHeldGet"; break;
        case ResumeState::Unpause::SameURL: decision = "SameURL"; break;
        case ResumeState::Unpause::None: break;
        }
        printf("strm %c: decision=%s stream=%u\n", command, decision, streamId.load());
    }
    if (command == 's') {
        reset_sonos_position(streamId.load());
        ++lmsStreamSerial; // release a producer waiting on an obsolete HTTP request
        return;
    }
    if (command != 'p' && command != 'q' && command != 'u') return;
    if (!ourStreamStarted.load() && (streamId.load() == 0 || command == 'q')) {
        printf("strm %c: transport ignored before first bridge stream\n", command);
        return;
    }
    if (unpause == ResumeState::Unpause::NewStream) {
        printf("strm u: new stream pending from strm s, no same-URL resume\n");
        return; // decoded track boundary allocates the ID and calls PlaySqueezeBox
    }
    if (command == 'q') {
        flush_squeezebox_response();
        std::lock_guard<std::mutex> lock(stopMutex);
        deferredStop.schedule(StopDebounce::Clock::now());
        printf("strm q: deferring %s for 400 ms\n", pauseMode() == PauseMode::Stop ? "Stop" : "Pause");
        return; // receive the next strm s without waiting on UPnP
    }
    dispatchTransportIntent();
    {
        std::lock_guard<std::mutex> lock(intentMutex);
        if (transportIntent.revision == revision && transportIntent.pending) {
            transportIntent.deferred = true;
            printf("strm %c: transport deferred until restart/transport ready\n", command);
        }
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
    const bool stopForPause = pauseMode() == PauseMode::Stop;
    if (stopForPause) {
        std::lock_guard<std::mutex> lock(resumeMutex);
        resumeState.stopForPause(streamId.load());
    }
    printf(stopForPause ? "strm q -> UPnP Stop (400 ms elapsed, pause=stop)\n"
                        : "strm q -> UPnP Pause (400 ms elapsed)\n");
    if (!(stopForPause ? gPlayer->stop() : gPlayer->pause()))
        printf("strm q: device transport command failed\n");
    flush_squeezebox_response();
}

class StopTimer {
public:
    StopTimer() : worker([this] {
        while (running.load()) {
            dispatchTransportIntent();
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
    std::string playlistTimestamp, playlistIndex, time, duration, mode;
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
        else if (key == "playlist_timestamp") playlistTimestamp = val;
        else if (key == "playlist_cur_index") playlistIndex = val;
        else if (key == "time") time = val;
        else if (key == "duration") duration = val;
        else if (key == "mode") mode = val;
    }

    printf("LMS identity: id=%s playlist_timestamp=%s index=%s time=%s duration=%s mode=%s\n",
        info.id.c_str(), playlistTimestamp.c_str(), playlistIndex.c_str(),
        time.c_str(), duration.c_str(), mode.c_str());

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
    const auto res = gStreamServer->resource(SBSTREAMER_CNAME);
    if (res.uri.empty()) return "";
    return gPlayer->controllerUri() + res.uri
        + (res.uri.find('?') == std::string::npos ? "?" : "&")
        + "session=" + streamSessionToken() + "&stream=" + std::to_string(stream_id);
}

static void ObserveDeviceTransport(const std::string& state)
{
    bool relay;
    {
        std::lock_guard<std::mutex> lock(resumeMutex);
        if (resumeState.expireResume())
            printf("Device-initiated resume lease expired: 5 s without strm u\n");
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
// a resume: require an observed pause (PAUSED or Stop-for-pause STOPPED)
// followed by PLAYING/TRANSITIONING, and an LMS pause or completed q-Stop without a subsequent u/s/q.
// Poll cached device state because GET and event callbacks
// can arrive in either order. Do not hold transportMutex across LMS CLI I/O:
// the resulting strm u feeds the held GET without another device command.
void ResumeSqueezeBox(unsigned requested)
{
    if (!ourStreamStarted.load() || stream_just_restarted()) return;
    ObserveDeviceTransport(gPlayer->transportInfo().state);
    bool resume;
    {
        std::lock_guard<std::mutex> lock(resumeMutex);
        resume = resumeState.takeResume(requested, streamId.load());
        // Publish the HTTP hold before CLI I/O: LMS can reply with q/s before
        // sendLmsCommand returns. Status and GET callbacks share this decision.
        if (requested == streamId.load() && resumeState.stopResumeRequested())
            hold_squeezebox_resume(requested);
    }
    if (resume) {
        printf("Device-initiated resume: current stream %u\n", requested);
        if (!sendLmsCommand(gServer, gMac, "play")) {
            printf("Device-initiated resume: LMS play failed; retaining 5 s lease\n");
        }
    }
}

static bool PlaySqueezeBoxLocked(unsigned stream_id, bool resetPosition)
{
    if (stream_id != streamId.load()) return false;
    std::string streamURL = SqueezeBoxURL(stream_id);
    bool ok = false;
    if (!streamURL.empty()) {
        const auto res = gStreamServer->resource(SBSTREAMER_CNAME);
        TrackInfo track = fetchLmsTrackInfo(gServer, gMac);
        std::string title = track.title.empty() ? "Squeezebox" : track.title;
        std::string artUrl = track.artworkUrl.empty()
            ? gPlayer->controllerUri() + res.iconUri : track.artworkUrl;
        printf("PlaySqueezeBox: title='%s' art='%s'\n", title.c_str(), artUrl.c_str());
        if (resetPosition) reset_sonos_position(stream_id);
        ok = gPlayer->playStream(streamURL, title, artUrl);
        if (ok) {
            ourStreamStarted.store(true);
            acknowledge_squeezebox_resume(stream_id);
        }
    }
    if (ok && stream_id == streamId.load()) completedStream.store(stream_id);
    if (!ok) printf("PlaySqueezeBox: stream %u failed\n", stream_id);
    return ok;
}

static void dispatchStreamStart()
{
    std::unique_lock<std::mutex> transport(transportMutex, std::try_to_lock);
    if (!transport.owns_lock()) return;
    const unsigned current = streamId.load();
    if (!current || completedStream.load() == current) return;
    uint64_t revision;
    {
        std::lock_guard<std::mutex> lock(intentMutex);
        revision = transportIntent.revision;
    }
    const bool newStream = current != retryStream;
    if (newStream || revision != retryRevision) {
        streamStartRetry = RetryBudget{};
        retryStream = current;
        retryRevision = revision;
    }
    if (!streamStartRetry.ready(RetryBudget::Clock::now())) return;
    streamStartRetry.begin();
    if (PlaySqueezeBoxLocked(current, newStream)) {
        streamStartRetry.succeeded();
    } else {
        streamStartRetry.failed(RetryBudget::Clock::now());
        printf("PlayStream(stream %u): attempt %u/3 failed; %s\n", current,
            streamStartRetry.count(), streamStartRetry.exhausted()
                ? "giving up until next stream/command" : "retry in 1 s");
    }
}

namespace {

// One-shot mode: play a local file straight from the filesystem instead of
// bridging an LMS/squeezelite session. Used for ad hoc testing, not the
// normal service path.
void playLocalFileOnce(const std::string& filePath)
{
    std::string extension = "none";
    auto dot = filePath.rfind('.');
    if (dot != std::string::npos)
        extension = filePath.substr(dot + 1);

    std::string url = gPlayer->controllerUri() + "/music/track." + extension
        + "?path=" + percentEncode(filePath);

    if (gPlayer->playStream(url, ""))
        printf("Started playing URL %s\n", url.c_str());
    else
        printf("Failed to start URL %s\n", url.c_str());
}

// Polls the actual Sonos playback position once per loop iteration. noson
// caches GetPositionInfo() for 1s internally, so this costs about one
// network round trip per second, not one per iteration.
void pollSonosPosition()
{
    auto token = sonos_position_poll_token();
    uint32_t ms;
    if (!gPlayer->positionInfo(ms)) return;
    set_sonos_position_ms(token, ms);
}

// Runs the periodic (every ~30s, or immediately on a Sonos event) status
// refresh: transport-state mirroring, error-state resume, and the printed
// status table when anything actually changed.
void refreshStatus(bridge::Status& status)
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

void runBridgeLoop(bridge::Status& status)
{
    unsigned idleTicks = 0;
    constexpr unsigned kStatusRefreshTicks = 3000;  // ~30s at the 10ms sleep below

    status.update();
    auto nextPoll = std::chrono::steady_clock::now() + std::chrono::milliseconds(gPlayer->pollIntervalMs());

    for (;;) {
        dispatchStreamStart();

        pollSonosPosition();

        const bool eventPending = gEvent.exchange(false);
        const auto pollInterval = gPlayer->pollIntervalMs();
        const bool pollDue = pollInterval && std::chrono::steady_clock::now() >= nextPoll;
        if (idleTicks >= kStatusRefreshTicks || eventPending || pollDue) {
            nextPoll = std::chrono::steady_clock::now() + std::chrono::milliseconds(pollInterval);
            refreshStatus(status);
            idleTicks = 0;
        } else {
            ++idleTicks;
        }

        usleep(10000);  // 10ms
    }
}

}  // namespace

// Slim discovery replies echo 'E', followed by four-byte tag / byte length / value.
static bool parseDiscoveryResponse(const char* data, size_t len, std::string& outName)
{
    outName.clear();
    if (!data || len < 1 || data[0] != 'E') return false;
    std::string name;
    size_t pos = 1;
    while (pos < len) {
        if (len - pos < 5) return false;
        size_t length = static_cast<unsigned char>(data[pos + 4]);
        if (length > len - pos - 5) return false;
        if (memcmp(data + pos, "NAME", 4) == 0)
            name.assign(data + pos + 5, length);
        pos += 5 + length;
    }
    outName = name;
    return true;
}

static std::string discoverLmsServer(unsigned timeoutMs = 3000, bool quiet = false)
{
    if (!timeoutMs) return ""; // a zero SO_RCVTIMEO would wait indefinitely
    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) return "";
    int broadcast = 1;
    timeval tv{static_cast<time_t>(timeoutMs / 1000),
        static_cast<suseconds_t>((timeoutMs % 1000) * 1000)};
    if (setsockopt(fd, SOL_SOCKET, SO_BROADCAST, &broadcast, sizeof(broadcast)) < 0
        || setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv)) < 0) {
        close(fd);
        return "";
    }
    sockaddr_in destination{};
    destination.sin_family = AF_INET;
    destination.sin_port = htons(3483);
    destination.sin_addr.s_addr = INADDR_BROADCAST;
    const char request[] = "eNAME\0JSON\0UUID\0VERS\0";
    if (sendto(fd, request, sizeof(request) - 1, 0,
            reinterpret_cast<sockaddr*>(&destination), sizeof(destination))
            != static_cast<ssize_t>(sizeof(request) - 1)) {
        close(fd);
        return "";
    }
    char data[65536];
    sockaddr_in sender{};
    socklen_t senderSize = sizeof(sender);
    ssize_t size = recvfrom(fd, data, sizeof(data), 0,
        reinterpret_cast<sockaddr*>(&sender), &senderSize);
    close(fd);
    std::string name;
    if (size < 0 || sender.sin_family != AF_INET
        || !parseDiscoveryResponse(data, static_cast<size_t>(size), name)) return "";
    std::string host = inet_ntoa(sender.sin_addr);
    if (!quiet) printf("LMS server from UDP discovery: %s%s%s\n", host.c_str(),
        name.empty() ? "" : " name=", name.c_str());
    return host;
}

static std::string readLmsServerFromConfig(const char* path = "/etc/sonos-lms/config")
{
    std::ifstream config(path);
    std::string line;
    const char* whitespace = " \t\r\n\f\v";
    while (std::getline(config, line)) {
        size_t start = line.find_first_not_of(whitespace);
        if (start == std::string::npos || line[start] == '#') continue;
        if (line.compare(start, 11, "LMS_SERVER=") != 0) continue;
        start = line.find_first_not_of(whitespace, start + 11);
        if (start == std::string::npos) return "";
        return line.substr(start, line.find_last_not_of(whitespace) - start + 1);
    }
    return "";
}

static int findServerCommand()
{
    const auto host = discoverLmsServer(3000, true);
    if (host.empty()) return 2;
    printf("%s\n", host.c_str());
    return 0;
}

// Keep stdout machine-readable even when the selected backend logs discovery.
// Complete backend destruction while diagnostics are still redirected.
static int listRoomsCommand(const std::string& ip, int debug)
{
    fflush(stdout);
    const int outputFd = dup(STDOUT_FILENO);
    if (outputFd < 0 || dup2(STDERR_FILENO, STDOUT_FILENO) < 0) {
        if (outputFd >= 0) close(outputFd);
        fprintf(stderr, "Cannot prepare room discovery output\n");
        return 2;
    }
    std::ostringstream rooms;
    int result = 2;
    try {
        if (upnp::backend() == upnp::Backend::Own) {
            upnp::OwnSpeakerControl control([] { return 0u; });
            result = upnp::listRooms(control, ip, rooms, std::cerr);
        } else {
            upnp::NosonStreamServer server(debug, nullptr);
            upnp::NosonSpeakerControl control(server, nullptr);
            result = upnp::listRooms(control, ip, rooms, std::cerr);
        }
    } catch (const std::exception& error) {
        fprintf(stderr, "Room discovery failed: %s\n", error.what());
    }
    fflush(stdout);
    if (dup2(outputFd, STDOUT_FILENO) < 0) {
        close(outputFd);
        fprintf(stderr, "Cannot restore room discovery output\n");
        return 2;
    }
    close(outputFd);
    if (!result) std::cout << rooms.str();
    return result;
}

int main(int argc, char** argv)
{
    setvbuf(stdout, nullptr, _IOLBF, 0);
    if (findFlag(argc, argv, "--find-server")) return findServerCommand();
    if (findFlag(argc, argv, "--list-rooms")) {
        const auto ip = findOption(argc, argv, "--ip");
        return listRoomsCommand(ip ? ip : "", findFlag(argc, argv, "--debug") ? 4 : 0);
    }
    (void)pauseMode();
    const auto backend = upnp::backend();
    try {
        printf("Stream session: %s\n", streamSessionToken().c_str());
    } catch (const std::exception& error) {
        fprintf(stderr, "%s\n", error.what());
        return EXIT_FAILURE;
    }

    int debugLevel = findFlag(argc, argv, "--debug") ? 4 : 0;
    const char* ip = findOption(argc, argv, "--ip");
    const char* room = findOption(argc, argv, "--room");
    const char* filename = findOption(argc, argv, "--file");
    const char* server = findOption(argc, argv, "--server");

    printf("\n\n| sonos-lms -- bridges a Sonos zone player into an LMS/squeezelite session\n\n\n");

    auto serverBackend = new upnp::NosonStreamServer(debugLevel, onSonosEvent);
    gStreamServer.reset(serverBackend);
    if (backend == upnp::Backend::Own)
        gPlayer = std::make_shared<upnp::OwnSpeakerControl>([] { return gStreamServer->port(); });
    else
        gPlayer = std::make_shared<upnp::NosonSpeakerControl>(*serverBackend, onSonosEvent);
    if (!room) {
        printf("Please specify a room to join with the --room option\n");
        return EXIT_FAILURE;
    }
    if (!gPlayer->discover(room, ip ? ip : "")) return EXIT_FAILURE;
    static bridge::SBStreamer streamer;
    streamer.registerWith(*gStreamServer);

    bridge::Status status(gPlayer);
    status.get_mac(gMac);
    printf(" (MAC = %02X:%02X:%02X:%02X:%02X:%02X)\n\n", gMac[0], gMac[1], gMac[2], gMac[3], gMac[4], gMac[5]);
    if (server) {
        gServer = server;
        printf("LMS server from --server: %s\n", gServer.c_str());
    } else {
        gServer = readLmsServerFromConfig();
        if (!gServer.empty())
            printf("LMS server from /etc/sonos-lms/config: %s\n", gServer.c_str());
        else
            gServer = discoverLmsServer();
    }
    if (gServer.empty()) {
        printf("No LMS server resolved from --server, config, or UDP discovery. "
            "Track metadata and Sonos-app pause/play relay are unavailable without a resolved server. "
            "Squeezelite's independent discovery will keep retrying for core playback; "
            "restart with a reachable server to enable metadata and relay.\n");
    }

    static StopTimer stopTimer;
    std::thread* squeezeliteThread = nullptr;
    if (filename) {
        playLocalFileOnce(filename);
    } else {
        squeezeliteThread = new std::thread(runSqueezeliteClient, gServer.empty() ? nullptr : gServer.c_str(), std::string(room) + " (Sonos)");
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
    gEvent.store(true);
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
