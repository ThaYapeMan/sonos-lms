// sbstreamer.cpp -- HTTP request broker that serves the FLAC stream to Sonos
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

#include "sbstreamer.h"
#include "stream_session.h"
#include "stream_close_log.h"

#include "sbencoder.h"
#include "sonos-position.h"

#include <atomic>
#include <cstring>
#include <algorithm>
#include <cerrno>
#include <chrono>
#include <memory>
#include <mutex>
#include <unistd.h>

#define SBSTREAMER_ICON "/pulseaudio.png"
#define SBSTREAMER_CONTENT "audio/flac"
#define SBSTREAMER_DESC "Audio stream from %s"
#define SBSTREAMER_TIMEOUT 10000
// Four seconds waiting + bounded socket writes/EOF stays below ~five seconds.
#define SBSTREAMER_HTTP_IDLE_TIMEOUT 4000
// Allow the device play -> LMS CLI -> strm u round trip a full five seconds.
#define SBSTREAMER_RESUME_TIMEOUT 5000
#define SBSTREAMER_STANDBY_TIMEOUT 30000
#define SBSTREAMER_CHUNK 16384

using namespace bridge;

// The LMS generation outlives every HTTP connection. Each ACTIVE request owns
// a fresh FLAC encoder in that generation; STANDBY requests have no encoder.
static std::shared_ptr<SBEncoder> g_enc;
static std::mutex g_enc_mutex;
// Ownership is independent of socket state. All request fields and slots are
// protected by g_enc_mutex; only ACTIVE requests have an encoder.
struct StreamRequest {
    unsigned long long id;
    unsigned stream;
    std::shared_ptr<SBEncoder> encoder;
    bool opened = false;
    bool pauseEnded = false;
    std::chrono::steady_clock::time_point expectedCloseAt{};
    std::atomic<bool> heldResume{false};
    std::atomic<bool> resumeAcknowledged{false};
    std::atomic<bool> serving{false};
};
static bridge::StreamCloseLog closeLog;
static std::atomic<bool> deferCloseLog{false};
extern "C" void configure_squeezebox_close_logging(bool own) { deferCloseLog = own; }
static unsigned long long nextRequestId = 0;
static unsigned ownershipStream = 0;
static std::shared_ptr<StreamRequest> activeRequest;
static std::vector<std::shared_ptr<StreamRequest>> standbyRequests;

// Caller holds g_enc_mutex, including when promoting a standby. Publish the
// fresh encoder and its owner together so PCM always goes to the ACTIVE request.
static void activateRequest(const std::shared_ptr<StreamRequest>& request, bool promoted)
{
    request->encoder = std::make_shared<SBEncoder>(request->stream);
    request->opened = request->encoder->open();
    sonos_position_connection(request->stream, request->id);
    activeRequest = request;
    g_enc = request->encoder;
    if (promoted && deferCloseLog) closeLog.observe(request->stream);
    if (promoted) request->expectedCloseAt = std::chrono::steady_clock::now();
    if (promoted)
        printf("stream %u: GET #%llu promoted\n", request->stream, request->id);
    printf("stream %u: GET #%llu ACTIVE\n", request->stream, request->id);
}
static unsigned endedByPause = 0;
extern void ResumeSqueezeBox(unsigned current);
extern std::string SqueezeBoxURL(unsigned current);
extern "C" unsigned get_squeezebox_stream_id(void);
extern "C" unsigned get_lms_stream_serial(void);
extern "C" int sonos_lms_is_paused(void);
extern "C" int sonos_output_running(void);

