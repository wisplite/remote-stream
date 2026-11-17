#pragma once
#include <cstdint>
#include <vector>

struct AudioConfig {
    uint32_t sample_rate = 48000;
    uint8_t channels = 2;
    uint32_t frames_per_chunk = 960; // ~20ms at 48000Hz
};

class IAudioSource {
public:
    virtual ~IAudioSource() = default;
    virtual const AudioConfig& config() const = 0;
    virtual bool init(const AudioConfig& config) = 0;
    virtual bool read_chunk(std::vector<float>& out_samples) = 0;
};