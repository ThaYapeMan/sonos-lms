#ifndef DEVICE_RESUME_H
#define DEVICE_RESUME_H

#include <cstdio>
#include <cstdlib>
#include <cstring>

enum class DeviceResume { SameURL503, SameURLClose, SameURLEmpty200, PlayOnly, PlayOnlyFrames, FeedRestart };

inline bool isPlayOnly(DeviceResume mode) {
    return mode == DeviceResume::PlayOnly || mode == DeviceResume::PlayOnlyFrames;
}

inline const char* deviceResumeName(DeviceResume mode) {
    switch (mode) {
    case DeviceResume::SameURLClose: return "sameurl-close";
    case DeviceResume::SameURLEmpty200: return "sameurl-empty200";
    case DeviceResume::PlayOnly: return "playonly";
    case DeviceResume::FeedRestart: return "feed-restart";
    case DeviceResume::PlayOnlyFrames: return "playonly-frames";
    default: return "sameurl-503";
    }
}

inline DeviceResume deviceResumeStrategy() {
    static const DeviceResume mode = [] {
        const char* value = std::getenv("SONOS_SQUEEZEBOX_DEVICE_RESUME");
        DeviceResume selected = DeviceResume::SameURL503;
        if (value) {
            if (!std::strcmp(value, "sameurl-close")) selected = DeviceResume::SameURLClose;
            else if (!std::strcmp(value, "sameurl-empty200")) selected = DeviceResume::SameURLEmpty200;
            else if (!std::strcmp(value, "playonly")) selected = DeviceResume::PlayOnly;
            else if (!std::strcmp(value, "playonly-frames")) selected = DeviceResume::PlayOnlyFrames;
            else if (!std::strcmp(value, "feed-restart")) selected = DeviceResume::FeedRestart;
            else if (std::strcmp(value, "sameurl-503"))
                printf("Warning: unknown SONOS_SQUEEZEBOX_DEVICE_RESUME='%s'; using sameurl-503\n", value);
        }
        printf("SONOS_SQUEEZEBOX_DEVICE_RESUME=%s\n", deviceResumeName(selected));
        return selected;
    }();
    return mode;
}
#endif
