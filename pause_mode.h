#ifndef PAUSE_MODE_H
#define PAUSE_MODE_H

#include <cstdio>
#include <cstdlib>
#include <cstring>

enum class PauseMode { Pause, Stop };
inline PauseMode pauseMode() {
    static const PauseMode mode = [] {
        const char* value = std::getenv("SONOS_SQUEEZEBOX_PAUSE");
        PauseMode selected = PauseMode::Stop;
        if (value && !std::strcmp(value, "pause")) selected = PauseMode::Pause;
        else if (value && std::strcmp(value, "stop"))
            printf("Warning: unknown SONOS_SQUEEZEBOX_PAUSE='%s'; using stop\n", value);
        printf("SONOS_SQUEEZEBOX_PAUSE=%s\n", selected == PauseMode::Stop ? "stop" : "pause");
        return selected;
    }();
    return mode;
}
#endif
