#pragma once
#include <memory>
#include "input_injector.hpp"

#ifdef PLATFORM_LINUX
#include "linux/x11_input_injector.cpp"
#elif defined(PLATFORM_WINDOWS)
#include "windows/dxgi_input_injector.hpp" // placeholder, probably direct input later
#endif

inline std::unique_ptr<IInputInjector> create_input_injector() {
#ifdef PLATFORM_LINUX
    return std::make_unique<X11InputInjector>();
#elif defined(PLATFORM_WINDOWS)
    return std::make_unique<DXGIInputInjector>();
#else
    return nullptr;
#endif
}