#pragma once
#include <cstdint>
#include <string>

enum class MouseButton : uint8_t {
    Left = 1,
    Middle = 2,
    Right = 3,
    WheelUp = 4,
    WheelDown = 5,
};

class IInputInjector {
public:
    virtual ~IInputInjector() = default;
    virtual bool init(const std::string& display_target) = 0;

    virtual void move_mouse(int x, int y) = 0;
    virtual void mouse_button(MouseButton button, bool pressed) = 0;
    virtual void scroll(int delta_x, int delta_y) = 0;

    virtual void key_event(uint32_t key, bool pressed) = 0;

    // Bring the display out of power saving so the compositor paints at full rate
    virtual void wake_display() {}
};