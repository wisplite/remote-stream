#pragma once
#include <opus/opus.h>
#include <cstdint>
#include <vector>
#include <iostream>

class OpusAudioEncoder {
public:
    bool init(int sample_rate = 48000, int channels = 2, int bitrate = 128000) {
        int err = 0;
        enc_ = opus_encoder_create(sample_rate, channels, OPUS_APPLICATION_AUDIO, &err);
        if (err != OPUS_OK || !enc_) {
            std::cerr << "Opus encoder init failed: " << opus_strerror(err) << std::endl;
            return false;
        }

        opus_encoder_ctl(enc_, OPUS_SET_BITRATE(bitrate));
        opus_encoder_ctl(enc_, OPUS_SET_COMPLEXITY(5));
        channels_ = channels;
        return true;
    }

    ~OpusAudioEncoder() {
        if (enc_) opus_encoder_destroy(enc_);
    }

    bool encode(const std::vector<float>& pcm_interleaved, std::vector<uint8_t>& out_packet) {
        int frame_size = static_cast<int>(pcm_interleaved.size() / channels_);
        out_packet.resize(4000);

        int bytes = opus_encode_float(
            enc_,
            pcm_interleaved.data(),
            frame_size,
            out_packet.data(),
            static_cast<opus_int32>(out_packet.size())
        );

        if (bytes < 0) {
            std::cerr << "Opus encode failed: " << opus_strerror(bytes) << std::endl;
            return false;
        }
        out_packet.resize(bytes);
        return true;
    }
private:
    OpusEncoder* enc_ = nullptr;
    int channels_ = 2;
};