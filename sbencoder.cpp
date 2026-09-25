// sbencoder.cpp -- streams decoded PCM into a FLAC-encoded byte stream for Sonos
//
// Copyright (c) 2026 Jaap van Vliet
//
// Original implementation for the sonos-squeezebox project. Licensed under
// the GNU General Public License, version 3 or (at your option) any later
// version, matching the rest of this project. See LICENSE.
//
// THIS SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.

#include "sbencoder.h"

#include <chrono>
#include <cstdio>
#include <cstring>
#include <unistd.h>

#include "private/ringbuffer.h"
#include "private/byteorder.h"

namespace {
// One squeezelite decode batch's worth of interleaved stereo samples.
constexpr int kSamplesPerChunk = 1024;
constexpr int kEncodedRingCapacity = 256;
constexpr int kChannelCount = 2;
constexpr uint32_t kSampleRateHz = 44100;

// How far, in milliseconds, encoded-but-unsent audio is allowed to run ahead
// of what Sonos has actually played before write() blocks the decoder. Kept
// short so a stream resume after a Sonos-side reconnect only has to discard
// a fraction of a second of audio, not several seconds of stale buffer.
constexpr uint32_t kMaxEncodeLeadMs = 250;

// Unpacks one little-endian PCM sample at `cursor` (advancing it) into the
// FLAC__int32 form libFLAC's interleaved encoder expects.
FLAC__int32 nextSampleAsInt32(const char*& cursor, int bitDepth)
{
    switch (bitDepth) {
    case 8: {
        FLAC__int32 v = (unsigned char)(*cursor) - 128;
        cursor += 1;
        return v;
    }
    case 16: {
        FLAC__int32 v = read_b16le(cursor);
        cursor += 2;
        return v;
    }
    case 24: {
        FLAC__int32 v = read_b24le(cursor);
        cursor += 3;
        return v;
    }
    case 32: {
        FLAC__int32 v = read_b32le(cursor);
        cursor += 4;
        return v;
    }
    default:
        return 0;
    }
}
}  // namespace

extern "C" {
uint32_t get_sb_time_ms(void);
unsigned get_squeezebox_stream_id(void);
int sonos_lms_is_paused(void);
}  // extern "C"

using namespace NSROOT;

SBEncoder::SBEncoder(unsigned streamId)
    : m_phase(Phase::Init)
    , m_firstReadAtMs(0)
    , m_pcmBytesAccepted(0)
    , m_bytesPerFrame(0)
    , m_sampleBits(0)
    , m_streamId(streamId)
    , m_interleaveBuf(nullptr)
    , m_encodedRing(nullptr)
    , m_pendingPacket(nullptr)
    , m_pendingPacketConsumed(0)
    , m_flac(nullptr)
{
    m_encodedRing = new RingBuffer(kEncodedRingCapacity);
    m_flac = new WriteBridge(this);
}

SBEncoder::~SBEncoder()
{
    m_flac->finish();
    delete m_flac;
    delete[] m_interleaveBuf;
    if (m_pendingPacket)
        m_encodedRing->freePacket(m_pendingPacket);
    delete m_encodedRing;
}

bool SBEncoder::open()
{
    return open(16);
}

bool SBEncoder::open(uint8_t sampleBits)
{
    if (m_phase != Phase::Init) {
        printf("SBEncoder::open(stream=%u) -- already opened\n", m_streamId);
        return false;
    }

    AudioFormat format;
    format.byteOrder = AudioFormat::LittleEndian;
    format.sampleType = AudioFormat::SignedInt;
    format.sampleSize = sampleBits;
    format.sampleRate = kSampleRateHz;
    format.channelCount = kChannelCount;
    format.codec = "audio/pcm";

    m_flac->set_verify(true);
    m_flac->set_compression_level(5);
    m_flac->set_channels(format.channelCount);
    m_flac->set_bits_per_sample(format.sampleSize);
    m_flac->set_sample_rate(format.sampleRate);

    m_bytesPerFrame = format.bytesPerFrame();
    m_sampleBits = format.sampleSize;

    m_encodedRing->clear();
    if (m_pendingPacket) {
        m_encodedRing->freePacket(m_pendingPacket);
        m_pendingPacket = nullptr;
    }

    delete[] m_interleaveBuf;
    m_interleaveBuf = new FLAC__int32[kSamplesPerChunk * format.channelCount];

    FLAC__StreamEncoderInitStatus status = m_flac->init();
    if (status != FLAC__STREAM_ENCODER_INIT_STATUS_OK) {
        printf("SBEncoder::open(stream=%u) -- FLAC encoder error %s\n",
            m_streamId, FLAC__StreamEncoderInitStatusString[status]);
        m_phase = Phase::Closed;
        return false;
    }
    m_phase = Phase::Encoding;
    return true;
}

void SBEncoder::close()
{
    std::lock_guard<std::mutex> lock(m_writeMutex);
    if (m_phase == Phase::Closed)
        return;
    m_flac->finish();
    m_phase = Phase::Closed;
}

int SBEncoder::pendingEncodedBytes() const
{
    if (m_pendingPacket)
        return m_pendingPacket->size - m_pendingPacketConsumed;
    return m_encodedRing->bytesAvailable();
}

