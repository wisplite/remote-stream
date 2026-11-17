#include "../audio_source.hpp"
#include <pulse/simple.h>
#include <pulse/error.h>
#include <iostream>

class PulseAudioSource : public IAudioSource {
public:
    ~PulseAudioSource() override {
        if (pa_) pa_simple_free(pa_);
    }

    bool init(const AudioConfig& config) override {
        config_ = config;

        pa_sample_spec spec{};
        spec.format = PA_SAMPLE_FLOAT32LE;
        spec.rate = config.sample_rate;
        spec.channels = config.channels;

        uint32_t bytes_per_chunk = config.frames_per_chunk * config.channels * sizeof(float);
        pa_buffer_attr attr{};
        attr.maxlength = bytes_per_chunk * 4;
        attr.fragsize = bytes_per_chunk;

        int error = 0;
        pa_ = pa_simple_new(
            nullptr,
            "remote-stream",
            PA_STREAM_RECORD,
            "@DEFAULT_MONITOR@",
            "Desktop Audio",
            &spec,
            nullptr,
            &attr,
            &error
        );

        if (!pa_) {
            std::cerr << "PulseAudio init failed: " << pa_strerror(error) << std::endl;
            return false;
        }

        return true;
    }

    const AudioConfig& config() const override {
        return config_;
    }

    bool read_chunk(std::vector<float>& out_samples) override {
        size_t total_floats = config_.frames_per_chunk * config_.channels;
        out_samples.resize(total_floats);

        int error = 0;
        if (pa_simple_read(pa_, out_samples.data(), total_floats * sizeof(float), &error) < 0) {
            std::cerr << "PulseAudio read failed: " << pa_strerror(error) << std::endl;
            return false;
        }

        return true;
    }
private:
    pa_simple* pa_ = nullptr;
    AudioConfig config_;
};