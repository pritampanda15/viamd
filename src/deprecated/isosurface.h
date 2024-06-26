#pragma once

#include <stdint.h>

struct IsoSurfaces {
    static constexpr int MaxCount = 8;
    float values[MaxCount] = {};
    uint32_t colors[MaxCount] = {};
    int count = 0;
};

static inline bool insert(IsoSurfaces& surface, float value, uint32_t color) {
    if (surface.count >= IsoSurfaces::MaxCount) return false;
    surface.values[surface.count] = value;
    surface.colors[surface.count] = color;
    ++surface.count;
    return true;
}

static inline void clear(IsoSurfaces& surface) {
    surface = {};
}

static inline void sort(IsoSurfaces& surface) {
    for (int i = 0; i < surface.count - 1; ++i) {
        for (int j = i + 1; j < surface.count; ++j) {
            if (surface.values[j] < surface.values[i]) {
                const auto tmp_val = surface.values[i];
                const auto tmp_col = surface.colors[i];
                surface.values[i] = surface.values[j];
                surface.colors[i] = surface.colors[j];
                surface.values[j] = tmp_val;
                surface.colors[j] = tmp_col;
            }
        }
    }
}