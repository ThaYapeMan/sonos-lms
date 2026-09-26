// sbencoder.h -- streams decoded PCM into a FLAC-encoded byte stream for Sonos
//
// Copyright (c) 2026 Jaap van Vliet
//
// Original implementation for the sonos-lms project. Licensed under
// the GNU General Public License, version 3 or (at your option) any later
// version, matching the rest of this project. See LICENSE.
//
// THIS SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.

#ifndef FLACENCODER_H
#define FLACENCODER_H

#include "upnp/encoded_buffer.h"

#include <atomic>
#include <functional>
#include <mutex>

#include <FLAC++/encoder.h>
#include <FLAC++/metadata.h>

namespace bridge {


// Bridges squeezelite's decoded PCM (pushed via write()) to the HTTP
// streamer's FLAC output (pulled via read()), pacing writes against actual
// Sonos playback progress and pausing cleanly instead of buffering ahead.
class SBEncoder {
    friend class WriteBridge;
#ifdef SBENCODER_TEST
    friend struct EncoderTestAccess;
#endif

public:
    explicit SBEncoder(unsigned streamId);
    ~SBEncoder();

    bool open();
    bool open(uint8_t sampleBits, unsigned sampleRate = 44100);

    // Blocks (up to timeout ms, 0 = forever) until len bytes of PCM have
    // been accepted into the encoder, or the stream ends/is cancelled.
    int write(const char* data, int len, unsigned timeout,
        const std::function<void()>& firstPcm = {},
        const std::function<bool()>& interrupted = {});

    // Blocks (up to timeout ms, 0 = forever) until some encoded FLAC bytes
    // are available, or the stream ends/is cancelled. While holdWhilePaused
    // is set, a paused LMS source never lets this call time out — the
    // deadline is pushed forward on every poll until playback resumes.
    // peerClosed, if given, is polled each iteration to detect a client
    // that has already disconnected.
    int read(char* data, int maxlen, unsigned timeout, bool holdWhilePaused = true,
        const std::function<bool()>& peerClosed = {});

    void close();

    // A GET whose producer has been superseded by a newer generation but
    // whose reader must still be allowed to drain: stop accepting writes,
    // never touch the reader side.
    void retireProducer() { m_producerRetired.store(true); }
    bool producerRetired() const { return m_producerRetired.load(); }

    // The HTTP response itself has ended (client gone) — distinct from
    // retireProducer(): this also unblocks read().
    void endResponse() { m_responseEnded.store(true); }
    bool responseEnded() const { return m_responseEnded.load(); }

    // Hard stop for both sides at once (e.g. an outdated generation).
    void cancel() { m_cancelled.store(true); }
    bool cancelled() const { return m_cancelled.load(); }

    // True once at least one non-empty FLAC frame has actually been written.
    bool hasAudio() const { return m_producedAudio.load(); }

    unsigned streamId() const { return m_streamId; }

private:
    enum class Phase {
        Init,
        Encoding,
        Closing,
        Closed,
    };

    int encodePcm(const char* data, int len);
    int pendingEncodedBytes() const;
    int acceptEncodedBytes(const char* data, int len);
    int drainEncodedBytes(char* data, int maxlen);

    std::atomic<Phase> m_phase;
    std::atomic<bool> m_cancelled{false};
    std::atomic<bool> m_responseEnded{false};
    std::atomic<bool> m_producerRetired{false};
    std::atomic<bool> m_producedAudio{false};
    std::mutex m_writeMutex;
    std::atomic<uint64_t> m_firstReadAtMs;  // set on the first successful read()
    uint64_t m_pcmBytesAccepted;            // running total handed to the encoder
    int m_bytesPerFrame;
    int m_sampleBits;
    unsigned m_sampleRate = 44100;
    unsigned m_streamId;
    FLAC__int32* m_interleaveBuf;

    upnp::EncodedBuffer* m_encodedRing;
    upnp::EncodedBuffer::Packet* m_pendingPacket;
    int m_pendingPacketConsumed;

    class WriteBridge : public FLAC::Encoder::Stream {
    public:
        explicit WriteBridge(SBEncoder* owner)
            : m_owner(owner)
        {
        }
        FLAC__StreamEncoderWriteStatus write_callback(const FLAC__byte buffer[], size_t bytes,
            unsigned samples, unsigned current_frame) override;

    private:
        SBEncoder* m_owner;
    };

    WriteBridge* m_flac;
};

}  // namespace bridge
#endif  // FLACENCODER_H
