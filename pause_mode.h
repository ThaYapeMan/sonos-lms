#ifndef PAUSE_MODE_H
#define PAUSE_MODE_H

#include <cstdio>
#include <cstdlib>
#include <cstring>

enum class PauseMode { Pause, Stop };
inline PauseMode pauseMode() {
    static const PauseMode mode = [] {
        const char* value = std::getenv("SONOS_SQUEEZEBOX_PAUSE");
        PauseMode selected = PauseMode::Pause;
        if (value && !std::strcmp(value, "stop")) selected = PauseMode::Stop;
        else if (value && std::strcmp(value, "pause"))
            printf("Warning: unknown SONOS_SQUEEZEBOX_PAUSE='%s'; using pause\n", value);
        printf("SONOS_SQUEEZEBOX_PAUSE=%s\n", selected == PauseMode::Stop ? "stop" : "pause");
        return selected;
    }();
    return mode;
}
#endif
