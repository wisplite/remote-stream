#pragma once
#include <cstdint>
#include <string>
#include <vector>

struct CursorImage {
    int width = 0;
    int height = 0;
    // Hotspot offset from the image's top-left corner
    int xhot = 0;
    int yhot = 0;
    // width * height pixels, tightly packed, straight (non-premultiplied) RGBA
    std::vector<uint8_t> rgba;
};

class ICursorSource {
public:
    virtual ~ICursorSource() = default;
    virtual bool init(const std::string& display_target) = 0;
    // Returns true and fills out_cursor when the host cursor's shape changed
    virtual bool poll_cursor(CursorImage& out_cursor) = 0;
    // The next poll_cursor will report the current shape even if it is unchanged
    virtual void request_full_update() = 0;
};