// Signal only the HTTP lifetime. Its worker sends EOF and retains the encoder
// until the next same-ID GET replaces it. No generation or FLAC close here.
static void endSqueezeboxResponse(bool flush)
{
    std::lock_guard<std::mutex> lock(g_enc_mutex);
    endedByPause = get_squeezebox_stream_id();
    if (deferCloseLog) closeLog.observe(endedByPause);
    if (activeRequest) activeRequest->expectedCloseAt = std::chrono::steady_clock::now();
    const bool preserve = flush && activeRequest && activeRequest->heldResume
        && !activeRequest->serving;
    if (preserve) activeRequest->resumeAcknowledged = false;
    if (!preserve && g_enc && g_enc->streamId() == endedByPause) g_enc->endResponse();
    // A pause ends existing requests, not a handoff to a standby. Standbys
    // have sent no headers and disconnect silently; later GETs may wait for PCM.
    for (auto& request : standbyRequests)
        request->pauseEnded = true;
    standbyRequests.clear();
}

extern "C" void note_squeezebox_device_close(void)
{
    if (deferCloseLog) closeLog.observe(get_squeezebox_stream_id());
    std::lock_guard<std::mutex> lock(g_enc_mutex);
    if (activeRequest) activeRequest->expectedCloseAt = std::chrono::steady_clock::now();
}

extern "C" {
void end_squeezebox_response(void) { endSqueezeboxResponse(false); }
void flush_squeezebox_response(void) { endSqueezeboxResponse(true); }

void hold_squeezebox_resume(unsigned stream)
{
    std::lock_guard<std::mutex> lock(g_enc_mutex);
    if (activeRequest && activeRequest->stream == stream && !activeRequest->encoder->hasAudio()
        && !activeRequest->encoder->responseEnded() && !activeRequest->encoder->cancelled())
        activeRequest->heldResume = true;
}

int squeezebox_response_streaming(unsigned stream)
{
    std::lock_guard<std::mutex> lock(g_enc_mutex);
    return activeRequest && activeRequest->stream == stream && activeRequest->serving
        && g_enc && g_enc->hasAudio() && !g_enc->cancelled() && !g_enc->responseEnded();
}

int squeezebox_request_open(unsigned stream)
{
    std::lock_guard<std::mutex> lock(g_enc_mutex);
    if (activeRequest && activeRequest->stream == stream) return 1;
    for (const auto& request : standbyRequests) if (request->stream == stream) return 1;
    return 0;
}

int squeezebox_response_open(unsigned stream)
{
    std::lock_guard<std::mutex> lock(g_enc_mutex);
    return g_enc && g_enc->streamId() == stream && !g_enc->cancelled()
        && !g_enc->responseEnded();
}

int squeezebox_response_ended(unsigned stream)
{
    std::lock_guard<std::mutex> lock(g_enc_mutex);
    return stream != 0 && endedByPause == stream;
}

void acknowledge_squeezebox_resume(unsigned stream)
{
    std::lock_guard<std::mutex> lock(g_enc_mutex);
    if (endedByPause == stream) endedByPause = 0;
    if (activeRequest && activeRequest->stream == stream)
        activeRequest->resumeAcknowledged = true;
}

void invalidate_squeezebox_held_get(unsigned stream)
{
    std::lock_guard<std::mutex> lock(g_enc_mutex);
    if (g_enc && g_enc->streamId() == stream && !g_enc->hasAudio() && !g_enc->responseEnded()) {
        printf("stream %u: invalidating held GET for same-URL resume\n", stream);
        g_enc->cancel();
    }
}

void encode_squeezebox_audio(const char* data, int len, uint64_t firstFrame)
{
    unsigned stream = get_squeezebox_stream_id();
    unsigned serial = get_lms_stream_serial();
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (sonos_output_running() && stream == get_squeezebox_stream_id() && serial == get_lms_stream_serial()) {
        std::shared_ptr<SBEncoder> enc;
        uint64_t requestId = 0;
        {
            std::lock_guard<std::mutex> lock(g_enc_mutex);
            enc = g_enc;
            if (activeRequest && enc == activeRequest->encoder) requestId = activeRequest->id;
        }
        if (enc && enc->streamId() == stream && !enc->cancelled() && !enc->responseEnded() && !enc->producerRetired()) {
            int written = enc->write(data, len, SBSTREAMER_TIMEOUT, [=] {
                sonos_position_pcm(stream, requestId, firstFrame);
            }, [] { return !sonos_output_running(); });
            if (written == len || !sonos_output_running()) return;
            // Active-request termination or resume may replace the encoder
            // while write waits. Retry the SAME PCM block on the new encoder.
            if (!enc->cancelled() && !enc->responseEnded() && !enc->producerRetired()) {
                printf("encode_squeezebox_audio: write() failed %d != %d\n", written, len);
                return;
            }
        }
        if (sonos_lms_is_paused())
            deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
        if (std::chrono::steady_clock::now() >= deadline) {
            printf("encode_squeezebox_audio: timeout waiting for stream request\n");
            return;
        }
        usleep(1000);
    }
}
} // extern "C"

