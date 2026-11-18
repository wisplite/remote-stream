#include "../input_injector.hpp"
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <xcb/xcb.h>
#include <xcb/dpms.h>
#include <xcb/xtest.h>
#include <xcb/xcb_keysyms.h>

class X11InputInjector : public IInputInjector {
public:
    ~X11InputInjector() override {
        if (syms_) xcb_key_symbols_free(syms_);
        if (conn_) xcb_disconnect(conn_);
    }
    bool init(const std::string& display_target) override {
        const char* disp = display_target.empty() ? nullptr : display_target.c_str();
        conn_ = xcb_connect(disp, nullptr);
        if (xcb_connection_has_error(conn_)) return false;

        const auto* ext = xcb_get_extension_data(conn_, &xcb_test_id);
        if (!ext || !ext->present) return false;

        syms_ = xcb_key_symbols_alloc(conn_);
        if (!syms_) return false;

        xcb_screen_t* screen = xcb_setup_roots_iterator(xcb_get_setup(conn_)).data;
        root_ = screen->root;

        const auto* dpms_ext = xcb_get_extension_data(conn_, &xcb_dpms_id);
        has_dpms_ = dpms_ext && dpms_ext->present;
        return true;
    }

    // While DPMS has the output off, the modesetting driver has no live CRTC and
    // Present falls back to a 1 Hz fake vblank, so a vsynced compositor (xfwm4)
    // only paints once a second. Synthetic XTest input doesn't power the output
    // back on, so do it explicitly.
    void wake_display() override {
        xcb_force_screen_saver(conn_, XCB_SCREEN_SAVER_RESET);
        if (has_dpms_) {
            auto* info = xcb_dpms_info_reply(conn_, xcb_dpms_info(conn_), nullptr);
            if (info) {
                if (info->state && info->power_level != XCB_DPMS_DPMS_MODE_ON) {
                    xcb_dpms_force_level(conn_, XCB_DPMS_DPMS_MODE_ON);
                }
                std::free(info);
            }
        }
        xcb_flush(conn_);
    }

    void move_mouse(int x, int y) override {
        wake_display_throttled();
        xcb_test_fake_input(conn_, XCB_MOTION_NOTIFY, 0, XCB_CURRENT_TIME, root_, x, y, 0);
        xcb_flush(conn_);
    }

    void mouse_button(MouseButton button, bool pressed) override {
        wake_display_throttled();
        uint8_t type = pressed ? XCB_BUTTON_PRESS : XCB_BUTTON_RELEASE;
        uint8_t button_code = static_cast<uint8_t>(button);
        xcb_test_fake_input(conn_, type, button_code, XCB_CURRENT_TIME, XCB_NONE, 0, 0, 0);
        xcb_flush(conn_);
    }

    void scroll(int delta_x, int delta_y) override {
        wake_display_throttled();
        if (delta_y == 0) return;

        uint8_t btn = (delta_y < 0) ? 4 : 5;
        xcb_test_fake_input(conn_, XCB_BUTTON_PRESS, btn, XCB_CURRENT_TIME, XCB_NONE, 0, 0, 0);
        xcb_test_fake_input(conn_, XCB_BUTTON_RELEASE, btn, XCB_CURRENT_TIME, XCB_NONE, 0, 0, 0);
        xcb_flush(conn_);
    }

    void key_event(uint32_t key, bool pressed) override {
        wake_display_throttled();
        uint8_t keycode = lookup_keycode(key);
        if (keycode == 0) return;

        uint8_t type = pressed ? XCB_KEY_PRESS : XCB_KEY_RELEASE;
        xcb_test_fake_input(conn_, type, keycode, XCB_CURRENT_TIME, root_, 0, 0, 0);
        xcb_flush(conn_);
    }
private:
    xcb_connection_t* conn_ = nullptr;
    xcb_window_t root_ = XCB_NONE;
    xcb_key_symbols_t* syms_ = nullptr;

    bool has_dpms_ = false;
    std::atomic<int64_t> last_wake_ms_{0};

    // The idle timers count from the last input, so checking once a second is
    // plenty, and keeps the DPMS round trip off most input events
    void wake_display_throttled() {
        const int64_t now = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count();
        int64_t last = last_wake_ms_.load(std::memory_order_relaxed);
        if (now - last < 1000) return;
        if (!last_wake_ms_.compare_exchange_strong(last, now)) return;
        wake_display();
    }

    uint8_t lookup_keycode(uint32_t keysym) {
        if (!syms_) return 0;

        xcb_keycode_t* keycodes = xcb_key_symbols_get_keycode(syms_, static_cast<xcb_keysym_t>(keysym));
        if (!keycodes) return 0;

        uint8_t keycode = keycodes[0];
        std::free(keycodes);
        return keycode;
    }
};