#pragma once
#include <cstdio>
#include <cstdlib>
#include <cstring>
namespace upnp {
enum class Backend { Noson, Own };
inline Backend backend() {
    static const Backend selected = [] {
        const char* value = std::getenv("SONOS_LMS_UPNP");
        Backend result = Backend::Noson;
        const bool alias = value && std::strcmp(value, "own") == 0;
        if (alias || (value && std::strcmp(value, "yeney") == 0)) result = Backend::Own;
        else if (value && std::strcmp(value, "noson") != 0)
            printf("Warning: invalid SONOS_LMS_UPNP='%s'; using noson\n", value);
        printf("UPnP layer: %s%s\n", result == Backend::Own ? "yeney" : "noson",
            alias ? " (alias own)" : "");
        return result;
    }();
    return selected;
}
}
