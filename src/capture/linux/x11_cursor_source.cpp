#include "x11_cursor_source.hpp"
#include <algorithm>
#include <cstdlib>

X11CursorSource::~X11CursorSource() {
    if (conn_) xcb_disconnect(conn_);
}

bool X11CursorSource::init(const std::string& display_target) {
    int screen_num = 0;
    const char* disp = display_target.empty() ? nullptr : display_target.c_str();
    conn_ = xcb_connect(disp, &screen_num);
    if (xcb_connection_has_error(conn_)) return false;

    xcb_screen_iterator_t iter = xcb_setup_roots_iterator(xcb_get_setup(conn_));
    for (int i = 0; i < screen_num; ++i) xcb_screen_next(&iter);
    root_ = iter.data->root;

    const xcb_query_extension_reply_t* ext = xcb_get_extension_data(conn_, &xcb_xfixes_id);
    if (!ext || !ext->present) return false;
    xfixes_event_base_ = ext->first_event;

    // XFixes requires a version handshake before any other request; cursor
    // notifications need v2+
    auto* ver = xcb_xfixes_query_version_reply(
        conn_, xcb_xfixes_query_version(conn_, XCB_XFIXES_MAJOR_VERSION, XCB_XFIXES_MINOR_VERSION), nullptr);
    if (!ver) return false;
    const bool supported = ver->major_version >= 2;
    free(ver);
    if (!supported) return false;

    xcb_xfixes_select_cursor_input(conn_, root_, XCB_XFIXES_CURSOR_NOTIFY_MASK_DISPLAY_CURSOR);
    xcb_flush(conn_);
    return true;
}

bool X11CursorSource::poll_cursor(CursorImage& out_cursor) {
    // Only the newest shape matters, so a burst of notifications collapses into one fetch
    bool changed = update_pending_;
    xcb_generic_event_t* event = nullptr;
    while ((event = xcb_poll_for_event(conn_)) != nullptr) {
        uint8_t response_type = event->response_type & ~0x80;
        if (response_type == xfixes_event_base_ + XCB_XFIXES_CURSOR_NOTIFY) {
            changed = true;
        }
        free(event);
    }
    if (!changed) return false;

    auto* reply = xcb_xfixes_get_cursor_image_reply(conn_, xcb_xfixes_get_cursor_image(conn_), nullptr);
    if (!reply) return false;

    CursorImage img;
    img.width = reply->width;
    img.height = reply->height;
    img.xhot = reply->xhot;
    img.yhot = reply->yhot;

    // XFixes hands back premultiplied ARGB in native-endian 32-bit words
    const uint32_t* argb = xcb_xfixes_get_cursor_image_cursor_image(reply);
    const size_t count = static_cast<size_t>(img.width) * img.height;
    img.rgba.resize(count * 4);
    for (size_t i = 0; i < count; ++i) {
        const uint32_t p = argb[i];
        const uint32_t a = p >> 24;
        uint32_t r = (p >> 16) & 0xFF;
        uint32_t g = (p >> 8) & 0xFF;
        uint32_t b = p & 0xFF;
        if (a == 0) {
            r = g = b = 0;
        } else if (a < 255) {
            r = std::min<uint32_t>(255, (r * 255 + a / 2) / a);
            g = std::min<uint32_t>(255, (g * 255 + a / 2) / a);
            b = std::min<uint32_t>(255, (b * 255 + a / 2) / a);
        }
        img.rgba[i * 4 + 0] = static_cast<uint8_t>(r);
        img.rgba[i * 4 + 1] = static_cast<uint8_t>(g);
        img.rgba[i * 4 + 2] = static_cast<uint8_t>(b);
        img.rgba[i * 4 + 3] = static_cast<uint8_t>(a);
    }
    free(reply);

    const bool forced = update_pending_;
    update_pending_ = false;

    // Apps often re-set the same cursor (e.g. on every hover), which still notifies
    if (!forced && img.width == last_.width && img.height == last_.height &&
        img.xhot == last_.xhot && img.yhot == last_.yhot && img.rgba == last_.rgba) {
        return false;
    }

    last_ = img;
    out_cursor = std::move(img);
    return true;
}
