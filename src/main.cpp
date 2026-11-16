#include <iostream>
#include <thread>
#include <chrono>
#include "capture/factory.hpp"

int main() {
    auto capture = create_tile_source();
    if (!capture || !capture->init(":1")) {
        std::cerr << "Failed to initialize capture source\n";
        return 1;
    }

    std::cout << "Listening for XDamage events on :1...\n";
    RawFrameDelta delta;

    while (true) {
        if (capture->poll_delta(delta)) {
            std::cout << "Frame update: " << delta.dirty_rects.size() << " dirty rect(s)\n";
            for (const auto& r : delta.dirty_rects) {
                std::cout << "  -> [" << r.x << ", " << r.y << " " << r.width << "x" << r.height << "]\n";
            }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(16)); // ~60Hz poll
    }

    return 0;
}