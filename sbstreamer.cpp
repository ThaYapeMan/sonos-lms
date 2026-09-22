// sbstreamer.cpp -- HTTP request broker that serves the FLAC stream to Sonos
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

#include "sbstreamer.h"

#include "data/datareader.h"
#include "imageservice.h"
#include "private/tokenizer.h"
#include "private/urlencoder.h"
#include "requestbroker.h"
#include "private/requestbrokeropaque.h"
#include "private/socket.h"
#include <sys/socket.h>
#include "sbencoder.h"

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
#define SBSTREAMER_MAX_PLAYBACK 3
#define SBSTREAMER_CHUNK 16384

using namespace NSROOT;

// The LMS generation outlives every HTTP connection. Each new connection owns
// a fresh FLAC encoder in that generation; the producer follows the active one.
static std::shared_ptr<SBEncoder> g_enc;
static std::mutex g_enc_mutex;
static std::vector<std::weak_ptr<SBEncoder>> connections;
static unsigned endedByPause = 0;
extern void ResumeSqueezeBox(unsigned current);
extern std::string SqueezeBoxURL(unsigned current);
extern "C" unsigned get_squeezebox_stream_id(void);
extern "C" unsigned get_lms_stream_serial(void);
extern "C" int sonos_lms_is_paused(void);