void SBStreamer::registerWith(upnp::StreamServer& server)
{
    server.registerStream(SBSTREAMER_CNAME, SBSTREAMER_URI, SBSTREAMER_DESC,
        SBSTREAMER_CONTENT, SBSTREAMER_ICON,
        [this](upnp::StreamRequest& request) { return HandleRequest(&request); });
}

bool SBStreamer::HandleRequest(upnp::StreamRequest* handle)
{
    if (IsAborted() || handle->aborted())
        return false;

    const std::string& requestUri = handle->path();
    if (requestUri.compare(0, strlen(SBSTREAMER_URI), SBSTREAMER_URI) != 0)
        return false;

    const auto method = handle->method();
    if (method != upnp::StreamRequest::Method::Get && method != upnp::StreamRequest::Method::Head) return false;
    const std::string session = handle->parameter("session");
    if (session != streamSessionToken()) {
        printf("stale request: session %s != %s\n", session.c_str(), streamSessionToken().c_str());
        const std::string response = "HTTP/1.1 404 Not Found\r\nContent-Length: 0\r\nConnection: close\r\n\r\n";
        handle->send(response.c_str(), response.size());
        handle->disconnect();
        return true;
    }

    switch (method) {
    case upnp::StreamRequest::Method::Get: {
        int stream = atoi(handle->parameter("stream").c_str());
        streamSqueezeBox(handle, stream);
        return true;
    }
    case upnp::StreamRequest::Method::Head: {
        handle->reply(200, SBSTREAMER_CONTENT);
        return true;
    }
    default:
        return false;  // unhandled method
    }
}

