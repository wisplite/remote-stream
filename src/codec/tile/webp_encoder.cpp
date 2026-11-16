#include "tile_encoder.hpp"
#include <webp/encode.h>

class WebpTileEncoder : public ITileEncoder {
public:
    explicit WebpTileEncoder(float quality = 75.0f) : quality_(quality) {}

    bool encode(const RawFrameDelta& frame, const Rect& rect, EncodedTile& out_tile) override {
        const uint8_t* start = frame.bgra_buffer + (rect.y * frame.stride) + (rect.x * 4);

        uint8_t* raw_out = nullptr;
        size_t size = WebPEncodeBGRA(start, rect.width, rect.height, frame.stride, quality_, &raw_out);
        if (size == 0 || !raw_out) return false;

        out_tile.bounds = rect;
        out_tile.codec = TileCodec::WebP;
        out_tile.payload.assign(raw_out, raw_out + size);
        WebPFree(raw_out);
        return true;
    }

private:
    float quality_;
};