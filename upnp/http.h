#pragma once
#include <map>
#include <string>
namespace upnp {
struct HttpUrl { std::string host, path; unsigned port = 0; };
bool parseHttpUrl(const std::string&, HttpUrl&);
struct HttpResponse { unsigned status = 0; std::string body, localAddress, error; std::map<std::string, std::string> headers{}; };
HttpResponse httpRequest(const std::string& method, const HttpUrl&,
                         const std::map<std::string, std::string>& headers,
                         const std::string& body = {}, unsigned timeoutMs = 2000);
// Numeric IPv4 endpoints; one absolute deadline covers connect/write/read.
HttpResponse httpGet(const HttpUrl&, unsigned timeoutMs = 1000);
HttpResponse httpPost(const HttpUrl&, const std::map<std::string, std::string>& headers,
                      const std::string& body, unsigned timeoutMs = 5000);
}
