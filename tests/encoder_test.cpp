#include "sbencoder.h"
#include <atomic>
#include <cassert>
#include <chrono>
#include <cstring>
#include <future>
#include <iostream>
#include <vector>
#include <FLAC++/decoder.h>
extern "C" void pack_audio_test(void*, int32_t*, unsigned, unsigned);

static std::atomic<unsigned> generation(1);
static std::atomic<bool> paused(false);
static std::atomic<uint64_t> fakeClock(0);
extern "C" unsigned get_squeezebox_stream_id() { return generation.load(); }
extern "C" int sonos_lms_is_paused() { return paused.load(); }
extern "C" uint64_t get_sb_time_ms()
{
    if (fakeClock.load()) return fakeClock.load();
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}

namespace bridge {
struct EncoderTestAccess {
    static void seed(SBEncoder& encoder, uint64_t bytes) {
        encoder.m_pcmBytesAccepted = bytes;
        encoder.m_firstReadAtMs = 1;
    }
    static std::vector<unsigned char> finish(SBEncoder& encoder) {
        encoder.close();
        std::vector<unsigned char> bytes;
        char buffer[4096];
        int n;
        while ((n = encoder.drainEncodedBytes(buffer, sizeof(buffer))) > 0)
            bytes.insert(bytes.end(), buffer, buffer + n);
        return bytes;
    }
    static uint64_t bytes(const SBEncoder& encoder) { return encoder.m_pcmBytesAccepted; }
};
}


