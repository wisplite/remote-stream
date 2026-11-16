#pragma once
#include <cstdint>
#include <vector>
#include "../../capture/tile_source.hpp"

enum class TileCodec : uint8_t {
    RawBGRA = 0,
    WebP = 1,
    JPEG = 2,
    PNG = 3,
    AVIF = 4,
};

struct EncodedTile {
    Rect bounds;
    TileCodec codec;
    std::vector<uint8_t> payload;
};

class ITileEncoder {
public:
    virtual ~ITileEncoder() = default;

    // Compress a sinple dirty rect into a byte blob
    virtual bool encode(const RawFrameDelta& frame, const Rect& rect, EncodedTile& out_tile) = 0;

    // Set the quality of the encoder, 0 = lowest quality, 100 = highest quality
    virtual void set_quality(int quality_0_to_100) = 0;
};