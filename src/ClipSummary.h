// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "common/Clip.h"

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace craw::tui {

// Metadata shown in the browser / media pool and by `craw-tui info`.
struct ClipSummary {
    std::string path, name, error;
    uint32_t width = 0, height = 0;
    size_t frames = 0;
    double fps = 0, seconds = 0, bitsPerPixel = 0;
    uint64_t fileBytes = 0;
    std::string camera, cfa, audio;
    double black = 0, white = 0;
    bool lensShading = false;

    static ClipSummary load(const std::string& path);
    std::vector<std::pair<std::string, std::string>> rows() const;
};

}  // namespace craw::tui