extern "C" {
// Signal only the HTTP lifetime. Its worker sends EOF and retains the encoder
// until the next same-ID GET replaces it. No generation or FLAC close here.
void end_squeezebox_response(void)
{
    std::lock_guard<std::mutex> lock(g_enc_mutex);
    endedByPause = get_squeezebox_stream_id();
    for (auto& connection : connections)
        if (auto enc = connection.lock())
            if (enc->streamId() == endedByPause) enc->endResponse();
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
}

void encode_squeezebox_audio(const char* data, int len)
{
    unsigned stream = get_squeezebox_stream_id();
    unsigned serial = get_lms_stream_serial();
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (stream == get_squeezebox_stream_id() && serial == get_lms_stream_serial()) {
        std::shared_ptr<SBEncoder> enc;
        {
            std::lock_guard<std::mutex> lock(g_enc_mutex);
            enc = g_enc;
        }
        if (enc && enc->streamId() == stream && !enc->cancelled() && !enc->responseEnded() && !enc->producerRetired()) {
            int written = enc->write(data, len, SBSTREAMER_TIMEOUT);
            if (written == len) return;
            // A probe, real GET, or resume may replace the connection while
            // write waits. Retry the SAME PCM block on the new encoder.
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

SBStreamer::SBStreamer(RequestBroker* imageService)
    : RequestBroker()
    , m_resources()
    , m_playbackCount(0)
{
    ResourcePtr icon;
    if (imageService) {
        icon = imageService->RegisterResource(SBSTREAMER_CNAME, "Icon for " SBSTREAMER_CNAME,
            SBSTREAMER_ICON, DataReader::Instance());
    }

    ResourcePtr streamResource(new Resource());
    streamResource->uri = SBSTREAMER_URI;
    streamResource->title = SBSTREAMER_CNAME;
    streamResource->description = SBSTREAMER_DESC;
    streamResource->contentType = SBSTREAMER_CONTENT;
    if (icon)
        streamResource->iconUri.assign(icon->uri).append("?id=" LIBVERSION);

    m_resources.push_back(streamResource);
}

bool SBStreamer::HandleRequest(handle* handle)
{
    if (IsAborted())
        return false;

    const std::string& requestUri = RequestBroker::GetRequestURI(handle);
    if (requestUri.compare(0, strlen(SBSTREAMER_URI), SBSTREAMER_URI) != 0)
        return false;

    switch (RequestBroker::GetRequestMethod(handle)) {
    case RequestBroker::Method_GET: {
        std::vector<std::string> params;
        readParameters(requestUri, params);
        int stream = atoi(getParamValue(params, "stream").c_str());
        streamSqueezeBox(handle, stream);
        return true;
    }
    case RequestBroker::Method_HEAD: {
        std::string response = std::string(RequestBroker::MakeResponseHeader(RequestBroker::Status_OK))
            + "Content-Type: audio/flac\r\n\r\n";
        RequestBroker::Reply(handle, response.c_str(), response.length());
        return true;
    }
    default:
        return false;  // unhandled method
    }
}

RequestBroker::ResourcePtr SBStreamer::GetResource(const std::string& title)
{
    (void)title;
    return m_resources.front();
}

RequestBroker::ResourceList SBStreamer::GetResourceList()
{
    return ResourceList(m_resources.begin(), m_resources.end());
}

RequestBroker::ResourcePtr SBStreamer::RegisterResource(const std::string& title,
    const std::string& description, const std::string& path, StreamReader* delegate)
{
    // This broker exposes exactly one fixed resource (the live stream); it
    // does not support registering additional ones.
    (void)title;
    (void)description;
    (void)path;
    (void)delegate;
    return ResourcePtr();
}

void SBStreamer::UnregisterResource(const std::string& uri)
{
    (void)uri;
}

// Send one HTTP chunk: hex-size CRLF data CRLF
bool SBStreamer::sendChunk(handle* h, const char* data, size_t size)
{
    char hdr[16];
    int hlen = snprintf(hdr, sizeof(hdr), "%x\r\n", (unsigned)size);
    return RequestBroker::Reply(h, hdr, hlen)
        && RequestBroker::Reply(h, data, size)
        && RequestBroker::Reply(h, "\r\n", 2);
}

void SBStreamer::streamSqueezeBox(handle* handle, int stream)
{
    printf("Sonos requested stream %d\n", stream);
    // Bound a stalled peer as well as a stalled PCM producer. noson SendData
    // uses the socket directly, so SetTimeout (receive only) is insufficient.
    timeval sendTimeout{0, 500000};
    setsockopt(handle->payload->socket->GetHandle(), SOL_SOCKET, SO_SNDTIMEO,
               &sendTimeout, sizeof(sendTimeout));

    auto peerClosed = [handle] {
        auto socket = handle->payload->socket;
        if (!socket->IsValid()) return true;
        if (socket->GetHandle() < 0) return false; // in-memory test socket
        char byte;
        int result = recv(socket->GetHandle(), &byte, 1, MSG_PEEK | MSG_DONTWAIT);
        return result == 0 || (result < 0 && errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR);
    };

    unsigned current = get_squeezebox_stream_id();
    if (stream <= 0 || (unsigned)stream > current) {
        Reply400(handle);
        return;
    }
    // Only superseded LMS streams redirect. Current-ID GETs (including the
    // normal probe/second GET and either kind of resume) NEVER change the ID.
    if ((unsigned)stream < current) {
        std::string url = SqueezeBoxURL(current);
        std::string redirect = "HTTP/1.1 302 Found\r\nLocation: " + url
            + "\r\nContent-Length: 0\r\nCache-Control: no-store\r\nConnection: close\r\n\r\n";
        printf("stream %d: HTTP 302 -> %s\n", stream, url.c_str());
        RequestBroker::Reply(handle, redirect.c_str(), redirect.size());
        return;
    }

    m_playbackCount.Add(1);
    if (m_playbackCount.Load() > SBSTREAMER_MAX_PLAYBACK) {
        Reply429(handle);
    } else {
        auto enc = std::make_shared<SBEncoder>(stream);
        bool opened = enc->open();
        if (opened) {
            std::lock_guard<std::mutex> lock(g_enc_mutex);
            // A track change may have won the race since request validation.
            // Never let an old GET cancel the newer generation's connection.
            if ((unsigned)stream == get_squeezebox_stream_id()) {
                if (g_enc) {
                    if (g_enc->streamId() == (unsigned)stream)
                        g_enc->retireProducer(); // stop feeding, NEVER cancel its reader
                    else
                        g_enc->cancel();
                }
                g_enc = enc;
                connections.erase(std::remove_if(connections.begin(), connections.end(),
                    [](const std::weak_ptr<SBEncoder>& c) { return c.expired(); }), connections.end());
                connections.push_back(enc);
            } else {
                opened = false;
            }
        }

        // A running stream's probe and real GET both get fresh FLAC headers
        // immediately. Only a GET arriving before LMS unpause waits for strm u.
        // Same-ID predecessors retain their reader until the client closes or
        // the existing idle deadline expires; only the newest receives PCM.
        bool waitForResumeAudio = sonos_lms_is_paused();
        auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(SBSTREAMER_RESUME_TIMEOUT);
        while (opened && waitForResumeAudio && !enc->hasAudio() && !enc->cancelled() && !enc->responseEnded() && !IsAborted() && !peerClosed()
               && (unsigned)stream == get_squeezebox_stream_id()
               && std::chrono::steady_clock::now() < deadline) {
            ResumeSqueezeBox(stream);
            usleep(10000);
        }
        char buf[SBSTREAMER_CHUNK];
        int r = 0;
        if (opened && (!waitForResumeAudio || enc->hasAudio()) && !enc->cancelled())
            r = enc->read(buf, sizeof(buf), SBSTREAMER_HTTP_IDLE_TIMEOUT, false, peerClosed);
        if (r < 4 || memcmp(buf, "fLaC", 4) != 0) {
            printf("stream %d: no audio before timeout or connection replaced\n", stream);
            std::string error = "HTTP/1.1 503 Service Unavailable\r\nContent-Length: 0\r\nConnection: close\r\n\r\n";
            RequestBroker::Reply(handle, error.c_str(), error.size());
        } else {
            std::string resp = RequestBroker::MakeResponseHeader(RequestBroker::Status_OK)
                + "Content-Type: audio/flac\r\nTransfer-Encoding: chunked\r\n\r\n";
            printf("stream %d: serving current generation with fresh FLAC header\n", stream);
            if (RequestBroker::Reply(handle, resp.c_str(), resp.size()) && sendChunk(handle, buf, r)) {
                while (!IsAborted() && (r = enc->read(buf, sizeof(buf), SBSTREAMER_HTTP_IDLE_TIMEOUT, false, peerClosed)) > 0) {
                    if (!sendChunk(handle, buf, r)) break;
                }
                RequestBroker::Reply(handle, "0\r\n\r\n", 5);
            }
        }
        if (peerClosed()) printf("stream %d: client closed connection\n", stream);
        // EOF above must precede socket close. Keep a paused encoder alive;
        // the next same-ID request cancels/replaces it, with a fresh FLAC header.
        handle->payload->socket->Disconnect();
        if (!enc->responseEnded()) {
            enc->cancel();
            {
                std::lock_guard<std::mutex> lock(g_enc_mutex);
                if (g_enc == enc) g_enc.reset();
            }
            enc->close();
        }
        printf("stream %d: done\n", stream);
    }

    m_playbackCount.Sub(1);
    printf("Done serving stream %d to Sonos\n", stream);
}

void SBStreamer::Reply400(handle* handle)
{
    std::string response = std::string(RequestBroker::MakeResponseHeader(RequestBroker::Status_Bad_Request)) + "\r\n";
    RequestBroker::Reply(handle, response.c_str(), response.length());
}

void SBStreamer::Reply429(handle* handle)
{
    std::string response = std::string(RequestBroker::MakeResponseHeader(RequestBroker::Status_Too_Many_Requests)) + "\r\n";
    RequestBroker::Reply(handle, response.c_str(), response.length());
}

void SBStreamer::readParameters(const std::string& streamUrl, std::vector<std::string>& params)
{
    size_t queryStart = streamUrl.find('?');
    if (queryStart != std::string::npos)
        tokenize(streamUrl.substr(queryStart + 1), "&", params, true);
}

std::string SBStreamer::getParamValue(const std::vector<std::string>& params, const std::string& name)
{
    size_t prefixLen = name.length() + 1;  // name + '='
    for (const std::string& param : params) {
        if (param.length() > prefixLen && param.at(name.length()) == '=' && param.compare(0, name.length(), name) == 0)
            return urldecode(param.substr(prefixLen));
    }
    return std::string();
}
