#include "sbstreamer.h"
#include "resume_state.h"
#include "private/socket.h"
#include "private/wsrequestbroker.h"
#include <FLAC++/decoder.h>
#include <atomic>
#include <cassert>
#include <chrono>
#include <cstring>
#include <future>
#include <iostream>
#include <mutex>
#include <thread>
#include <vector>

using namespace SONOS;
static std::atomic<unsigned> generation(1), resumeCommands(0), sameURLRequests(0);
static std::atomic<bool> paused(false);
static std::mutex stateMutex;
static ResumeState state;
static std::string deviceState = "PLAYING";
static unsigned resumeDelayMs = 0;
static bool pendingResume = false;
static std::chrono::steady_clock::time_point resumeAt;
extern "C" unsigned get_lms_stream_serial() { return generation.load(); }
extern "C" unsigned get_squeezebox_stream_id() { return generation.load(); }
extern "C" int sonos_lms_is_paused() { return paused.load(); }
extern "C" uint32_t get_sb_time_ms() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}
extern "C" void encode_squeezebox_audio(const char*, int);
extern "C" void end_squeezebox_response(void);
extern "C" int squeezebox_response_ended(unsigned stream);
extern "C" int squeezebox_response_open(unsigned stream);
extern "C" void acknowledge_squeezebox_resume(unsigned stream);
extern "C" void invalidate_squeezebox_held_get(unsigned stream);
std::string SqueezeBoxURL(unsigned id) { return "http://bridge/music/squeezebox.flac?stream=" + std::to_string(id); }
void ResumeSqueezeBox(unsigned id) {
    std::lock_guard<std::mutex> lock(stateMutex);
    state.observe(deviceState);
    if (state.takeResume(id, generation)) {
        ++resumeCommands;
        pendingResume = true;
        resumeAt = std::chrono::steady_clock::now() + std::chrono::milliseconds(resumeDelayMs);
    }
    if (pendingResume && std::chrono::steady_clock::now() >= resumeAt) {
        // Simulate LMS receiving CLI play and answering with strm u, not s.
        assert(state.command('u', squeezebox_response_open(id)) == ResumeState::Unpause::SameURL);
        invalidate_squeezebox_held_get(id);
        acknowledge_squeezebox_resume(id);
        pendingResume = false;
        paused = false;
        // Model PlayStream(same URL): the test's device opens a fresh GET.
        // The cancelled held request closes before the device reconnects.
        ++sameURLRequests;
    }
}

// Exercise the real HTTP broker with an in-memory noson socket. No device or
// network access: only the socket I/O and LMS/device event source are simulated.
class Socket : public TcpSocket {
public:
    explicit Socket(unsigned id, bool probe = false, bool stayOpen = false) : probe(probe), stayOpen(stayOpen) {
        input = "GET /music/squeezebox.flac?stream=" + std::to_string(id) + " HTTP/1.1\r\nHost: bridge\r\n\r\n";
    }
    size_t ReceiveData(void* buf, size_t n) override {
        n = std::min(n, input.size() - offset);
        memcpy(buf, input.data() + offset, n); offset += n; return n;
    }
    bool SendData(const char* data, size_t n) override {
        if (drop || clientClosed.load()) return false;
        if (n == 5 && memcmp(data, "0\r\n\r\n", 5) == 0) {
            assert(!disconnected); eof = true; return true;
        }
        if (!headersSent.exchange(true)) {
            headers.assign(data, n);
            if (probe) { drop = true; return false; }
            return true;
        }
        // sendChunk calls size, payload, CRLF separately. Close after the first
        // complete audio packet, keeping the captured FLAC frames decodable.
        if (phase == 1) {
            body.insert(body.end(), data, data + n);
            if (n > 2 && (unsigned char)data[0] == 0xff && ((unsigned char)data[1] & 0xfe) == 0xf8)
                audioSeen = true;
        }
        phase = (phase + 1) % 3;
        if (phase == 0 && audioSeen && !stayOpen) { drop = true; return false; }
        return true;
    }
    bool IsValid() const override { return !disconnected.load() && !clientClosed.load(); }
    void Disconnect() override { disconnected = true; }
    std::atomic<bool> clientClosed{false};
    std::atomic<bool> headersSent{false}, audioSeen{false}, eof{false}, disconnected{false};
    std::string headers;
    std::vector<char> body;
private:
    std::string input;
    size_t offset = 0;
    int phase = 0;
    bool probe, stayOpen, drop = false;
};

