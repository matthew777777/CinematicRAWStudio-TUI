// SPDX-License-Identifier: Apache-2.0
//
// Small developed previews for the terminal: half-block cells (upper pixel =
// foreground, lower = background), truecolour.
#pragma once

#include "Develop.h"

#include <cstdint>
#include <string>
#include <vector>

namespace craw::tui {

struct Thumbnail {
    uint32_t width = 0, height = 0;  // pixels; height is even (two per text row)
    std::vector<uint8_t> rgb;        // 8-bit sRGB-ish display values
    size_t frame = 0, frames = 0;
    std::string error;
};

// Develops `frame` of `path` with `settings` at half resolution and box-filters
// it to `columns` pixels wide.
Thumbnail makeThumbnail(const std::string& path, size_t frame, const DevelopSettings& settings, uint32_t columns,
                        unsigned threads);

}  // namespace craw::tui
