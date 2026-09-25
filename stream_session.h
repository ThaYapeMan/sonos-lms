#ifndef STREAM_SESSION_H
#define STREAM_SESSION_H

#include <openssl/rand.h>
#include <stdexcept>
#include <string>

// One cryptographically random identity shared by all streams and HTTP workers
// in this process. Fail startup rather than reuse a predictable fallback.
inline const std::string& streamSessionToken() {
    static const std::string token = [] {
        unsigned char bytes[8];
        if (RAND_bytes(bytes, sizeof(bytes)) != 1)
            throw std::runtime_error("cannot generate stream session token");
        const char* hex = "0123456789abcdef";
        std::string result;
        for (unsigned char byte : bytes) {
            result += hex[byte >> 4];
            result += hex[byte & 15];
        }
        return result;
    }();
    return token;
}
#endif
