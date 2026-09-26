#include "upnp/xml.h"
#include "upnp/stream_server.h"
#include "upnp/soap.h"
#include "upnp/discovery.h"
#include <cassert>
#include <fstream>
#include <iostream>
#include <sstream>
using namespace upnp;
std::string file(const char* name) { std::ifstream in(name); assert(in); std::ostringstream out; out << in.rdbuf(); return out.str(); }
int main() {
    assert(streamHeaderLog({{"User-Agent", "Sonos"}, {"Range", "bytes=0-"},
        {"Icy-MetaData", "1"}, {"Connection", "close"}, {"x-Test", "a"},
        {"Some-SoNoS-Field", "b"}, {"Host", "ignored"}}) ==
        "User-Agent=Sonos; Range=bytes=0-; Icy-MetaData=1; Connection=close; x-Test=a; Some-SoNoS-Field=b");
    assert(streamHeaderLog({{"X-Long", std::string(100, 'x')}}) == "X-Long=" + std::string(80, 'x'));
    assert(streamHeaderLog({{"X-Test", "a\r\nb"}}) == "X-Test=a??b");
    assert(streamHeaderLog({{"Accept", "ignored"}}) == "(none)");
    std::cout << "PASS: stream GET header diagnostics select names case-insensitively, truncate at 80 and stay on one line\n";

    const std::string special = "&<>\"' café";
    assert(xmlEscape(special) == "&amp;&lt;&gt;&quot;' café");
    std::string decoded;
    assert(xmlUnescape(xmlEscape(special), decoded) && decoded == special);
    assert(xmlUnescape("&#65;&#x1f3b5;&apos;&amp;lt;", decoded) && decoded == "A🎵'&lt;");
    for (const auto bad : {"&broken;", "&", "&#;", "&#x;", "&#x110000;", "&#0;", "&#xD800;", "&#99999999999999999999;"}) assert(!xmlUnescape(bad, decoded));
    XmlNode root;
    assert(parseXml("<?xml version='1.0'?><!--x--><p:a x='A &amp; B'><p:b><![CDATA[<hi>]]></p:b><empty/></p:a>", root));
    assert(root.value("b") == "<hi>" && root.attribute("x") == "A & B" && root.child("empty"));
    for (const auto bad : {"<a><b></a>", "<a>", "<a/>junk", "<!DOCTYPE a><a/>", "<a x='1' x='2'/>", "<a x='<'/>", "<a>&fake;</a>", "<a><b/></wrong>"}) assert(!parseXml(bad, root));
    std::string deep;
    for (unsigned i=0; i<70; ++i) deep += "<a>";
    for (unsigned i=0; i<70; ++i) deep += "</a>";
    assert(!parseXml(deep, root));
    HttpUrl location;
    const std::string reply = "HTTP/1.1 200 OK\r\nst: urn:schemas-upnp-org:device:ZonePlayer:1\r\nLoCaTiOn: http://192.168.1.10:1400/xml/device_description.xml\r\n\r\n";
    assert(parseSsdpReply(reply, location));
    assert(location.host == "192.168.1.10" && location.port == 1400 && location.path == "/xml/device_description.xml");
    assert(!parseSsdpReply("HTTP/1.1 200 OK\r\nLOCATION: http://192.168.1.10:1400/x\r\n\r\n", location));
    assert(!parseSsdpReply(reply.substr(0, reply.size()-2), location));
    assert(!parseSsdpReply("HTTP/1.1 404 Missing\r\n\r\n", location));
    for (const auto bad : {"http://127.0.0.1:0/x", "http://127.0.0.1:70000/x", "http://localhost/x", "https://127.0.0.1/x", "http://127.0.0.1/x\r\nBad"}) assert(!parseHttpUrl(bad, location));
    const auto speakers = parseTopology(file("tests/fixtures/topology.xml"));
    assert(speakers.size() == 3);
    Speaker study, port;
    assert(matchRoom(speakers, "Study", study));
    assert(matchRoom(speakers, "Sonos Port", port));
    assert(study.uuid == "RINCON_00112233445501400" && study.ip == "127.0.0.1");
    assert(study.coordinator == "Study" && study.members == std::vector<std::string>({"Study", "Sonos Port"}));
    assert(port.coordinator == "Study" && port.members == study.members);
    Speaker combined;
    assert(matchRoom(speakers, "Study + Sonos Port", combined) && combined.uuid == study.uuid);
    assert(!matchRoom(speakers, "sonos port", combined) && !matchRoom(speakers, "Sonos", combined));
    std::cout << groupDescription(study) << '\n' << groupDescription(port) << '\n';
    const std::string url = "http://bridge:1400/music/squeezebox.flac?session=0123456789abcdef&stream=7";
    const auto didl = streamDidl(url, "A & B <Live> \"Mix\" '26", "http://lms:9000/art?a=1&b=2");
    assert(didl + "\n" == file("tests/fixtures/noson-didl.xml"));
    const auto body = soapBody("AVTransport", "SetAVTransportURI", {{"InstanceID", "0"}, {"CurrentURI", url}, {"CurrentURIMetaData", didl}});
    assert(body == file("tests/fixtures/noson-set-uri.xml"));
    std::ofstream("/tmp/sonos-own-set-uri.xml") << body;
    assert(streamDidl(url, "", "").find("albumArtURI") == std::string::npos);
    assert(streamDidl("http://bridge/track.mp3", "", "").find("x-rincon-mp3radio://bridge/track.mp3") != std::string::npos);
    const auto fault = parseSoap("<s:Envelope><s:Body><s:Fault><detail><UPnPError><errorCode>701</errorCode><errorDescription>Transition &amp; unavailable</errorDescription></UPnPError></detail></s:Fault></s:Body></s:Envelope>", "Play");
    assert(!fault.ok && fault.faultCode == "701" && fault.faultDescription == "Transition & unavailable");
    const auto media = parseSoap("<s:Envelope><s:Body><u:GetMediaInfoResponse><CurrentURI>http://external/?x=1&amp;y=2</CurrentURI><TrackURI>wrong</TrackURI></u:GetMediaInfoResponse></s:Body></s:Envelope>", "GetMediaInfo");
    assert(media.ok && media.response.value("CurrentURI") == "http://external/?x=1&y=2");
    assert(!parseSoap("<GetMediaInfoResponse/>", "GetMediaInfo").ok);
    std::cout << "PASS: XML, SSDP, room/group topology, faults, CurrentURI and byte-identical noson golden SOAP\n";
}
