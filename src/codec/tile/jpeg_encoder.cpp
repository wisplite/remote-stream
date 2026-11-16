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
        const uint8_t* start = frame.bgra_buffer + (rect.y * frame.stride) + (rect.x * 4);

        unsigned char* jpeg_buf = nullptr;
        unsigned long jpeg_size = 0;

        int res = tjCompress2(
            handle_, start, rect.width, frame.stride, rect.height,
            TJPF_BGRA, &jpeg_buf, &jpeg_size, TJSAMP_420, quality_, TJFLAG_FASTDCT
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
};