class Decoder : public FLAC::Decoder::Stream {
public:
    explicit Decoder(const std::vector<char>& bytes) : bytes(bytes) {}
    unsigned frames = 0;
    int first = 0;
    bool error = false;
    FLAC__StreamDecoderReadStatus read_callback(FLAC__byte* buffer, size_t* n) override {
        *n = std::min(*n, bytes.size() - offset);
        memcpy(buffer, bytes.data() + offset, *n); offset += *n;
        return *n ? FLAC__STREAM_DECODER_READ_STATUS_CONTINUE : FLAC__STREAM_DECODER_READ_STATUS_END_OF_STREAM;
    }
    FLAC__StreamDecoderWriteStatus write_callback(const FLAC__Frame*, const FLAC__int32* const samples[]) override {
        if (!frames++) first = samples[0][0];
        return FLAC__STREAM_DECODER_WRITE_STATUS_CONTINUE;
    }
    void error_callback(FLAC__StreamDecoderErrorStatus) override { error = true; }
private:
    const std::vector<char>& bytes;
    size_t offset = 0;
};

static void serve(SBStreamer& broker, Socket& socket) {
    WSRequestBroker request(&socket, /*secure=*/false, /*timeout ms=*/1000);
    assert(request.IsParsed());
    RequestBroker::handle handle{nullptr, &request};
    assert(broker.HandleRequest(&handle));
}
static void feed(int first) {
    // 8192 stereo frames produce real FLAC frames, not merely init metadata.
    std::vector<int16_t> pcm(8192 * 2);
    for (unsigned i = 0; i < pcm.size(); ++i) pcm[i] = (first + i * 7919) % 30000;
    encode_squeezebox_audio(reinterpret_cast<const char*>(pcm.data()), pcm.size() * 2);
}
static void playable(const Socket& socket, int first) {
    assert(socket.headers.find("200 OK") != std::string::npos);
    assert(socket.body.size() > 4 && memcmp(socket.body.data(), "fLaC", 4) == 0);
    Decoder decoder(socket.body);
    assert(decoder.init() == FLAC__STREAM_DECODER_INIT_STATUS_OK);
    assert(decoder.process_until_end_of_stream());
    assert(!decoder.error && decoder.frames && decoder.first == first);
    decoder.finish();
}
static void connection(SBStreamer& broker, Socket& socket, int first, bool held = false) {
    auto http = std::async(std::launch::async, [&] { serve(broker, socket); });
    std::this_thread::sleep_for(std::chrono::milliseconds(40));
    assert(socket.headersSent.load() == !held); // only a paused resume waits for PCM
    feed(first);
    assert(http.wait_for(std::chrono::seconds(2)) == std::future_status::ready);
    http.get();
}
// Simulate Sonos honoring the same-URL PlayStream by opening a fresh GET.
// Sonos waits for the speculative held request to close before reconnecting.
static void deviceReconnect(SBStreamer& broker, unsigned id, int first) {
    unsigned requestsBefore = sameURLRequests.load();
    unsigned commandsBefore = resumeCommands.load();
    Socket held(id, false, true);
    auto oldGet = std::async(std::launch::async, [&] { serve(broker, held); });
    std::this_thread::sleep_for(std::chrono::milliseconds(40));
    assert(!held.headersSent && squeezebox_response_open(id));
    {
        std::lock_guard<std::mutex> lock(stateMutex);
        deviceState = "TRANSITIONING";
    }
    auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(4800);
    while (sameURLRequests == requestsBefore && std::chrono::steady_clock::now() < deadline)
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    assert(sameURLRequests == requestsBefore + 1 && resumeCommands == commandsBefore + 1);
    assert(!paused);
    assert(oldGet.wait_for(std::chrono::milliseconds(300)) == std::future_status::ready);
    oldGet.get();
    assert(held.disconnected && !held.eof && held.body.empty());
    assert(held.headers.find("503 Service Unavailable") != std::string::npos);
    assert(!squeezebox_response_ended(id));
    Socket fresh(id);
    auto freshGet = std::async(std::launch::async, [&] { serve(broker, fresh); });
    std::this_thread::sleep_for(std::chrono::milliseconds(40));
    assert(fresh.headersSent);
    feed(first);
    assert(freshGet.wait_for(std::chrono::seconds(1)) == std::future_status::ready);
    freshGet.get();
    playable(fresh, first);
    assert(generation == id);
    assert(sameURLRequests == requestsBefore + 1 && resumeCommands == commandsBefore + 1);
}