class Decoder : public FLAC::Decoder::Stream {
public:
    std::vector<unsigned char> data;
    std::vector<int32_t> samples;
    size_t offset = 0;
    unsigned rate = 0, bits = 0;
    FLAC__StreamDecoderReadStatus read_callback(FLAC__byte* dst, size_t* n) override {
        *n = std::min(*n, data.size() - offset);
        if (!*n) return FLAC__STREAM_DECODER_READ_STATUS_END_OF_STREAM;
        memcpy(dst, data.data() + offset, *n); offset += *n;
        return FLAC__STREAM_DECODER_READ_STATUS_CONTINUE;
    }
    FLAC__StreamDecoderWriteStatus write_callback(const FLAC__Frame* frame,
                                                  const FLAC__int32* const pcm[]) override {
        rate = frame->header.sample_rate; bits = frame->header.bits_per_sample;
        for (unsigned i = 0; i < frame->header.blocksize; ++i)
            for (unsigned ch = 0; ch < 2; ++ch) samples.push_back(pcm[ch][i]);
        return FLAC__STREAM_DECODER_WRITE_STATUS_CONTINUE;
    }
    void error_callback(FLAC__StreamDecoderErrorStatus) override { assert(false); }
};
static void audioQualityTests() {
    for (unsigned rate : {44100u, 48000u}) for (unsigned bits : {16u, 24u}) {
        std::vector<int32_t> input, expected;
        // Full 16-bit range, negative values and zero; left-aligned internal PCM.
        for (int v = -32768; v <= 32767; ++v) {
            input.push_back(v * 65536);
            expected.push_back(v * (bits == 24 ? 256 : 1));
        }
        std::vector<char> packed(input.size() * bits / 8);
        pack_audio_test(packed.data(), input.data(), input.size()/2, bits);
        bridge::SBEncoder encoder(1);
        assert(encoder.open(bits, rate));
        assert(encoder.write(packed.data(), packed.size(), 10) == int(packed.size()));
        Decoder decoder;
        decoder.data = bridge::EncoderTestAccess::finish(encoder);
        assert(decoder.init() == FLAC__STREAM_DECODER_INIT_STATUS_OK);
        assert(decoder.process_until_end_of_stream());
        assert(decoder.samples == expected && decoder.rate == rate && decoder.bits == bits);
        decoder.finish();
        std::cout << "PASS: decoded " << bits << "-bit FLAC " << rate
                  << " Hz preserves every 16-bit sample (24-bit padding only)\n";
        if (bits == 24) {
            // Every low bit survives too; not just padded 16-bit content.
            for (size_t i = 0; i < input.size(); ++i) {
                expected[i] = int(i * 7919 % 16777216) - 8388608;
                input[i] = expected[i] * 256;
            }
            pack_audio_test(packed.data(), input.data(), input.size()/2, bits);
            bridge::SBEncoder native(1);
            assert(native.open(bits, rate));
            assert(native.write(packed.data(), packed.size(), 10) == int(packed.size()));
            Decoder full;
            full.data = bridge::EncoderTestAccess::finish(native);
            assert(full.init() == FLAC__STREAM_DECODER_INIT_STATUS_OK);
            assert(full.process_until_end_of_stream() && full.samples == expected);
            full.finish();
            std::cout << "PASS: native 24-bit precision preserved at " << rate << " Hz\n";
        }
        const uint64_t boundary = uint64_t(1) << 32;
        bridge::SBEncoder paced(1);
        assert(paced.open(bits, rate));
        const unsigned frameBytes = bits / 8 * 2;
        const uint64_t below = boundary / frameBytes * frameBytes;
        bridge::EncoderTestAccess::seed(paced, below);
        fakeClock = below / frameBytes * 1000 / rate + 1;
        unsigned crossingAnchors = 0;
        assert(paced.write(packed.data(), frameBytes, 10, [&]{++crossingAnchors;}) == int(frameBytes));
        assert(bridge::EncoderTestAccess::bytes(paced) > boundary && crossingAnchors == 0);
        const uint64_t bytes = (boundary + 2000) * rate / 1000 * frameBytes;
        bridge::EncoderTestAccess::seed(paced, bytes);
        const uint64_t elapsed = bytes / frameBytes * 1000 / rate;
        fakeClock = elapsed - 1000 + 1;
        assert(paced.write(packed.data(), frameBytes, 5) == 0);
        fakeClock = elapsed + 1;
        unsigned anchors = 0;
        assert(paced.write(packed.data(), frameBytes, 10, [&]{++anchors;}) == int(frameBytes));
        assert(anchors == 0 && bridge::EncoderTestAccess::bytes(paced) == bytes + frameBytes);
        fakeClock = 0;
        std::cout << "PASS: " << bits << "-bit " << rate << " Hz pacing beyond 2^32 bytes and 49 days\n";
    }
}
static void counterAndShutdownTests() {
    bridge::SBEncoder encoder(1);
    assert(encoder.open());
    char pcm[4] = {};
    const uint64_t boundary = uint64_t(1) << 32;
    bridge::EncoderTestAccess::seed(encoder, boundary - 4);
    fakeClock = boundary / 4 * 1000 / 44100 + 1;
    unsigned anchors = 0;
    auto anchor = [&] { ++anchors; };
    assert(encoder.write(pcm, 4, 10, anchor) == 4);
    assert(bridge::EncoderTestAccess::bytes(encoder) == boundary);
    assert(encoder.write(pcm, 4, 10, anchor) == 4);
    assert(bridge::EncoderTestAccess::bytes(encoder) == boundary + 4 && anchors == 0);
    // Pacing time must also remain 64-bit, beyond the old 49-day ms wrap.
    const uint64_t bytes = (boundary + 2000) * 44100 / 1000 * 4;
    const uint64_t elapsed = bytes / 4 * 1000 / 44100;
    bridge::EncoderTestAccess::seed(encoder, bytes);
    fakeClock = elapsed - 1000 + 1;
    assert(encoder.write(pcm, 4, 5, anchor) == 0); // still a second ahead
    fakeClock = elapsed + 1;
    assert(encoder.write(pcm, 4, 10, anchor) == 4 && anchors == 0);
    fakeClock = 0;
    std::cout << "PASS: PCM counter crosses 2^32 without re-anchoring; pacing remains 64-bit beyond 49 days\n";

    bridge::SBEncoder waiting(1);
    assert(waiting.open());
    paused = true;
    std::atomic<bool> stopping(false);
    auto writer = std::async(std::launch::async, [&] {
        return waiting.write(pcm, 4, 0, {}, [&] { return stopping.load(); });
    });
    assert(writer.wait_for(std::chrono::milliseconds(30)) == std::future_status::timeout);
    stopping = true;
    assert(writer.wait_for(std::chrono::seconds(1)) == std::future_status::ready);
    assert(writer.get() == 0);
    paused = false;
    std::cout << "PASS: shutdown interrupts an encoder write held indefinitely by pause\n";
}