int SBEncoder::drainEncodedBytes(char* data, int maxlen)
{
    if (!m_pendingPacket) {
        m_pendingPacket = m_encodedRing->read();
        m_pendingPacketConsumed = 0;
    }
    if (!m_pendingPacket)
        return 0;

    int available = m_pendingPacket->size - m_pendingPacketConsumed;
    int n = (maxlen < available) ? maxlen : available;
    memcpy(data, m_pendingPacket->data + m_pendingPacketConsumed, n);
    m_pendingPacketConsumed += n;
    if (m_pendingPacketConsumed >= m_pendingPacket->size) {
        m_encodedRing->freePacket(m_pendingPacket);
        m_pendingPacket = nullptr;
    }
    return n;
}

int SBEncoder::acceptEncodedBytes(const char* data, int len)
{
    return m_encodedRing->write(data, len);
}

int SBEncoder::encodePcm(const char* data, int len)
{
    int samplesLeft = len / m_bytesPerFrame;
    while (samplesLeft > 0) {
        int chunk = (samplesLeft > kSamplesPerChunk) ? kSamplesPerChunk : samplesLeft;
        const char* cursor = data;
        for (int i = 0; i < chunk * kChannelCount; ++i)
            m_interleaveBuf[i] = nextSampleAsInt32(cursor, m_sampleBits);
        data = cursor;
        if (!m_flac->process_interleaved(m_interleaveBuf, chunk))
            break;
        samplesLeft -= chunk;
    }
    return len;
}

FLAC__StreamEncoderWriteStatus SBEncoder::WriteBridge::write_callback(
    const FLAC__byte buffer[], size_t bytes, unsigned samples, unsigned current_frame)
{
    (void)current_frame;
    int written = m_owner->acceptEncodedBytes((const char*)buffer, (int)bytes);
    if (samples && written == (int)bytes)
        m_owner->m_producedAudio.store(true);
    return (written == (int)bytes) ? FLAC__STREAM_ENCODER_WRITE_STATUS_OK
                                    : FLAC__STREAM_ENCODER_WRITE_STATUS_FATAL_ERROR;
}

int SBEncoder::read(char* data, int maxlen, unsigned timeout, bool holdWhilePaused,
    const std::function<bool()>& peerClosed)
{
    const bool limited = timeout != 0;
    auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout);

    for (;;) {
        if (cancelled() || responseEnded() || (peerClosed && peerClosed()))
            return 0;

        if (m_phase == Phase::Closed) {
            printf("SBEncoder::read: encoder is closed\n");
            return 0;
        }
        if (m_streamId != get_squeezebox_stream_id()) {
            printf("SBEncoder::read: stream mismatch (%u != %u)\n", m_streamId, get_squeezebox_stream_id());
            usleep(1000);
            return 0;
        }

        if (pendingEncodedBytes()) {
            if (!m_firstReadAtMs)
                m_firstReadAtMs = get_sb_time_ms();
            return drainEncodedBytes(data, maxlen);
        }

        if (m_phase == Phase::Closing) {
            printf("All data consumed\n");
            close();
            return 0;
        }

        auto now = std::chrono::steady_clock::now();
        if (holdWhilePaused && sonos_lms_is_paused())
            deadline = now + std::chrono::milliseconds(timeout);
        if (limited && now >= deadline) {
            printf("SBEncoder::read: timeout\n");
            return 0;
        }
        usleep(1000);
    }
}

int SBEncoder::write(const char* data, int len, unsigned timeout, const std::function<void()>& firstPcm)
{
    const bool limited = timeout != 0;

    for (;;) {
        if (cancelled() || responseEnded() || producerRetired())
            return 0;
        if (m_phase != Phase::Encoding) {
            printf("SBEncoder::write: encoder not active\n");
            return 0;
        }
        if (m_streamId != get_squeezebox_stream_id()) {
            printf("SBEncoder::write: stream mismatch (%u != %u)\n", m_streamId, get_squeezebox_stream_id());
            usleep(1000);
            return 0;
        }
        if (len == 0) {
            printf("Reached end of stream\n");
            m_phase = Phase::Closing;
            return 0;
        }

        uint32_t encodedMs = (uint32_t)((uint64_t)m_pcmBytesAccepted / (uint64_t)m_bytesPerFrame
            * 1000ULL / (uint64_t)kSampleRateHz);
        uint32_t playedMs = m_firstReadAtMs ? get_sb_time_ms() - m_firstReadAtMs : 0;

        // Sonos drops its old HTTP connection on resume, so keep the
        // encode-ahead window short: reconnect loss should stay under a
        // second, not the several seconds a fully-buffered decoder would
        // otherwise let build up.
        if (!sonos_lms_is_paused() && encodedMs < playedMs + kMaxEncodeLeadMs) {
            std::lock_guard<std::mutex> lock(m_writeMutex);
            if (cancelled() || responseEnded() || producerRetired() || m_phase != Phase::Encoding)
                return 0;
            if (!m_pcmBytesAccepted && firstPcm) firstPcm();
            m_pcmBytesAccepted += len;
            return encodePcm(data, len);
        }

        if (limited && !sonos_lms_is_paused()) {
            if (!timeout--) {
                printf("SBEncoder::write: timeout\n");
                return 0;
            }
        }
        usleep(1000);
    }
}
