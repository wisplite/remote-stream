#pragma once
#include <memory>
#include "tile_source.hpp"
#include "cursor_source.hpp"

#ifdef PLATFORM_LINUX
#include "linux/x11_tile_source.hpp"
#include "linux/x11_cursor_source.hpp"
#elif defined(PLATFORM_WINDOWS)
#include "windows/dxgi_tile_source.hpp"
#endif

inline std::unique_ptr<ITileSource> create_tile_source() {
#ifdef PLATFORM_LINUX
    return std::make_unique<X11TileSource>();
#elif defined(PLATFORM_WINDOWS)
    return std::make_unique<DXGITileSource>();
#else
    return nullptr;
#endif
}

// May return nullptr on platforms without host cursor capture; callers should
// treat that as "let the client draw its own cursor"
inline std::unique_ptr<ICursorSource> create_cursor_source() {
#ifdef PLATFORM_LINUX
    return std::make_unique<X11CursorSource>();
#else
    return nullptr;
#endif
}
