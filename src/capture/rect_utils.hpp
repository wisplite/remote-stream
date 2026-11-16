#pragma once

#include <vector>
#include "tile_source.hpp"

class RectUtils {
public:
    static inline void merge_dirty_rects(std::vector<Rect>& dirty_rects, int padding = 4) {
        if (dirty_rects.size() <= 1) return;

        bool merged = true;
        while (merged) {
            merged = false;
            for (size_t i = 0; i < dirty_rects.size(); ++i) {
                for (size_t j = i + 1; j < dirty_rects.size(); ++j) {
                    const auto& a = dirty_rects[i];
                    const auto& b = dirty_rects[j];

                    // Check if A and B overlap or are within `padding` pixels of touching
                    bool overlap_x = (a.x - padding <= b.x + b.width) && (a.x + a.width + padding >= b.x);
                    bool overlap_y = (a.y - padding <= b.y + b.height) && (a.y + a.height + padding >= b.y);

                    if (overlap_x && overlap_y) {
                        int min_x = std::min(a.x, b.x);
                        int min_y = std::min(a.y, b.y);
                        int max_x = std::max(a.x + a.width, b.x + b.width);
                        int max_y = std::max(a.y + a.height, b.y + b.height);

                        dirty_rects[i] = {min_x, min_y, max_x - min_x, max_y - min_y};
                        dirty_rects.erase(dirty_rects.begin() + j);
                        merged = true;
                        break;
                    }
                }
                if (merged) break;
            }
        }
    }
};