#pragma once
#include "../cursor_source.hpp"
#include <xcb/xcb.h>
#include <xcb/xfixes.h>

class X11CursorSource : public ICursorSource {
public:
    X11CursorSource() = default;
    ~X11CursorSource() override;

    bool init(const std::string& display_target) override;
    bool poll_cursor(CursorImage& out_cursor) override;
    void request_full_update() override { update_pending_ = true; }

private:
    xcb_connection_t* conn_ = nullptr;
    xcb_window_t root_ = XCB_NONE;
    uint8_t xfixes_event_base_ = 0;

    // Starts true so the first poll reports the cursor shown at startup
    bool update_pending_ = true;
    CursorImage last_;
};
