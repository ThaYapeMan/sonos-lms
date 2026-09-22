// sbstreamer.h -- HTTP request broker that serves the FLAC stream to Sonos
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

#ifndef SBSTREAMER_H
#define SBSTREAMER_H

#include "locked.h"
#include "requestbroker.h"

#include <vector>

#define SBSTREAMER_CNAME "squeezebox"
#define SBSTREAMER_URI "/music/squeezebox.flac"

namespace NSROOT {

// Registers "/music/squeezebox.flac" as a Sonos-facing HTTP resource and
// serves the live FLAC-encoded PCM squeezelite hands to encode_squeezebox_audio().
class SBStreamer : public RequestBroker {
public:
    explicit SBStreamer(RequestBroker* imageService = nullptr);
    ~SBStreamer() override = default;

    bool HandleRequest(handle* handle) override;

    const char* CommonName() override { return SBSTREAMER_CNAME; }
    RequestBroker::ResourcePtr GetResource(const std::string& title) override;
    RequestBroker::ResourceList GetResourceList() override;
    RequestBroker::ResourcePtr RegisterResource(const std::string& title, const std::string& description,
        const std::string& path, StreamReader* delegate) override;
    void UnregisterResource(const std::string& uri) override;

private:
    ResourceList m_resources;
    LockedNumber<int> m_playbackCount;

    void streamSqueezeBox(handle* handle, int stream);
    static bool sendChunk(handle* h, const char* data, size_t size);

    void Reply400(handle* handle);
    void Reply429(handle* handle);

    void readParameters(const std::string& streamUrl, std::vector<std::string>& params);
    std::string getParamValue(const std::vector<std::string>& params, const std::string& name);
};

}  // namespace NSROOT

#endif  // SBSTREAMER_H
