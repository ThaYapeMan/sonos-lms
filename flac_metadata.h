#ifndef FLAC_METADATA_H
#define FLAC_METADATA_H

#include <algorithm>
#include <cstddef>
#include <cstdint>

// Incrementally consume only the native FLAC marker and metadata blocks.
// Encoder reads may split a marker, block header, or payload at any byte.
class FlacMetadataSkipper {
public:
    // Returns the prefix length to discard; any remainder is untouched audio.
    size_t consume(const char* data, size_t size) {
        size_t offset = 0;
        while (offset < size && !done_ && valid_) {
            if (markerBytes_ < 4) {
                valid_ = data[offset++] == "fLaC"[markerBytes_++];
            } else if (remaining_) {
                size_t take = std::min(size - offset, size_t(remaining_));
                offset += take;
                remaining_ -= take;
                if (!remaining_ && last_) done_ = true;
            } else {
                header_[headerBytes_++] = static_cast<unsigned char>(data[offset++]);
                if (headerBytes_ == 4) {
                    last_ = (header_[0] & 0x80) != 0;
                    remaining_ = (uint32_t(header_[1]) << 16)
                        | (uint32_t(header_[2]) << 8) | header_[3];
                    headerBytes_ = 0;
                    if (!remaining_ && last_) done_ = true;
                }
            }
        }
        skipped_ += offset;
        return offset;
    }

    bool done() const { return done_; }
    bool valid() const { return valid_; }
    size_t skipped() const { return skipped_; }

private:
    size_t markerBytes_ = 0, headerBytes_ = 0, skipped_ = 0;
    unsigned char header_[4]{};
    uint32_t remaining_ = 0;
    bool last_ = false, done_ = false, valid_ = true;
};

#endif
