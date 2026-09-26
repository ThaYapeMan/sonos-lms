#include "http.h"
#include <algorithm>
#include <arpa/inet.h>
#include <cerrno>
#include <chrono>
#include <climits>
#include <cstring>
#include <fcntl.h>
#include <poll.h>
#include <stdexcept>
#include <sys/socket.h>
#include <unistd.h>
namespace upnp {
namespace {
constexpr size_t limit = 2 * 1024 * 1024;
std::string lower(std::string s) { for (char& c : s) if (c >= 'A' && c <= 'Z') c += 'a' - 'A'; return s; }
std::string trim(const std::string& s) {
    auto a = s.find_first_not_of(" \t\r\n"), b = s.find_last_not_of(" \t\r\n");
    return a == std::string::npos ? "" : s.substr(a, b - a + 1);
}
bool number(const std::string& s, size_t& n, unsigned base = 10) {
    if (s.empty()) return false;
    n = 0;
    for (char c : s) {
        unsigned d = c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : 99;
        if (d >= base || n > (limit - d) / base) return false;
        n = n * base + d;
    }
    return true;
}
struct Socket {
    int fd = -1;
    ~Socket() { if (fd >= 0) close(fd); }
};
using Clock = std::chrono::steady_clock;
void waitSocket(int fd, short events, Clock::time_point deadline) {
    for (;;) {
        auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - Clock::now()).count();
        if (ms <= 0) throw std::runtime_error("timeout");
        pollfd p{fd, events, 0};
        int n = poll(&p, 1, static_cast<int>(std::min<long long>(ms, INT_MAX)));
        if (n > 0) return;
        if (n == 0) throw std::runtime_error("timeout");
        if (errno != EINTR) throw std::runtime_error(strerror(errno));
    }
}
}
bool parseHttpUrl(const std::string& input, HttpUrl& out) {
    out = {};
    if (input.compare(0, 7, "http://") != 0 || input.find_first_of("\r\n\t ") != std::string::npos) return false;
    size_t slash = input.find('/', 7);
    auto authority = input.substr(7, slash == std::string::npos ? std::string::npos : slash - 7);
    auto colon = authority.find(':');
    out.host = authority.substr(0, colon); out.port = 80;
    if (colon != std::string::npos) {
        size_t port;
        if (!number(authority.substr(colon + 1), port) || !port || port > 65535) return false;
        out.port = port;
    }
    in_addr ip{};
    if (inet_pton(AF_INET, out.host.c_str(), &ip) != 1) return false;
    out.path = slash == std::string::npos ? "/" : input.substr(slash);
    return true;
}
HttpResponse httpRequest(const std::string& method, const HttpUrl& url, const std::map<std::string, std::string>& headers,
                      const std::string& body, unsigned timeoutMs) {
    HttpResponse result;
    try {
        if (method.empty() || method.find_first_not_of("ABCDEFGHIJKLMNOPQRSTUVWXYZ") != std::string::npos) throw std::runtime_error("invalid method");
        if (!timeoutMs || !url.port || url.port > 65535 || url.path.empty() || url.path[0] != '/'
            || url.path.find_first_of("\r\n ") != std::string::npos) throw std::runtime_error("invalid endpoint");
        sockaddr_in peer{}; peer.sin_family = AF_INET; peer.sin_port = htons(url.port);
        if (inet_pton(AF_INET, url.host.c_str(), &peer.sin_addr) != 1) throw std::runtime_error("numeric IPv4 required");
        Socket socket; socket.fd = ::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
        if (socket.fd < 0) throw std::runtime_error(strerror(errno));
        const auto deadline = Clock::now() + std::chrono::milliseconds(timeoutMs);
        if (connect(socket.fd, reinterpret_cast<sockaddr*>(&peer), sizeof(peer)) < 0) {
            if (errno != EINPROGRESS) throw std::runtime_error(strerror(errno));
            waitSocket(socket.fd, POLLOUT, deadline);
            int error = 0; socklen_t len = sizeof(error);
            if (getsockopt(socket.fd, SOL_SOCKET, SO_ERROR, &error, &len) < 0) throw std::runtime_error(strerror(errno));
            if (error) throw std::runtime_error(strerror(error));
        }
        sockaddr_in local{}; socklen_t len = sizeof(local);
        if (getsockname(socket.fd, reinterpret_cast<sockaddr*>(&local), &len) == 0) {
            char address[INET_ADDRSTRLEN];
            if (inet_ntop(AF_INET, &local.sin_addr, address, sizeof(address))) result.localAddress = address;
        }
        std::string request = method + " " + url.path + " HTTP/1.1\r\nHost: " + url.host + ":" + std::to_string(url.port)
            + "\r\nConnection: close\r\nContent-Length: " + std::to_string(body.size()) + "\r\n";
        for (const auto& h : headers) {
            if (h.first.find_first_of(":\r\n") != std::string::npos || h.second.find_first_of("\r\n") != std::string::npos)
                throw std::runtime_error("invalid header");
            request += h.first + ": " + h.second + "\r\n";
        }
        request += "\r\n" + body;
        size_t offset = 0;
        while (offset < request.size()) {
            waitSocket(socket.fd, POLLOUT, deadline);
            ssize_t n = send(socket.fd, request.data() + offset, request.size() - offset, MSG_NOSIGNAL);
            if (n < 0 && (errno == EAGAIN || errno == EINTR)) continue;
            if (n <= 0) throw std::runtime_error(n ? strerror(errno) : "short send");
            offset += n;
        }
        std::string wire;
        auto receive = [&] {
            waitSocket(socket.fd, POLLIN, deadline);
            char buffer[8192];
            ssize_t n = recv(socket.fd, buffer, sizeof(buffer), 0);
            if (n < 0 && (errno == EAGAIN || errno == EINTR)) return true;
            if (n < 0) throw std::runtime_error(strerror(errno));
            if (wire.size() + static_cast<size_t>(n) > limit) throw std::runtime_error("response too large");
            wire.append(buffer, n); return n != 0;
        };
        size_t split;
        while ((split = wire.find("\r\n\r\n")) == std::string::npos) {
            if (wire.size() > 65536 || !receive()) throw std::runtime_error("invalid HTTP headers");
        }
        if (wire.compare(0, 9, "HTTP/1.1 ") && wire.compare(0, 9, "HTTP/1.0 ")) throw std::runtime_error("invalid HTTP status");
        size_t status;
        if (!number(wire.substr(9, 3), status) || status < 100 || status > 599) throw std::runtime_error("invalid HTTP status");
        result.status = status;
        std::map<std::string, std::string> fields;
        size_t p = wire.find("\r\n") + 2;
        while (p < split) {
            auto end = wire.find("\r\n", p), colon = wire.find(':', p);
            if (colon == std::string::npos || colon >= end) throw std::runtime_error("invalid HTTP header");
            auto key = lower(wire.substr(p, colon - p));
            if (!fields.emplace(key, trim(wire.substr(colon + 1, end - colon - 1))).second)
                throw std::runtime_error("duplicate HTTP header");
            p = end + 2;
        }
        result.headers = fields;
        p = split + 4;
        auto need = [&](size_t end) {
            if (end > limit) throw std::runtime_error("response too large");
            while (wire.size() < end) if (!receive()) throw std::runtime_error("truncated HTTP body");
        };
        if (fields.count("transfer-encoding")) {
            if (lower(fields["transfer-encoding"]) != "chunked" || fields.count("content-length"))
                throw std::runtime_error("unsupported HTTP framing");
            for (;;) {
                size_t end;
                while ((end = wire.find("\r\n", p)) == std::string::npos)
                    if (!receive()) throw std::runtime_error("truncated chunk");
                auto sizeText = wire.substr(p, end - p); size_t size;
                sizeText = sizeText.substr(0, sizeText.find(';'));
                if (!number(sizeText, size, 16)) throw std::runtime_error("invalid chunk size");
                p = end + 2;
                if (!size) {
                    // Consume trailers through their empty terminating line.
                    for (;;) {
                        while ((end = wire.find("\r\n", p)) == std::string::npos)
                            if (!receive()) throw std::runtime_error("truncated trailers");
                        if (end == p) break;
                        p = end + 2;
                    }
                    break;
                }
                need(p + size + 2);
                if (wire.compare(p + size, 2, "\r\n")) throw std::runtime_error("invalid chunk ending");
                result.body.append(wire, p, size); p += size + 2;
            }
        } else if (fields.count("content-length")) {
            size_t size;
            if (!number(fields["content-length"], size)) throw std::runtime_error("invalid Content-Length");
            need(p + size); result.body = wire.substr(p, size);
        } else {
            while (receive()) {}
            result.body = wire.substr(p);
        }
    } catch (const std::runtime_error& error) { result.error = error.what(); result.body.clear(); }
    return result;
}
HttpResponse httpPost(const HttpUrl& url, const std::map<std::string, std::string>& headers,
                      const std::string& body, unsigned timeoutMs) {
    return httpRequest("POST", url, headers, body, timeoutMs);
}
HttpResponse httpGet(const HttpUrl& url, unsigned timeoutMs) {
    return httpRequest("GET", url, {}, "", timeoutMs);
}

}
