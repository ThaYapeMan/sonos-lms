#include "encoded_buffer.h"
#include "private/ringbuffer.h"
#include "private/byteorder.h"
namespace upnp {
struct EncodedBuffer::Impl { SONOS::RingBuffer ring; explicit Impl(int n) : ring(n) {} };
EncodedBuffer::EncodedBuffer(int n) : impl(new Impl(n)) {}
EncodedBuffer::~EncodedBuffer() = default;
void EncodedBuffer::clear() { impl->ring.clear(); }
int EncodedBuffer::bytesAvailable() { return impl->ring.bytesAvailable(); }
int EncodedBuffer::write(const char* p, int n) { return impl->ring.write(p, n); }
EncodedBuffer::Packet* EncodedBuffer::read() {
    auto p = impl->ring.read();
    return p ? new Packet{p->size, p->data, p} : nullptr;
}
void EncodedBuffer::freePacket(Packet* p) {
    if (p) { impl->ring.freePacket(static_cast<SONOS::RingBufferPacket*>(p->native)); delete p; }
}
int32_t littleEndianSample(const char* p, unsigned bits) {
    return bits == 16 ? read_b16le(p) : bits == 24 ? read_b24le(p) : read_b32le(p);
}
}
