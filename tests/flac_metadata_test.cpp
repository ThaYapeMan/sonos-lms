#include "flac_metadata.h"
#include "sbencoder.h"
#include <cassert>
#include <chrono>
#include <cstdio>
#include <vector>

extern "C" unsigned get_squeezebox_stream_id() { return 1; }
extern "C" int sonos_lms_is_paused() { return 0; }
extern "C" uint32_t get_sb_time_ms() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}

static std::vector<char> strip(const std::vector<char>& input, size_t chunk, size_t expected) {
    FlacMetadataSkipper skip;
    std::vector<char> audio;
    for (size_t pos = 0; pos < input.size();) {
        size_t count = std::min(chunk, input.size() - pos);
        size_t removed = skip.consume(input.data() + pos, count);
        audio.insert(audio.end(), input.begin() + pos + removed, input.begin() + pos + count);
        pos += count;
    }
    assert(skip.valid() && skip.done() && skip.skipped() == expected);
    return audio;
}

int main() {
    SONOS::SBEncoder encoder(1);
    assert(encoder.open());
    std::vector<int16_t> pcm(8192 * 2);
    for (size_t n = 0; n < pcm.size(); ++n) pcm[n] = (n * 7919) % 30000;
    assert(encoder.write(reinterpret_cast<const char*>(pcm.data()), pcm.size() * 2, 100) == int(pcm.size() * 2));
    std::vector<char> output;
    FlacMetadataSkipper skip;
    size_t audioBytes = 0;
    while (audioBytes < 2) {
        char buf[16384];
        int count = encoder.read(buf, sizeof(buf), 100);
        assert(count > 0);
        output.insert(output.end(), buf, buf + count);
        audioBytes += count - skip.consume(buf, count);
    }
    assert(skip.valid() && skip.done() && skip.skipped() > 8);
    size_t metadataBytes = skip.skipped();
    assert(static_cast<unsigned char>(output[metadataBytes]) == 0xff);
    assert((static_cast<unsigned char>(output[metadataBytes + 1]) & 0xfe) == 0xf8);
    std::vector<char> frames(output.begin() + metadataBytes, output.end());
    for (size_t chunk : {1u, 2u, 3u, 4u, 7u, 16384u})
        assert(strip(output, chunk, metadataBytes) == frames);
    printf("PASS: real encoder metadata (%zu bytes) skipped across arbitrary read boundaries; body starts at FLAC frame sync\n", metadataBytes);

    // Exercise all 24 length bits, several blocks, and a zero-length last block.
    std::vector<char> synthetic{'f', 'L', 'a', 'C', 1, 1, 2, 3};
    synthetic.resize(synthetic.size() + 0x010203, 'x');
    synthetic.insert(synthetic.end(), {char(0x81), 0, 0, 0});
    size_t prefix = synthetic.size();
    synthetic.insert(synthetic.end(), {char(0xff), char(0xf9), 1, 2});
    assert(strip(synthetic, 3, prefix) == std::vector<char>({char(0xff), char(0xf9), 1, 2}));
    FlacMetadataSkipper truncated;
    truncated.consume(synthetic.data(), prefix - 1);
    assert(truncated.valid() && !truncated.done());
    FlacMetadataSkipper invalid;
    invalid.consume("bad!", 4);
    assert(!invalid.valid() && !invalid.done());
    puts("PASS: metadata parser respects 24-bit lengths, last-block flag, empty blocks, and incomplete/invalid input");
}
