# The UPnP layer

Phase 1 keeps noson as the default. The bridge talks to `upnp::SpeakerControl`
and `upnp::StreamServer`; the noson adapters contain the discovery/control and
HTTP implementation. HTTP handlers receive `upnp::StreamRequest`, never a noson
handle. No SMAPI service is implemented. HTTP/XML helpers must not depend on LMS,
stream generations, transport intent, or other bridge state.

## Inventory before extraction

References in this table are pinned to **b165e01**, so subsequent extraction does
not invalidate the inventory. `main` means startup or the main status loop;
`transport` means StopTimer/transport-intent dispatch under transportMutex;
`HTTP` means a noson request worker; `output` means squeezelite's PCM producer.
Paths without a prefix are repository-root paths.

| Calls / types and original file:line | Wire work and result | Caller |
|---|---|---|
| `System`, `PlayerPtr` (`sonos-lms.cpp:61`, `:62`), `System::Debug`, constructor (`:859`) | Own subscriptions, listener and player; callback sets atomic gEvent. Debug changes logging only. | main; callback on noson event thread |
| `System::Discover()` / `Discover(url)` (`sonos-lms.cpp:669`, `:680`) | SSDP broadcast/multicast discovery (or explicit seed); LOCATION supplies the host/port, topology/subscriptions initialize the household. Returns startup success. | main startup |
| `GetZonePlayerList`, `ZonePlayerList` (`sonos-lms.cpp:619`, `:872`); player `GetUUID/GetHost/GetPort` (`:625`) | Cached topology; printed device table. | main startup |
| `GetZoneList`, `ZoneList`, zone `GetZoneName/GetCoordinator` (`sonos-lms.cpp:632`, `:646`, `:875`) | Cached topology; exact case-sensitive combined zone name (`Study + Sonos Port`), not whitespace token matching. | main startup |
| `GetPlayer(zone, ..., callback)` (`sonos-lms.cpp:652`) | Controller for coordinator, subscriptions to AVTransport and RenderingControl. Failure aborts startup. | main startup |
| `GetTransportProperty`, `AVTProperty.TransportStatus` (`sonos-lms.cpp:141`) | Cached event data; ERROR_LOST_CONNECTION triggers same-URL PlayStream recovery. No SOAP polling here. | transport |
| `Stop/Pause/Play` (`sonos-lms.cpp:167`, `:288`) | AVTransport SOAP Stop, Pause, Play; InstanceID=0, Speed=1 on all three (including noson’s extra Speed argument on Pause/Stop). Result drives bounded retries. HTTP EOF precedes Pause/Stop. | transport |
| `GetRequestBroker`, `GetResource`, `ResourcePtr.uri/iconUri` (`sonos-lms.cpp:501`, `:560`) | Local lookup builds URL with session token and stream number, or artwork fallback. | main / HTTP redirect |
| `GetControllerUri` (`sonos-lms.cpp:504`, `:565`, `:698`) | Local controller address reachable from speaker plus listener port; no SOAP. | main / HTTP redirect |
| `GetTransportProperty().TransportState` (`sonos-lms.cpp:536`) | Event cache drives ObserveDeviceTransport and held-GET resume. Must remain nonblocking on HTTP worker. | HTTP / main |
| `PlayStream(url,title,art)` (`sonos-lms.cpp:568`, `:701`) | SetAVTransportURI followed by Play only on success. Exact metadata below. Success completes stream-start generation. | main / transport |
| `ElementList`, `GetPositionInfo`, `GetValue("RelTime")` (`sonos-lms.cpp:713`) | SOAP GetPositionInfo, InstanceID=0; noson caches successful result for 1 second. Feeds connection-specific position anchor. | main loop |
| `GetZone/GetCoordinator/GetUUID/GetZoneName` (`sonos-status.cpp:44`) | Cached identity; display name and squeezelite MAC. | main startup |
| `TransportPropertyEmpty`, `GetVolume(uuid,&volume)` (`sonos-status.cpp:58`, `:61`) | Event-cache availability; GetVolume sends SOAP to `/MediaRenderer/RenderingControl/Control` (InstanceID=0, Channel=Master). Result is displayed; failure displays zero. | main status refresh |
| `ElementList/GetPositionInfo/GetValue`, `GetTransportProperty`, `AVTProperty`, `DigitalItemPtr/GetValue` (`sonos-status.cpp:64`, `:68`) | Position SOAP/cache plus event transport metadata title, album, creator, status, state, duration; populate display. | main status refresh |
| `ImageService`, `FileStreamer`, `RequestBrokerPtr`, `RegisterRequestBroker` (`sonos-lms.cpp:866`) | HTTP routes: packaged icon, live stream, local file `--file`. FileStreamer handles finite file requests. | main registration; HTTP serving |
| `RequestBroker` inheritance, `Resource/ResourcePtr/ResourceList`, `StreamReader` (`sbstreamer.h:29`, `:38`; `sbstreamer.cpp:187`, `:247`, `:258`) | Local route/resource registry. RegisterResource returns empty; UnregisterResource is a no-op for the fixed live route. | main / noson dispatch |
| `ImageService::RegisterResource`, `DataReader::Instance`, `LIBVERSION` (`sbstreamer.cpp:193`, `:203`) | `/pulseaudio.png` packaged artwork; icon URI has `?id=<version>`. | main startup |
| `IsAborted`, `handle`, `WSRequestBroker::GetRequestPath/GetRequestMethod/GetURIParams`, `tokenize/urldecode` (`sbstreamer.cpp:210`, `:213`, `:217`, `:220`, `:478`) | Inspect GET/HEAD and query; reject stale sessions, classify ACTIVE/STANDBY. | HTTP |
| `ReplyData`, `Socket/Disconnect/GetHandle/IsValid` (`sbstreamer.cpp:225`, `:281`, `:292`, `:296`, `:320`, `:382`, `:426`, `:431`, `:435`, `:460`) | Raw HTTP headers, FLAC chunks, terminating chunk, redirect/error responses; 500ms send timeout and nonblocking peer probe. ReplyData reports boolean, not partial byte count. | HTTP |
| `WSRequestReply/AddHeader/PostReply`, WS method/header/status enums (`sbstreamer.cpp:237`, `:467`) | Existing noson HEAD 200 and 400 formatting, kept by request adapter. | HTTP |
| `AudioFormat`, `RingBuffer/RingBufferPacket`, byte-order helpers (`sbencoder.h:16`, `:28`; `sbencoder.cpp:20`, `:88`, `:114`, `:160`) | No network: PCM format, little-endian sample decoding, encoded FLAC packet queue. These are encoder utilities, not speaker control. | output and HTTP, protected by encoder mutex |

