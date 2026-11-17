#pragma once
#include <memory>
#include "audio_source.hpp"

#ifdef PLATFORM_LINUX
#include "linux/pulse_audio_source.cpp"
#elif defined(PLATFORM_WINDOWS)
#include "windows/dxgi_audio_source.hpp" // placeholder, probably WASAPI later
#endif

inline std::unique_ptr<IAudioSource> create_audio_source() {
#ifdef PLATFORM_LINUX
    return std::make_unique<PulseAudioSource>();
#elif defined(PLATFORM_WINDOWS)
    return std::make_unique<DXGIAudioSource>();
#else
    return nullptr;
#endif
}