int main()
{
    audioQualityTests();
    counterAndShutdownTests();
    bridge::SBEncoder old(1);
    char data[16384];
    assert(old.open());
    assert(old.read(data, 4, 10) == 4);
    assert(std::memcmp(data, "fLaC", 4) == 0);
    // Drain metadata: an idle, unpaused encoder must actually time out.
    auto drain = std::async(std::launch::async, [&] {
        while (old.read(data, sizeof(data), 5) > 0) {}
    });
    assert(drain.wait_for(std::chrono::seconds(1)) == std::future_status::ready);
    drain.get();

    paused = true;
    auto held = std::async(std::launch::async, [&] { return old.read(data, sizeof(data), 5); });
    assert(held.wait_for(std::chrono::milliseconds(30)) == std::future_status::timeout);
    paused = false;
    assert(held.wait_for(std::chrono::seconds(1)) == std::future_status::ready);
    assert(held.get() == 0);

    // Invalidating the generation ends the old reader without closing an encoder
    // from another thread. The new generation starts with its own FLAC marker.
    generation = 2;
    assert(old.read(data, sizeof(data), 5) == 0);
    bridge::SBEncoder fresh(2);
    assert(fresh.open());
    assert(fresh.read(data, 4, 10) == 4);
    assert(std::memcmp(data, "fLaC", 4) == 0);
    old.close();
    assert(fresh.read(data, sizeof(data), 10) > 0);
    std::vector<char> pcm(44100 * 4 * 3, 0);
    assert(fresh.write(pcm.data(), pcm.size(), 10) == (int)pcm.size());
    auto full = std::async(std::launch::async, [&] { return fresh.write(pcm.data(), 4, 5); });
    assert(full.wait_for(std::chrono::seconds(1)) == std::future_status::ready);
    assert(full.get() == 0);
    paused = true;
    auto waitingWriter = std::async(std::launch::async, [&] { return fresh.write(pcm.data(), 4, 5); });
    assert(waitingWriter.wait_for(std::chrono::milliseconds(30)) == std::future_status::timeout);
    fresh.cancel();
    assert(waitingWriter.wait_for(std::chrono::seconds(1)) == std::future_status::ready);
    assert(waitingWriter.get() == 0);
    bridge::SBEncoder replacement(2);
    assert(replacement.open());
    assert(replacement.read(data, 4, 10, false) == 4);
    assert(std::memcmp(data, "fLaC", 4) == 0);
    // Ending the HTTP response is distinct from cancelling/closing FLAC.
    replacement.retireProducer();
    assert(!replacement.cancelled() && !replacement.responseEnded());
    assert(replacement.read(data, sizeof(data), 100, false) > 0); // header survives handoff
    assert(replacement.write(pcm.data(), 4, 100) == 0);
    replacement.endResponse();
    assert(replacement.responseEnded() && !replacement.cancelled());
    assert(replacement.read(data, sizeof(data), 100, false) == 0);
    assert(replacement.write(pcm.data(), 4, 100) == 0);
    paused = false;
    std::cout << "PASS: FLAC headers, timeouts, pause hold, cancellation, response EOF, same-ID replacement\n";
}