## Event data versus polling

`noson/noson/src/sonosplayer.cpp:255` copies `AVTransport::GetAVTProperty()`.
`AVTransport` subscribes to `/MediaRenderer/AVTransport/Event` in
`noson/noson/src/avtransport.cpp:69`. GENA NOTIFY arrives at the same noson listener
that hosts HTTP streams; AVTransport parses LastChange and updates its locked
property cache, then invokes the bridge callback. Reading GetTransportProperty
never invokes GetTransportInfo. RenderingControl and ZoneGroupTopology have their
own subscriptions. `GetPositionInfo` is separately cached SOAP (`avtransport.cpp:95`).
The bridge main loop refreshes status after gEvent or roughly 30 seconds.

## Exact FLAC metadata and SOAP

Sources: `noson/noson/src/sonosplayer.cpp:554` (PlayStream), `:497` (SetCurrentURI),
`digitalitem.cpp:51` and `:166` (item and DIDL), `element.h:49` and `:99`
(serialization/escaping), `didlparser.cpp:31` (namespace order),
`avtransport.cpp:250` (arguments), `service.cpp:67` (SOAP envelope).

The FLAC path retains the HTTP URL. The item has **no id, parentID or restricted
attributes**. Class precedes title; empty streamContent is an explicit open/close
tag. Artwork is omitted only when empty. Namespace order and the space before
DIDL's closing `>` matter for the golden comparison. Noson escapes `& < > "` but
leaves apostrophes unchanged (safe in text and double-quoted attributes).
Non-FLAC file mode uses x-rincon-mp3radio URI/protocol as noson does.

The fixtures use URL `http://bridge:1400/music/squeezebox.flac?session=0123456789abcdef&stream=7`,
title `A & B <Live> "Mix" '26`, and artwork `http://lms:9000/art?a=1&b=2`.
`tests/noson_golden.cpp` runs the actual noson DigitalItem and AVTransport client;
the SOAP fixture was captured by a loopback HTTP speaker, not inferred.

DIDL-Lite (one line):

```xml
<DIDL-Lite xmlns="urn:schemas-upnp-org:metadata-1-0/DIDL-Lite/" xmlns:r="urn:schemas-rinconnetworks-com:metadata-1-0/" xmlns:dc="http://purl.org/dc/elements/1.1/" xmlns:upnp="urn:schemas-upnp-org:metadata-1-0/upnp/" ><item><upnp:class>object.item.audioItem</upnp:class><dc:title>A &amp; B &lt;Live&gt; &quot;Mix&quot; '26</dc:title><r:streamContent></r:streamContent><upnp:albumArtURI>http://lms:9000/art?a=1&amp;b=2</upnp:albumArtURI><res protocolInfo="x-rincon-mp3radio:*:audio/flac:*">http://bridge:1400/music/squeezebox.flac?session=0123456789abcdef&amp;stream=7</res></item></DIDL-Lite>
```

