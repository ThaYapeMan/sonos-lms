#ifndef RESUME_RESPONSE_H
#define RESUME_RESPONSE_H

#include <cstdio>
#include <cstdlib>
#include <cstring>

struct ResumeResponse {
    bool frames = false;
    bool raw = false;
    const char* bodyName() const { return frames ? "frames" : "header"; }
    const char* transferName() const { return raw ? "raw" : "chunked"; }
};

inline bool parseResumeSwitch(const char* name, const char* value,
                              const char* normal, const char* alternate) {
    if (!value || !std::strcmp(value, normal)) return false;
    if (!std::strcmp(value, alternate)) return true;
    printf("Warning: unknown %s='%s'; using %s\n", name, value, normal);
    return false;
}

inline const ResumeResponse& resumeResponseSettings() {
    static const ResumeResponse settings = [] {
        ResumeResponse selected;
        selected.frames = parseResumeSwitch("SONOS_SQUEEZEBOX_RESUME_BODY",
            std::getenv("SONOS_SQUEEZEBOX_RESUME_BODY"), "header", "frames");
        selected.raw = parseResumeSwitch("SONOS_SQUEEZEBOX_RESUME_TRANSFER",
            std::getenv("SONOS_SQUEEZEBOX_RESUME_TRANSFER"), "chunked", "raw");
        printf("SONOS_SQUEEZEBOX_RESUME_BODY=%s\nSONOS_SQUEEZEBOX_RESUME_TRANSFER=%s\n",
            selected.bodyName(), selected.transferName());
        return selected;
    }();
    return settings;
}
#endif
