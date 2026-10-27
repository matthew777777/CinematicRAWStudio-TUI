// SPDX-License-Identifier: Apache-2.0
#include "Thumbnail.h"

#include <algorithm>

namespace craw::tui {

Thumbnail makeThumbnail(const std::string& path, size_t frame, const DevelopSettings& settings, uint32_t columns,
                        unsigned threads) {
    Thumbnail t;
    try {
        const tools::Clip clip(path);
        t.frames = clip.frameCount();
        if (t.frames == 0) throw std::runtime_error("no frames");
        t.frame = std::min(frame, t.frames - 1);
        DevelopSettings s = settings;
        s.halfResolution = true;
        const auto raw = clip.frameRaw(t.frame, threads);
        std::vector<uint16_t> rgb;
        uint32_t w = 0, h = 0;
        develop(raw, clip.info(), s, rgb, w, h, threads);
        t.width = std::max(1u, std::min(columns, w));
        t.height = std::max(2u, (static_cast<uint32_t>(double(h) * t.width / w + 1) / 2) * 2);
        t.rgb.assign(size_t{t.width} * t.height * 3, 0);
        for (uint32_t y = 0; y < t.height; ++y) {
            const uint32_t y0 = y * h / t.height, y1 = std::max(y0 + 1, (y + 1) * h / t.height);
            for (uint32_t x = 0; x < t.width; ++x) {
                const uint32_t x0 = x * w / t.width, x1 = std::max(x0 + 1, (x + 1) * w / t.width);
                uint64_t sum[3] = {0, 0, 0}, n = 0;
                for (uint32_t yy = y0; yy < y1; yy += std::max(1u, (y1 - y0) / 4))
                    for (uint32_t xx = x0; xx < x1; xx += std::max(1u, (x1 - x0) / 4), ++n)
                        for (int c = 0; c < 3; ++c) sum[c] += rgb[(size_t(yy) * w + xx) * 3 + c];
                for (int c = 0; c < 3; ++c) t.rgb[(size_t(y) * t.width + x) * 3 + c] = static_cast<uint8_t>(sum[c] / n >> 8);
            }
        }
    } catch (const std::exception& e) {
        t.error = e.what();
    }
    return t;
}

}  // namespace craw::tui
