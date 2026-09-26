#pragma once
#include <string>

// Log only routing identity, never an external URI's credentials or query.
inline std::string speakerUriDescription(const std::string& uri, const std::string& session) {
    const auto query = uri.find('?');
    std::string token, stream;
    if (query != std::string::npos) {
        size_t at = query + 1;
        while (at < uri.size()) {
            auto end = uri.find('&', at);
            auto item = uri.substr(at, end == std::string::npos ? end : end - at);
            if (item.compare(0, 8, "session=") == 0) token = item.substr(8);
            if (item.compare(0, 7, "stream=") == 0) stream = item.substr(7);
            if (end == std::string::npos) break;
            at = end + 1;
        }
    }
    if (!session.empty() && token == session && !stream.empty()
        && stream.find_first_not_of("0123456789") == std::string::npos)
        return "stream=" + stream + " session=" + session;
    auto colon = uri.find(':');
    auto scheme = colon == std::string::npos ? "unknown" : uri.substr(0, colon);
    if (scheme.empty() || scheme.find_first_not_of("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789+.-") != std::string::npos)
        scheme = "unknown";
    return "other " + scheme;
}
