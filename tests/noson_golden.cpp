// Execute noson's actual metadata serializer and SOAP client against a local fixture.
#include "digitalitem.h"
#include "avtransport.h"
#include <cstdio>
#include <cstdlib>
#include <iostream>
int main(int argc, char** argv) {
    const std::string url = "http://bridge:1400/music/squeezebox.flac?session=0123456789abcdef&stream=7";
    SONOS::DigitalItem item(SONOS::DigitalItem::Type_item, SONOS::DigitalItem::SubType_audioItem);
    item.SetProperty("dc:title", "A & B <Live> \"Mix\" '26");
    item.SetProperty("r:streamContent", "");
    item.SetProperty("upnp:albumArtURI", "http://lms:9000/art?a=1&b=2");
    SONOS::ElementPtr res(new SONOS::Element("res", url));
    res->SetAttribut("protocolInfo", "x-rincon-mp3radio:*:audio/flac:*");
    item.SetProperty(res);
    std::cout << item.DIDL() << '\n';
    if (argc > 1) {
        SONOS::AVTransport avt("127.0.0.1", std::strtoul(argv[argc - 1], nullptr, 10));
        if (!avt.SetCurrentURI(url, item.DIDL())) return 1;
        if (argc > 2) {
            SONOS::ElementList vars;
            if (!avt.Play() || !avt.Pause() || !avt.Stop() || !avt.GetTransportInfo(vars)
                || !avt.GetPositionInfo(vars) || !avt.GetMediaInfo(vars)) return 1;
        }
        return 0;
    }
}
