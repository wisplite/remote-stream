#pragma once
#include "../tile_source.hpp"
#include <xcb/xcb.h>
#include <xcb/damage.h>
#include <xcb/shm.h>

class X11TileSource : public ITileSource {
public:
    X11TileSource() = default;
    ~X11TileSource() override;

    bool init(const std::string& display_target) override;
    bool poll_delta(RawFrameDelta& out_delta) override;
    void request_full_frame() override { full_frame_pending_ = true; }

private:
    bool alloc_shm();
    void free_shm();

    xcb_connection_t* conn_ = nullptr;
    xcb_screen_t* screen_ = nullptr;
    xcb_damage_damage_t damage_ = 0;
    uint8_t damage_event_base_ = 0;

    int shm_id_ = -1;
    uint8_t* shm_addr_ = nullptr;
    xcb_shm_seg_t shm_seg_ = 0;

    int width_ = 0;
    int height_ = 0;
    bool full_frame_pending_ = true;
    bool size_changed_ = true;
};