HTTP/1.1 POST `/MediaRenderer/AVTransport/Control`, SOAPAction
`"urn:schemas-upnp-org:service:AVTransport:1#SetAVTransportURI"`, Content-Type
`text/xml`. Exact body (no trailing newline on the wire):

```xml
<?xml version="1.0" encoding="utf-8"?><s:Envelope xmlns:s="http://schemas.xmlsoap.org/soap/envelope/" s:encodingStyle="http://schemas.xmlsoap.org/soap/encoding/"><s:Body><u:SetAVTransportURI xmlns:u="urn:schemas-upnp-org:service:AVTransport:1"><InstanceID>0</InstanceID><CurrentURI>http://bridge:1400/music/squeezebox.flac?session=0123456789abcdef&amp;stream=7</CurrentURI><CurrentURIMetaData>&lt;DIDL-Lite xmlns=&quot;urn:schemas-upnp-org:metadata-1-0/DIDL-Lite/&quot; xmlns:r=&quot;urn:schemas-rinconnetworks-com:metadata-1-0/&quot; xmlns:dc=&quot;http://purl.org/dc/elements/1.1/&quot; xmlns:upnp=&quot;urn:schemas-upnp-org:metadata-1-0/upnp/&quot; &gt;&lt;item&gt;&lt;upnp:class&gt;object.item.audioItem&lt;/upnp:class&gt;&lt;dc:title&gt;A &amp;amp; B &amp;lt;Live&amp;gt; &amp;quot;Mix&amp;quot; '26&lt;/dc:title&gt;&lt;r:streamContent&gt;&lt;/r:streamContent&gt;&lt;upnp:albumArtURI&gt;http://lms:9000/art?a=1&amp;amp;b=2&lt;/upnp:albumArtURI&gt;&lt;res protocolInfo=&quot;x-rincon-mp3radio:*:audio/flac:*&quot;&gt;http://bridge:1400/music/squeezebox.flac?session=0123456789abcdef&amp;amp;stream=7&lt;/res&gt;&lt;/item&gt;&lt;/DIDL-Lite&gt;</CurrentURIMetaData></u:SetAVTransportURI></s:Body></s:Envelope>
```

`upnp/encoded_buffer.*` wraps the remaining noson packet queue and endian helpers;
SBEncoder itself now lives in the bridge namespace. Its FLAC settings and queue
semantics are unchanged. AudioFormat's fixed PCM values are applied directly.
The interfaces return value types and express failure separately from an empty URI
or a zero position. `currentUri()` explicitly calls GetMediaInfo/CurrentURI, not
GetPositionInfo/TrackURI. It is available for later ownership work and does not
change ownership policy in this phase.

Display volume is a separate interface call because noson GetVolume performs SOAP.
It must never be called by the cached transport read on an HTTP worker.


## Own backend (experimental)

`SONOS_LMS_UPNP` is read and logged once at startup. Unset means noson; invalid
values warn and select noson. `OwnSpeakerControl` has no noson includes or calls.
It sends SSDP M-SEARCH to 239.255.255.250:1900 with the ZonePlayer:1 ST, waits
three seconds and retries once if no valid replies arrive. `--ip` bypasses SSDP.
It obtains ZoneGroupState via `/ZoneGroupTopology/Control`, then matches an exact,
case-sensitive individual room name (spaces preserved), or noson's combined group
name. Invisible bonded members are excluded. Ambiguous names fail discovery.

The selected physical room's address remains the transport target. A group change
only updates/logs the topology; it does not redirect commands to a new coordinator,
regroup speakers, or alter LMS sync. Topology is checked every five seconds from
the status loop; unavailable/malformed topology keeps the last good snapshot.
`controllerUri()` combines getsockname on the socket connected to the selected
speaker with the actual port of NosonStreamServer, including listener port fallback.

Own transport state is polled by Status::update on the existing main status loop
at a 500 ms interval. The resulting snapshot feeds the same refreshStatus,
ObserveDeviceTransport and ResumeSqueezeBox functions as GENA in noson mode.
HTTP workers only read the snapshot; they never poll or hold a SOAP I/O lock.
The usual detection delay is up to 500 ms plus network/loop work, unlike immediate
GENA notifications. A slow control, position or topology call can extend the delay.
Each SOAP exchange has one deadline including connect/send/read:

