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
    // Always the full display. Stays valid (holding the last captured frame) across
    // poll_delta calls that return false; the source resets it to nullptr itself
    // before the memory goes away (e.g. on resize), so reuse the same delta object.
    const uint8_t* bgra_buffer = nullptr;
    int width = 0;
    int height = 0;
    int stride = 0;
    std::vector<Rect> dirty_rects;
    // dirty_rects covers the whole screen; viewers should treat this as a keyframe
    bool full_frame = false;
    // width/height differ from the previous delta
    bool size_changed = false;
};

class ITileSource {
public:
    virtual ~ITileSource() = default;
    virtual bool init(const std::string& display_target) = 0;
    virtual bool poll_delta(RawFrameDelta& out_delta) = 0;
    // The next successful poll_delta will report the entire screen as dirty
    virtual void request_full_frame() = 0;
};