void SBStreamer::streamSqueezeBox(upnp::StreamRequest* handle, int stream)
{
    printf("Sonos requested stream %d\n", stream);
    // Bound a stalled peer as well as a stalled PCM producer. noson SendData
    // uses the socket directly, so SetTimeout (receive only) is insufficient.
    handle->sendTimeout(500);
    auto peerClosed = [handle] { return handle->peerClosed(); };

    auto request = std::make_shared<StreamRequest>();
    {
        std::lock_guard<std::mutex> lock(g_enc_mutex);
        request->id = ++nextRequestId;
        request->stream = stream;
    }
    unsigned current = get_squeezebox_stream_id();
    if (stream <= 0 || (unsigned)stream > current) {
        handle->reply(400);
        return;
    }
    auto redirect = [&] {
        std::string url = SqueezeBoxURL(get_squeezebox_stream_id());
        std::string response = "HTTP/1.1 302 Found\r\nLocation: " + url
            + "\r\nContent-Length: 0\r\nCache-Control: no-store\r\nConnection: close\r\n\r\n";
        printf("stream %d: HTTP 302 -> %s\n", stream, url.c_str());
        handle->send(response.c_str(), response.size());
    };
    if ((unsigned)stream < current) {
        redirect();
        return;
    }

    {
        std::lock_guard<std::mutex> lock(g_enc_mutex);
        // A track change may have won the race since request validation.
        if ((unsigned)stream == get_squeezebox_stream_id()) {
            if (ownershipStream != (unsigned)stream) {
                if (g_enc) g_enc->cancel();
                activeRequest.reset();
                standbyRequests.clear(); // old workers redirect on their next poll
                ownershipStream = stream;
            }
            // Preserve pause-ended encoders until a later GET replaces them.
            if (activeRequest && activeRequest->encoder->responseEnded())
                activeRequest.reset();
            if (!activeRequest) {
                activateRequest(request, false);
            } else {
                standbyRequests.push_back(request);
                printf("stream %d: GET #%llu STANDBY\n", stream, request->id);
            }
        }
    }

    std::shared_ptr<SBEncoder> enc;
    bool opened = false;
    auto standbyDeadline = std::chrono::steady_clock::now()
        + std::chrono::milliseconds(SBSTREAMER_STANDBY_TIMEOUT);
    for (;;) {
        bool obsolete = false, disconnect = false;
        {
            std::lock_guard<std::mutex> lock(g_enc_mutex);
            obsolete = (unsigned)stream != get_squeezebox_stream_id();
            // An activated request must always take the active cleanup path,
            // even if its peer closes immediately after promotion.
            if (request->encoder) {
                enc = request->encoder;
                opened = request->opened;
                break;
            }
            if (peerClosed()) {
                printf("stream %d: GET #%llu standby closed by client\n", stream, request->id);
                disconnect = true;
            } else if (!obsolete && !request->pauseEnded && !(IsAborted() || handle->aborted())
                       && !sonos_lms_is_paused() && activeRequest
                       && activeRequest->encoder->hasAudio()
                       && std::chrono::steady_clock::now() >= standbyDeadline) {
                printf("stream %d: GET #%llu standby timeout\n", stream, request->id);
                disconnect = true;
            }
            disconnect = disconnect || request->pauseEnded || (IsAborted() || handle->aborted());
            if (obsolete || disconnect)
                standbyRequests.erase(std::remove(standbyRequests.begin(), standbyRequests.end(), request),
                                      standbyRequests.end());
        }
        if (disconnect || obsolete) {
            if (obsolete && !disconnect) redirect();
            handle->disconnect();
            return;
        }
        usleep(5000);
    }

    // Only ACTIVE gets an encoder and headers. A held GET still means an
    // ACTIVE request waiting for PCM while LMS is paused, never a standby.
    bool waitForResumeAudio = sonos_lms_is_paused() || request->heldResume;
    auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(SBSTREAMER_RESUME_TIMEOUT);
    auto waitingForAudio = [&] {
        return !enc->hasAudio() || (request->heldResume && !request->resumeAcknowledged);
    };
    while (opened && waitForResumeAudio && waitingForAudio() && !enc->cancelled() && !enc->responseEnded() && !(IsAborted() || handle->aborted()) && !peerClosed()
           && (unsigned)stream == get_squeezebox_stream_id()
           && std::chrono::steady_clock::now() < deadline) {
        // A marked resume already sent LMS play. Its q/s reply may clear the
        // transport state, but must not renew the hold or send another play.
        if (!request->heldResume) ResumeSqueezeBox(stream);
        usleep(10000);
    }
    char buf[SBSTREAMER_CHUNK];
    int r = 0;
    const bool heldResume = request->heldResume;
    const bool closedResume = heldResume && peerClosed();
    const bool expiredResume = heldResume && waitingForAudio()
        && std::chrono::steady_clock::now() >= deadline;
    if (opened && !closedResume && (!waitForResumeAudio || !waitingForAudio()) && !enc->cancelled())
        r = enc->read(buf, sizeof(buf), SBSTREAMER_HTTP_IDLE_TIMEOUT, false, peerClosed);
    bool streamReady = r >= 4 && memcmp(buf, "fLaC", 4) == 0;
    const std::string streamingHeaders = "HTTP/1.1 200 OK\r\nServer: " + handle->serverName() + "\r\nConnection: close\r\n"
        "Content-Type: audio/flac\r\nTransfer-Encoding: chunked\r\n\r\n";
    if (closedResume) {
        printf("held resume GET #%llu closed by client\n", request->id);
    } else if (!streamReady && waitForResumeAudio && (unsigned)stream != get_squeezebox_stream_id()) {
        if (heldResume)
            printf("held resume GET #%llu -> 302 stream %u\n", request->id, get_squeezebox_stream_id());
        // LMS play after stop can start a new delivery generation. The held
        // GET follows that URL instead of reporting an audio failure.
        redirect();
    } else if (!streamReady) {
        if (expiredResume) printf("held resume GET #%llu expired\n", request->id);
        printf("stream %d: no audio before timeout or connection replaced\n", stream);
        std::string error = "HTTP/1.1 503 Service Unavailable\r\nContent-Length: 0\r\nConnection: close\r\n\r\n";
        handle->send(error.c_str(), error.size());
    } else {
        request->serving = true;
        if (heldResume) printf("held resume GET #%llu fed\n", request->id);
        printf("stream %d: serving current generation with fresh FLAC header\n", stream);
        const auto responseStarted = std::chrono::steady_clock::now();
        size_t sent = 0;
        bool sendFailed = false;
        auto send = [&](const char* data, size_t size) {
            if (sendFailed) return false;
            errno = 0;
            if (handle->send(data, size)) {
                sent += size;
                return true;
            }
            const int error = errno; // capture before logging or cleanup changes it
            sendFailed = true;
            const double seconds = std::chrono::duration<double>(
                std::chrono::steady_clock::now() - responseStarted).count();
            if (deferCloseLog && (error == EPIPE || error == ECONNRESET)) {
                closeLog.failed(stream, seconds, sent, error);
                return false;
            }
            bool expected = false;
            {
                std::lock_guard<std::mutex> lock(g_enc_mutex);
                expected = (error == EPIPE || error == ECONNRESET)
                    && request->expectedCloseAt != std::chrono::steady_clock::time_point{}
                    && std::chrono::steady_clock::now() - request->expectedCloseAt < std::chrono::seconds(2);
            }
            if (expected)
                printf("stream %d: client closed after %.1f s (%zu bytes confirmed sent; %s; errno=%d)\n",
                    stream, seconds, sent, strerror(error), error);
            else printf("stream %d: send to Sonos failed after %.1f s (%s; errno=%d; "
                   "%zu bytes confirmed sent, partial failed write uncounted)\n",
                stream, seconds, error ? strerror(error) : "short write or closed socket",
                error, sent);
            return false;
        };
        auto sendChunk = [&](const char* data, size_t size) {
            char header[16];
            int length = snprintf(header, sizeof(header), "%x\r\n", (unsigned)size);
            return send(header, length) && send(data, size) && send("\r\n", 2);
        };
        if (send(streamingHeaders.c_str(), streamingHeaders.size()) && sendChunk(buf, r)) {
            while (!(IsAborted() || handle->aborted()) && (r = enc->read(buf, sizeof(buf), SBSTREAMER_HTTP_IDLE_TIMEOUT, false, peerClosed)) > 0) {
                if (!sendChunk(buf, r)) break;
            }
            send("0\r\n\r\n", 5);
        }
    }
    if (peerClosed()) printf("stream %d: client closed connection\n", stream);
    {
        std::lock_guard<std::mutex> lock(g_enc_mutex);
        const bool pauseEnded = enc->responseEnded();
        if (!pauseEnded) enc->cancel();
        if (activeRequest == request) {
            activeRequest.reset();
            if (!pauseEnded) {
                if (g_enc == enc) g_enc.reset();
                if ((unsigned)stream == get_squeezebox_stream_id() && !standbyRequests.empty()) {
                    auto newest = std::max_element(standbyRequests.begin(), standbyRequests.end(),
                        [](const std::shared_ptr<StreamRequest>& a, const std::shared_ptr<StreamRequest>& b) {
                            return a->id < b->id;
                        });
                    auto promoted = *newest;
                    standbyRequests.erase(newest);
                    activateRequest(promoted, true);
                }
            }
        }
    }
    // Preserve paused encoders and send EOF before disconnecting.
    handle->disconnect();
    if (!enc->responseEnded()) enc->close();
    printf("stream %d: done\n", stream);

    printf("Done serving stream %d to Sonos\n", stream);
}
