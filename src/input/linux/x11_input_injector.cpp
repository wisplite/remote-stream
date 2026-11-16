#include "../input_injector.hpp"
#include <cstdlib>
#include <xcb/xcb.h>
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
        return true;
    }

    void move_mouse(int x, int y) override {
        xcb_test_fake_input(conn_, XCB_MOTION_NOTIFY, 0, XCB_CURRENT_TIME, root_, x, y, 0);
        xcb_flush(conn_);
    }

    void mouse_button(MouseButton button, bool pressed) override {
        uint8_t type = pressed ? XCB_BUTTON_PRESS : XCB_BUTTON_RELEASE;
        uint8_t button_code = static_cast<uint8_t>(button);
        xcb_test_fake_input(conn_, type, button_code, XCB_CURRENT_TIME, XCB_NONE, 0, 0, 0);
        xcb_flush(conn_);
    }

    void scroll(int delta_x, int delta_y) override {
        if (delta_y == 0) return;

        uint8_t btn = (delta_y < 0) ? 4 : 5;
        xcb_test_fake_input(conn_, XCB_BUTTON_PRESS, btn, XCB_CURRENT_TIME, XCB_NONE, 0, 0, 0);
        xcb_test_fake_input(conn_, XCB_BUTTON_RELEASE, btn, XCB_CURRENT_TIME, XCB_NONE, 0, 0, 0);
        xcb_flush(conn_);
    }

    void key_event(uint32_t key, bool pressed) override {
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

    uint8_t lookup_keycode(uint32_t keysym) {
        if (!syms_) return 0;

        xcb_keycode_t* keycodes = xcb_key_symbols_get_keycode(syms_, static_cast<xcb_keysym_t>(keysym));
        if (!keycodes) return 0;

        uint8_t keycode = keycodes[0];
        std::free(keycodes);
        return keycode;
    }
};