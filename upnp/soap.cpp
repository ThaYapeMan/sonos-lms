#include "soap.h"
namespace upnp {
std::string soapBody(const std::string& service, const std::string& action, const SoapArguments& args) {
    std::string body = "<?xml version=\"1.0\" encoding=\"utf-8\"?>"
        "<s:Envelope xmlns:s=\"http://schemas.xmlsoap.org/soap/envelope/\" s:encodingStyle=\"http://schemas.xmlsoap.org/soap/encoding/\">"
        "<s:Body><u:" + action + " xmlns:u=\"urn:schemas-upnp-org:service:" + service + ":1\">";
    for (const auto& a : args) body += "<" + a.first + ">" + xmlEscape(a.second) + "</" + a.first + ">";
    return body + "</u:" + action + "></s:Body></s:Envelope>";
}
std::string streamDidl(const std::string& url, const std::string& title, const std::string& art) {
    if (url.find(':') == std::string::npos) return {};
    auto path = url.substr(0, url.find('?'));
    bool flac = path.size() >= 5 && path.compare(path.size() - 5, 5, ".flac") == 0;
    auto uri = flac ? url : "x-rincon-mp3radio" + url.substr(url.find(':'));
    std::string didl = "<DIDL-Lite xmlns=\"urn:schemas-upnp-org:metadata-1-0/DIDL-Lite/\""
        " xmlns:r=\"urn:schemas-rinconnetworks-com:metadata-1-0/\" xmlns:dc=\"http://purl.org/dc/elements/1.1/\""
        " xmlns:upnp=\"urn:schemas-upnp-org:metadata-1-0/upnp/\" ><item><upnp:class>object.item.audioItem</upnp:class>"
        "<dc:title>" + xmlEscape(title) + "</dc:title><r:streamContent></r:streamContent>";
    if (!art.empty()) didl += "<upnp:albumArtURI>" + xmlEscape(art) + "</upnp:albumArtURI>";
    return didl + "<res protocolInfo=\"x-rincon-mp3radio:*:" + (flac ? "audio/flac" : "*")
        + ":*\">" + xmlEscape(uri) + "</res></item></DIDL-Lite>";
}
SoapResult parseSoap(const std::string& body, const std::string& action) {
    SoapResult result; XmlNode root;
    if (!parseXml(body, root)) return result;
    auto colon = root.name.find(':');
    if (root.name.substr(colon == std::string::npos ? 0 : colon + 1) != "Envelope") return result;
    const auto content = root.child("Body");
    if (!content) return result;
    if (const auto fault = content->child("Fault")) {
        const auto detail = fault->child("detail");
        const auto error = detail ? detail->child("UPnPError") : nullptr;
        if (error) { result.faultCode = error->value("errorCode"); result.faultDescription = error->value("errorDescription"); }
        else result.faultDescription = fault->value("faultstring");
        return result;
    }
    if (const auto response = content->child(action + "Response")) { result.ok = true; result.response = *response; }
    return result;
}
}
