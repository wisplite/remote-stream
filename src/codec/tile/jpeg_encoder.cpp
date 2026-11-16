#include "tile_encoder.hpp"
#include <stdexcept>
#include <turbojpeg.h>

class JpegTileEncoder : public ITileEncoder {
public:
    explicit JpegTileEncoder(int quality = 80) : quality_(quality) {
        handle_ = tjInitCompress();
        if (!handle_) throw std::runtime_error("Failed to initialize TurboJPEG compressor");
    }
    ~JpegTileEncoder() override {
        tjDestroy(handle_);
    }

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

        unsigned char* jpeg_buf = nullptr;
        unsigned long jpeg_size = 0;

        int res = tjCompress2(
            handle_, bgr_buffer_.data(), rect.width, bgr_stride, rect.height,
            TJPF_BGR, &jpeg_buf, &jpeg_size, TJSAMP_420, quality_, TJFLAG_FASTDCT
        );
        if (res != 0) return false;

        out_tile.bounds = rect;
        out_tile.codec = TileCodec::JPEG;
        out_tile.payload.assign(jpeg_buf, jpeg_buf + jpeg_size);
        tjFree(jpeg_buf);
        return true;
    }

    void set_quality(int quality_0_to_100) override {
        quality_ = quality_0_to_100;
    }

private:
    int quality_;
    tjhandle handle_ = nullptr;
    std::vector<uint8_t> bgr_buffer_;
};