| Action | Deadline |
| --- | --- |
| Play, SetAVTransportURI, Stop, Pause | 20 seconds |
| GetTransportInfo, GetPositionInfo, GetMediaInfo, GetVolume, GetZoneGroupState and other reads | 5 seconds |

Noson's socket timeout is 5 seconds with three read attempts (up to about 15 seconds
without incoming data): `noson/noson/src/private/socket.h:33–35`,
`socket.cpp:103`, `socket.cpp:428–465`. Its timeout is per socket wait, not a single
HTTP exchange deadline. The 20-second transport deadline allows the Sonos standby
GET probe to finish before acknowledging Play.

Position uses a one-second cache. Failed polls mark transport unavailable instead
of inventing STOPPED or replaying a stale transition. Own polling fills title from
TrackMetaData (falling back to the sent DIDL title when empty or equal to the
stream URL or its basename, with or without the query),
track duration from GetPositionInfo, and volume from RenderingControl GetVolume
(InstanceID 0, Channel Master, `/MediaRenderer/RenderingControl/Control`). Volume
is cached for at least one second; display reads never send SOAP.
While the last transport state is STOPPED/PAUSED_PLAYBACK and no response is
streaming, position reads retain their last value instead of sending SOAP.
Transport polls continue; a resume or new stream permits fresh position reads.
A position timeout overlapping an open request while paused emits one informational
message per pause. The bridge supplies request activity through a read-only callback.

Before retrying PlayStream, both backends freshly query GetTransportInfo and
GetMediaInfo. PLAYING/TRANSITIONING on the exact current session-and-stream URL
completes the start without sending SetAVTransportURI/Play again. Unknown state,
failed reads, STOPPED, or another URL retain the bounded three-attempt retry.

In own mode, EPIPE/ECONNRESET classification waits up to two seconds on a separate
logging worker. A pause/stop observation, response end or standby promotion for
the same stream within two seconds before or after the error makes it a normal
client close. Otherwise the original send-failure diagnostic is emitted, exactly
once. Socket cleanup never waits for classification. Noson keeps its existing
immediate classification and output.

All seven AVTransport actions use `/MediaRenderer/AVTransport/Control`, InstanceID
0 and the service namespace `urn:schemas-upnp-org:service:AVTransport:1`.
GetMediaInfo returns CurrentURI independently of TrackURI. PlayStream issues Play
only after SetAVTransportURI succeeds. UPnP faults log action, errorCode and
errorDescription, including HTTP 500 responses.

The reusable XML reader supports local-name tag lookup, quoted attributes, the
five predefined entities, numeric Unicode references, comments and CDATA. It
rejects malformed trees, DTDs/external entities, excessive depth and inputs over
2 MiB. It is a minimal UPnP reader, not a schema validator. The HTTP client accepts
numeric IPv4 endpoints and handles Content-Length, chunked and close-delimited
responses under a single deadline and size bound; it performs no DNS or redirects.
These helpers contain no bridge state and implement no SMAPI functionality.

`make test` includes XML/SSDP/topology tests and a loopback mock HTTP speaker.
The mock captures all seven requests from the actual linked noson AVTransport
client and compares own requests byte-for-byte (including the legacy Speed=1 on
Pause/Stop). It also verifies the committed SetAVTransportURI fixture, CurrentURI,
SOAP faults, chunked/truncated responses, deadlines, cached nonblocking reads,
group-change logging, and real own polling feeding the extracted production
ObserveDeviceTransport/ResumeSqueezeBox code. Physical S1–S7 testing remains a
separate release gate, performed by the user; no deployment is part of these tests.


Noson startup also has implicit wire traffic behind System::Discover:
`sonossystem.cpp:105` subscribes to `/ZoneGroupTopology/Event`, waits up to 3 s for
NOTIFY and falls back to GetZoneGroupState at `/ZoneGroupTopology/Control`.
It calls GetHouseholdID and GetZoneInfo at `/DeviceProperties/Control`, loads
music services via ListAvailableServices at `/MusicServices/Control`, and subscribes
to `/AlarmClock/Event` and `/MediaServer/ContentDirectory/Event`. Player::Init
(`sonosplayer.cpp:114`) opens a TCP connection to select the local interface,
subscribes each group member's RenderingControl plus coordinator AVTransport and
ContentDirectory. System's event handler owns subscription renewal and dispatch.
Those noson-side ancillary services remain in default mode; own mode needs only
topology and AVTransport and does not initialize them.
