#include "x11_tile_source.hpp"
#include <sys/ipc.h>
#include <sys/shm.h>
#include <cstdlib>
#include <cstring>

X11TileSource::~X11TileSource() {
    if (conn_) {
        if (shm_seg_) xcb_shm_detach(conn_, shm_seg_);
        if (damage_) xcb_damage_destroy(conn_, damage_);
        xcb_disconnect(conn_);
    }
    if (shm_addr_ && shm_addr_ != reinterpret_cast<uint8_t*>(-1)) {
        shmdt(shm_addr_);
    }
    if (shm_id_ >= 0) {
        shmctl(shm_id_, IPC_RMID, nullptr);
    }
}

bool X11TileSource::init(const std::string& display_target) {
    int screen_num = 0;
    const char* disp = display_target.empty() ? nullptr : display_target.c_str();
    conn_ = xcb_connect(disp, &screen_num);
    if (xcb_connection_has_error(conn_)) return false;

    const xcb_setup_t* setup = xcb_get_setup(conn_);
    xcb_screen_iterator_t iter = xcb_setup_roots_iterator(setup);
    for (int i = 0; i < screen_num; ++i) xcb_screen_next(&iter);
    screen_ = iter.data;

    width_ = screen_->width_in_pixels;
    height_ = screen_->height_in_pixels;

    // 1. Initialize XDamage extension
    const xcb_query_extension_reply_t* dmg_ext = xcb_get_extension_data(conn_, &xcb_damage_id);
    if (!dmg_ext || !dmg_ext->present) return false;
    damage_event_base_ = dmg_ext->first_event;

    xcb_damage_query_version_unchecked(conn_, XCB_DAMAGE_MAJOR_VERSION, XCB_DAMAGE_MINOR_VERSION);

    damage_ = xcb_generate_id(conn_);
    xcb_damage_create(conn_, damage_, screen_->root, XCB_DAMAGE_REPORT_LEVEL_RAW_RECTANGLES);

    // 2. Allocate POSIX Shared Memory (MIT-SHM) for zero-copy pixel readback
    size_t buffer_size = static_cast<size_t>(width_) * height_ * 4;
    shm_id_ = shmget(IPC_PRIVATE, buffer_size, IPC_CREAT | 0600);
    if (shm_id_ < 0) return false;

    shm_addr_ = static_cast<uint8_t*>(shmat(shm_id_, nullptr, 0));
    if (shm_addr_ == reinterpret_cast<uint8_t*>(-1)) return false;

    shm_seg_ = xcb_generate_id(conn_);
    xcb_shm_attach(conn_, shm_seg_, shm_id_, 0);
    xcb_flush(conn_);

    return true;
}

bool X11TileSource::poll_delta(RawFrameDelta& out_delta) {
    out_delta.dirty_rects.clear();
    out_delta.bgra_buffer = shm_addr_;
    out_delta.width = width_;
    out_delta.height = height_;
    out_delta.stride = width_ * 4;

    // Send full screen on initial connection
    if (first_frame_) {
        first_frame_ = false;
        out_delta.dirty_rects.push_back({0, 0, width_, height_});
    } else {
        // Drain all queued XDamage events since the last poll
        xcb_generic_event_t* event = nullptr;
        while ((event = xcb_poll_for_event(conn_)) != nullptr) {
            uint8_t response_type = event->response_type & ~0x80;
            if (response_type == damage_event_base_ + XCB_DAMAGE_NOTIFY) {
                auto* dmg = reinterpret_cast<xcb_damage_notify_event_t*>(event);
                out_delta.dirty_rects.push_back({
                    dmg->area.x,
                    dmg->area.y,
                    dmg->area.width,
                    dmg->area.height
                });
            }
            free(event);
        }

        if (out_delta.dirty_rects.empty()) {
            return false; // Nothing changed on screen
        }

        // Clear accumulated damage in the X server
        xcb_damage_subtract(conn_, damage_, XCB_NONE, XCB_NONE);
    }

    // Grab the updated framebuffer into SHM (in production, you can optimize this
    // to only call xcb_shm_get_image on the merged dirty bounding box!)
    xcb_shm_get_image_cookie_t cookie = xcb_shm_get_image(
        conn_, screen_->root, 0, 0, width_, height_, ~0, XCB_IMAGE_FORMAT_Z_PIXMAP, shm_seg_, 0
    );
    xcb_shm_get_image_reply_t* reply = xcb_shm_get_image_reply(conn_, cookie, nullptr);
    if (!reply) return false;
    free(reply);

    return true;
}