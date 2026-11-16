#include "tile_encoder.hpp"
#include <webp/encode.h>

class WebpTileEncoder : public ITileEncoder {
public:
    explicit WebpTileEncoder(float quality = 75.0f) : quality_(quality) {}

    bool encode(const RawFrameDelta& frame, const Rect& rect, EncodedTile& out_tile) override {
        if (!frame.bgra_buffer || rect.width <= 0 || rect.height <= 0) return false;

        const uint8_t* start = frame.bgra_buffer + (static_cast<size_t>(rect.y) * frame.stride) + (static_cast<size_t>(rect.x) * 4);
        const int bgr_stride = rect.width * 3;
        bgr_buffer_.resize(static_cast<size_t>(bgr_stride) * rect.height);

        for (int y = 0; y < rect.height; ++y) {
            const uint8_t* src_row = start + static_cast<size_t>(y) * frame.stride;
            uint8_t* dst_row = bgr_buffer_.data() + static_cast<size_t>(y) * bgr_stride;
            for (int x = 0; x < rect.width; ++x) {
                dst_row[x * 3 + 0] = src_row[x * 4 + 0];
                dst_row[x * 3 + 1] = src_row[x * 4 + 1];
                dst_row[x * 3 + 2] = src_row[x * 4 + 2];
            }
        }

        uint8_t* raw_out = nullptr;
        size_t size = WebPEncodeBGR(bgr_buffer_.data(), rect.width, rect.height, bgr_stride, quality_, &raw_out);
        if (size == 0 || !raw_out) return false;

        out_tile.bounds = rect;
        out_tile.codec = TileCodec::WebP;
        out_tile.payload.assign(raw_out, raw_out + size);
        WebPFree(raw_out);
        return true;
    }

    void set_quality(int quality_0_to_100) override {
        quality_ = static_cast<float>(quality_0_to_100);
    }

private:
    float quality_;
    std::vector<uint8_t> bgr_buffer_;
};