int main() {
    setvbuf(stdout, nullptr, _IOLBF, 0);
    SBStreamer broker;
    state.command('s'); state.observe("PLAYING");
    Socket probe(1, true);
    serve(broker, probe); // the client closes its probe after HTTP headers
    Socket real(1);
    connection(broker, real, 200);
    playable(real, 200);
    assert(real.headers == "HTTP/1.1 200 OK\r\nServer: libnoson/" LIBVERSION "\r\nConnection: close\r\n"
        "Content-Type: audio/flac\r\nTransfer-Encoding: chunked\r\n\r\n");
    std::cout << "PASS: streaming response restores Server and Connection: close headers\n";
    assert(generation == 1 && resumeCommands == 0);
    std::cout << "PASS: probe and second GET keep the ID and decode fresh FLAC\n";

    state.command('p'); state.observe("PAUSED_PLAYBACK");
    state.command('u'); paused = false;
    Socket lmsResume(1);
    connection(broker, lmsResume, 300);
    playable(lmsResume, 300);
    assert(generation == 1 && resumeCommands == 0);
    std::cout << "PASS: LMS resume serves the same ID without CLI play or redirect\n";

    state.observe("PLAYING"); // explicit device event after LMS unpause
    assert(state.observe("PAUSED_PLAYBACK"));
    state.command('p'); paused = true; deviceState = "PAUSED_PLAYBACK";
    deviceReconnect(broker, 1, 400);
    assert(generation == 1 && resumeCommands == 1);
    std::cout << "PASS: device transition plus strm p sends one play; held GET closes promptly, fresh same-ID GET carries audio\n";

    Socket predecessor(1, false, true), replacement(1, false, true);
    auto oldGet = std::async(std::launch::async, [&] { serve(broker, predecessor); });
    std::this_thread::sleep_for(std::chrono::milliseconds(40));
    assert(predecessor.headersSent && !predecessor.disconnected && !predecessor.eof);
    auto newGet = std::async(std::launch::async, [&] { serve(broker, replacement); });
    std::this_thread::sleep_for(std::chrono::milliseconds(40));
    assert(replacement.headersSent && !predecessor.disconnected && !predecessor.eof);
    assert(squeezebox_response_open(1));
    feed(500);
    auto audioLimit = std::chrono::steady_clock::now() + std::chrono::seconds(1);
    while (!replacement.audioSeen && std::chrono::steady_clock::now() < audioLimit)
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    assert(replacement.audioSeen && !predecessor.audioSeen);
    assert(!predecessor.disconnected && !predecessor.eof);
    predecessor.clientClosed = true;
    assert(oldGet.wait_for(std::chrono::seconds(1)) == std::future_status::ready);
    oldGet.get();
    assert(!predecessor.eof); // no server EOF before the client's close
    assert(!replacement.disconnected && !replacement.eof);
    feed(501); // the newest connection continues receiving PCM
    std::this_thread::sleep_for(std::chrono::milliseconds(40));
    assert(!replacement.disconnected && !replacement.eof);
    replacement.clientClosed = true;
    assert(newGet.wait_for(std::chrono::seconds(1)) == std::future_status::ready);
    newGet.get();
    playable(replacement, 500);
    assert(generation == 1 && resumeCommands == 1);
    std::cout << "PASS: both GETs get headers; client closes the older; newest keeps streaming\n";

    Socket survivor(1, false, true), shortLived(1, false, true);
    auto survivingGet = std::async(std::launch::async, [&] { serve(broker, survivor); });
    std::this_thread::sleep_for(std::chrono::milliseconds(40));
    assert(survivor.headersSent);
    auto shortGet = std::async(std::launch::async, [&] { serve(broker, shortLived); });
    std::this_thread::sleep_for(std::chrono::milliseconds(40));
    assert(shortLived.headersSent && !survivor.disconnected);
    feed(510);
    std::this_thread::sleep_for(std::chrono::milliseconds(40));
    assert(shortLived.audioSeen && !survivor.audioSeen);
    shortLived.clientClosed = true;
    assert(shortGet.wait_for(std::chrono::seconds(1)) == std::future_status::ready);
    shortGet.get();
    // Keep feeding beyond the original four-second idle deadline.
    for (int n = 0; n < 10; ++n) {
        feed(520 + n);
        std::this_thread::sleep_for(std::chrono::milliseconds(450));
        assert(!survivor.disconnected && !survivor.eof);
    }
    assert(survivor.audioSeen);
    survivor.clientClosed = true;
    survivingGet.get();
    playable(survivor, 520);
    std::cout << "PASS: client closes newest GET; surviving first GET receives PCM beyond idle deadline\n";

    // An established response ends immediately after Pause, including a read
    // already waiting for PCM. Keep the generation across a real 30-second pause.
    Socket longPause(1, false, true);
    auto playing = std::async(std::launch::async, [&] { serve(broker, longPause); });
    std::this_thread::sleep_for(std::chrono::milliseconds(40));
    feed(600);
    auto audioDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (!longPause.audioSeen && std::chrono::steady_clock::now() < audioDeadline)
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    assert(longPause.audioSeen);
    {
        std::lock_guard<std::mutex> lock(stateMutex);
        state.command('p'); deviceState = "PAUSED_PLAYBACK"; paused = true;
    }
    end_squeezebox_response(); // called AFTER UPnP Pause in production
    assert(playing.wait_for(std::chrono::milliseconds(500)) == std::future_status::ready);
    playing.get();
    assert(longPause.eof && longPause.disconnected && generation == 1);
    assert(squeezebox_response_ended(1) && !squeezebox_response_ended(2));
    assert(!squeezebox_response_open(1));
    std::this_thread::sleep_for(std::chrono::seconds(30));
    state.command('u'); deviceState = "PLAYING"; paused = false;
    Socket afterLongPause(1);
    connection(broker, afterLongPause, 700);
    playable(afterLongPause, 700);
    assert(squeezebox_response_ended(1)); // GET must not consume the re-prime decision
    acknowledge_squeezebox_resume(1); // successful PlayStream or held-GET unpause clears it
    assert(!squeezebox_response_ended(1));
    assert(generation == 1 && resumeCommands == 1);
    std::cout << "PASS: Pause sends chunked EOF and closes within 0.5s; same-ID resume after 30s\n";

    // A playing connection with no more PCM must also end under five seconds.
    Socket idle(1, false, true);
    auto idleGet = std::async(std::launch::async, [&] { serve(broker, idle); });
    std::this_thread::sleep_for(std::chrono::milliseconds(40));
    feed(800);
    assert(idleGet.wait_for(std::chrono::seconds(5)) == std::future_status::ready);
    idleGet.get();
    assert(idle.eof && idle.disconnected);
    std::cout << "PASS: established response ends cleanly within five seconds without PCM\n";

    // A paused current GET with no device transition must time out, not infer
    // a resume or emit a header-only 200. Exercise the real five-second deadline.
    state.command('p'); deviceState = "PAUSED_PLAYBACK"; paused = true;
    Socket timeout(1);
    auto begin = std::chrono::steady_clock::now();
    serve(broker, timeout);
    auto elapsed = std::chrono::steady_clock::now() - begin;
    assert(elapsed >= std::chrono::seconds(5) && elapsed < std::chrono::milliseconds(5500));
    assert(timeout.headers.find("503 Service Unavailable") != std::string::npos);
    assert(timeout.body.empty() && generation == 1 && resumeCommands == 1);
    std::cout << "PASS: paused GET without PCM receives HTTP 503 after five seconds\n";

    generation = 2;
    Socket older(1);
    serve(broker, older);
    assert(older.headers.find("302 Found") != std::string::npos);
    assert(older.headers.find("Location: " + SqueezeBoxURL(2)) != std::string::npos);
    assert(generation == 2 && resumeCommands == 1);
    std::cout << "PASS: older ID redirects directly to current without changing generation\n";

    // Reproduce J with the pause-ended flag still set and a real held GET.
    // Include a delayed CLI response beyond the old four-second timeout.
    for (unsigned delay : {0u, 4300u}) {
        {
            std::lock_guard<std::mutex> lock(stateMutex);
            state.command('u'); state.observe("PLAYING");
            state.observe("PAUSED_PLAYBACK"); state.command('p');
            paused = true; deviceState = "PAUSED_PLAYBACK";
            resumeDelayMs = delay;
        }
        end_squeezebox_response();
        assert(squeezebox_response_ended(2));
        deviceReconnect(broker, 2, 900 + delay);
        assert(generation == 2);
    }
    assert(resumeCommands == 3);
    std::cout << "PASS: ended-response device resumes reconnect with immediate and 4.3s LMS replies; held GET closes promptly and fresh GET decodes\n";

    // A held speculative GET must close promptly, before Sonos opens a new one.
    {
        std::lock_guard<std::mutex> lock(stateMutex);
        state.command('u'); state.observe("PLAYING");
        state.command('p'); state.observe("PAUSED_PLAYBACK");
        deviceState = "PAUSED_PLAYBACK"; paused = true;
    }
    Socket invalidated(2);
    auto heldGet = std::async(std::launch::async, [&] { serve(broker, invalidated); });
    auto openDeadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(300);
    while (!squeezebox_response_open(2) && std::chrono::steady_clock::now() < openDeadline)
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    assert(squeezebox_response_open(2) && !invalidated.headersSent);
    invalidate_squeezebox_held_get(1); // wrong ID must not cancel this request
    assert(squeezebox_response_open(2));
    auto invalidationStart = std::chrono::steady_clock::now();
    invalidate_squeezebox_held_get(2);
    assert(heldGet.wait_for(std::chrono::milliseconds(300)) == std::future_status::ready);
    heldGet.get();
    assert(std::chrono::steady_clock::now() - invalidationStart < std::chrono::milliseconds(300));
    assert(invalidated.disconnected && invalidated.body.empty() && !invalidated.eof);
    assert(invalidated.headers.find("503 Service Unavailable") != std::string::npos);
    assert(!squeezebox_response_open(2) && generation == 2 && resumeCommands == 3);
    std::cout << "PASS: matching held GET invalidates with HTTP 503 and closes within 300ms; wrong ID is ignored\n";

    state.command('u'); deviceState = "PLAYING"; paused = false;
    Socket afterInvalidation(2, false, true);
    auto freshGet = std::async(std::launch::async, [&] { serve(broker, afterInvalidation); });
    std::this_thread::sleep_for(std::chrono::milliseconds(40));
    assert(afterInvalidation.headersSent);
    feed(1200);
    auto audioReady = std::chrono::steady_clock::now() + std::chrono::seconds(1);
    while (!afterInvalidation.audioSeen && std::chrono::steady_clock::now() < audioReady)
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    assert(afterInvalidation.audioSeen);
    invalidate_squeezebox_held_get(2); // audio-producing encoder must survive
    assert(squeezebox_response_open(2));
    assert(freshGet.wait_for(std::chrono::milliseconds(20)) == std::future_status::timeout);
    assert(!afterInvalidation.disconnected && !afterInvalidation.eof);
    end_squeezebox_response();
    assert(freshGet.wait_for(std::chrono::milliseconds(300)) == std::future_status::ready);
    freshGet.get();
    playable(afterInvalidation, 1200);
    assert(generation == 2 && afterInvalidation.eof && afterInvalidation.disconnected);
    std::cout << "PASS: fresh same-ID GET decodes after invalidation; active audio is never invalidated\n";

}
