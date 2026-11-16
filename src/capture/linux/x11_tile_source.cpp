#include "x11_tile_source.hpp"
#include <sys/ipc.h>
#include <sys/shm.h>
#include <algorithm>
#include <cstdlib>
#include <cstring>

X11TileSource::~X11TileSource() {
    if (conn_) {
        if (damage_) xcb_damage_destroy(conn_, damage_);
    }
    free_shm();
    if (conn_) xcb_disconnect(conn_);
}

bool X11TileSource::alloc_shm() {
    size_t buffer_size = static_cast<size_t>(width_) * height_ * 4;
    shm_id_ = shmget(IPC_PRIVATE, buffer_size, IPC_CREAT | 0600);
    if (shm_id_ < 0) return false;

    shm_addr_ = static_cast<uint8_t*>(shmat(shm_id_, nullptr, 0));
    if (shm_addr_ == reinterpret_cast<uint8_t*>(-1)) {
        shm_addr_ = nullptr;
        return false;
    }

    shm_seg_ = xcb_generate_id(conn_);
    xcb_shm_attach(conn_, shm_seg_, shm_id_, 0);
    xcb_flush(conn_);
    return true;
}

void X11TileSource::free_shm() {
    if (conn_ && shm_seg_) {
        xcb_shm_detach(conn_, shm_seg_);
        xcb_flush(conn_);
        shm_seg_ = 0;
    }
    if (shm_addr_) {
        shmdt(shm_addr_);
        shm_addr_ = nullptr;
    }
    if (shm_id_ >= 0) {
        shmctl(shm_id_, IPC_RMID, nullptr);
        shm_id_ = -1;
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

    // Root ConfigureNotify events report desktop resizes (e.g. RandR mode changes)
    const uint32_t event_mask = XCB_EVENT_MASK_STRUCTURE_NOTIFY;
    xcb_change_window_attributes(conn_, screen_->root, XCB_CW_EVENT_MASK, &event_mask);

    // The setup block is a snapshot from connect time, so ask for the live geometry
    width_ = screen_->width_in_pixels;
    height_ = screen_->height_in_pixels;
    if (auto* geo = xcb_get_geometry_reply(conn_, xcb_get_geometry(conn_, screen_->root), nullptr)) {
        width_ = geo->width;
        height_ = geo->height;
        free(geo);
    }

    // 1. Initialize XDamage extension
    const xcb_query_extension_reply_t* dmg_ext = xcb_get_extension_data(conn_, &xcb_damage_id);
    if (!dmg_ext || !dmg_ext->present) return false;
    damage_event_base_ = dmg_ext->first_event;

    xcb_damage_query_version_unchecked(conn_, XCB_DAMAGE_MAJOR_VERSION, XCB_DAMAGE_MINOR_VERSION);

    damage_ = xcb_generate_id(conn_);
    xcb_damage_create(conn_, damage_, screen_->root, XCB_DAMAGE_REPORT_LEVEL_RAW_RECTANGLES);

    // 2. Allocate POSIX Shared Memory (MIT-SHM) for zero-copy pixel readback
    return alloc_shm();
}

bool X11TileSource::poll_delta(RawFrameDelta& out_delta) {
    out_delta.dirty_rects.clear();
    out_delta.full_frame = false;
    out_delta.size_changed = false;

    // Drain all queued events since the last poll
    int new_width = width_;
    int new_height = height_;
    bool got_damage = false;
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
            got_damage = true;
        } else if (response_type == XCB_CONFIGURE_NOTIFY) {
            auto* cfg = reinterpret_cast<xcb_configure_notify_event_t*>(event);
            if (cfg->window == screen_->root) {
                new_width = cfg->width;
                new_height = cfg->height;
            }
        }
        free(event);
    }

    if (got_damage) {
        // Clear accumulated damage in the X server
        xcb_damage_subtract(conn_, damage_, XCB_NONE, XCB_NONE);
    }

    if (new_width != width_ || new_height != height_) {
        free_shm();
        width_ = new_width;
        height_ = new_height;
        if (!alloc_shm()) return false;
        size_changed_ = true;
        full_frame_pending_ = true;
    }

    if (full_frame_pending_) {
        out_delta.dirty_rects.assign(1, {0, 0, width_, height_});
    } else {
        // Damage queued before a shrink can reference pixels that no longer exist
        std::erase_if(out_delta.dirty_rects, [&](Rect& r) {
            int x0 = std::max(r.x, 0);
            int y0 = std::max(r.y, 0);
            int x1 = std::min(r.x + r.width, width_);
            int y1 = std::min(r.y + r.height, height_);
            r = {x0, y0, x1 - x0, y1 - y0};
            return r.width <= 0 || r.height <= 0;
        });
        if (out_delta.dirty_rects.empty()) {
            return false; // Nothing changed on screen
        }
    }

    // Grab the updated framebuffer into SHM (in production, you can optimize this
    // to only call xcb_shm_get_image on the merged dirty bounding box!)
    xcb_shm_get_image_cookie_t cookie = xcb_shm_get_image(
        conn_, screen_->root, 0, 0, width_, height_, ~0, XCB_IMAGE_FORMAT_Z_PIXMAP, shm_seg_, 0
    );
    xcb_generic_error_t* error = nullptr;
    xcb_shm_get_image_reply_t* reply = xcb_shm_get_image_reply(conn_, cookie, &error);
    if (!reply) {
        // Typically the screen resized between draining events and this request;
        // the ConfigureNotify arrives on the next poll, so resend everything then
        free(error);
        full_frame_pending_ = true;
        return false;
    }
    free(reply);

    out_delta.bgra_buffer = shm_addr_;
    out_delta.width = width_;
    out_delta.height = height_;
    out_delta.stride = width_ * 4;
    out_delta.full_frame = full_frame_pending_;
    out_delta.size_changed = size_changed_;
    full_frame_pending_ = false;
    size_changed_ = false;
    return true;
}
