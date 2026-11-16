#pragma once
#include <cstdint>
#include <string>
#include <vector>

struct Rect {
    int x = 0;
    int y = 0;
    int width = 0;
    int height = 0;
};

struct RawFrameDelta {
    const uint8_t* bgra_buffer = nullptr;
    int width = 0;
    int height = 0;
    int stride = 0;
    std::vector<Rect> dirty_rects;
};

class ITileSource {
public:
    virtual ~ITileSource() = default;
    virtual bool init(const std::string& display_target) = 0;
    virtual bool poll_delta(RawFrameDelta& out_delta) = 0;
};