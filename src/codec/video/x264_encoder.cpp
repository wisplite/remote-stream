#pragma once
#include <cstdint>
#include <x264.h>
#include <libyuv.h>
#include <vector>
#include <atomic>
#include <cstring>

struct EncodedVideoFrame {
    bool is_keyframe = false;
    int64_t timestamp_us = 0;
    std::vector<uint8_t> annexb_data;
};

class X264Encoder {
public:
    ~X264Encoder() { close(); }
    
    bool init(int width, int height, int fps = 60, int bitrate_kbps = 3500) {
        close();
        // 4:2:0 chroma needs even dimensions; drop the odd last row/column
        width_ = width & ~1;
        height_ = height & ~1;
        source_width_ = width;
        source_height_ = height;
        if (width_ <= 0 || height_ <= 0) return false;
        
        x264_param_t param;
        if (x264_param_default_preset(&param, "ultrafast", "zerolatency") < 0) return false;

        param.i_width = width_;
        param.i_height = height_;
        param.i_fps_num = fps;
        param.i_fps_den = 1;
        param.i_csp = X264_CSP_I420;

        // streaming settings
        param.i_threads = 4;
        param.b_sliced_threads = 1;
        param.i_keyint_max = fps * 2;
        param.b_intra_refresh = 0;
        param.b_repeat_headers = 1;
        param.b_annexb = 1;

        param.rc.i_rc_method = X264_RC_ABR;
        param.rc.i_bitrate = bitrate_kbps;
        param.rc.i_vbv_max_bitrate = bitrate_kbps;
        param.rc.i_vbv_buffer_size = bitrate_kbps / 2;
        
        x264_param_apply_profile(&param, "main");

        enc_ = x264_encoder_open(&param);
        if (!enc_) return false;

        x264_picture_alloc(&pic_in_, X264_CSP_I420, width_, height_);
        pic_in_.i_pts = 0;
        force_idr_.store(true); // a fresh encoder has no references to predict from
        return true;
    }

    void close() {
        if (enc_) {
            x264_encoder_close(enc_);
            enc_ = nullptr;
            x264_picture_clean(&pic_in_);
        }
    }

    bool is_open() const { return enc_ != nullptr; }
    // True when the encoder was initialised for a source of exactly this size
    bool matches(int width, int height) const {
        return enc_ && source_width_ == width && source_height_ == height;
    }

    void request_keyframe() { force_idr_.store(true); }

    bool encode_bgra(const uint8_t* bgra, int stride, int64_t timestamp_us, EncodedVideoFrame& out) {
        if (!enc_) return false;

        libyuv::ARGBToI420(
            bgra, stride,
            pic_in_.img.plane[0], pic_in_.img.i_stride[0],
            pic_in_.img.plane[1], pic_in_.img.i_stride[1],
            pic_in_.img.plane[2], pic_in_.img.i_stride[2],
            width_, height_
        );

        pic_in_.i_type = force_idr_.exchange(false) ? X264_TYPE_IDR : X264_TYPE_AUTO;
        pic_in_.i_pts++;

        x264_nal_t* nals = nullptr;
        int num_nals = 0;
        x264_picture_t pic_out;

        int frame_size = x264_encoder_encode(enc_, &nals, &num_nals, &pic_in_, &pic_out);
        if (frame_size <= 0) return false;

        out.is_keyframe = (pic_out.b_keyframe != 0);
        out.timestamp_us = timestamp_us;
        out.annexb_data.resize(frame_size);

        std::memcpy(out.annexb_data.data(), nals[0].p_payload, frame_size);
        return true;
    }
private:
    x264_t* enc_ = nullptr;
    x264_picture_t pic_in_{};
    int width_ = 0;
    int height_ = 0;
    int source_width_ = 0;
    int source_height_ = 0;
    std::atomic<bool> force_idr_{true};
};