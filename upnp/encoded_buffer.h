#pragma once
#include <cstdint>
#include <memory>
namespace upnp {
// Compatibility wrapper around noson's packet queue; no protocol or bridge state.
class EncodedBuffer {
public:
    struct Packet { int size; const char* data; void* native; };
    explicit EncodedBuffer(int capacity);
    ~EncodedBuffer();
    void clear();
    int bytesAvailable();
    int write(const char*, int);
    Packet* read();
    void freePacket(Packet*);
private:
    struct Impl;
    std::unique_ptr<Impl> impl;
};
int32_t littleEndianSample(const char* data, unsigned bits);
}
