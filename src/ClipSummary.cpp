// SPDX-License-Identifier: Apache-2.0
#include "ClipSummary.h"

#include <cstdio>
#include <cstring>

namespace craw::tui {

ClipSummary ClipSummary::load(const std::string& path) {
    ClipSummary s;
    s.path = path;
    s.name = std::filesystem::path(path).filename().string();
    std::error_code ec;
    s.fileBytes = std::filesystem::file_size(path, ec);
    try {
        const tools::Clip clip(path);
        const auto& i = clip.info();
        s.frames = clip.frameCount();
        std::tie(s.width, s.height) = clip.imageSize();
        s.fps = i.fpsNum / i.fpsDen;
        s.seconds = s.fps > 0 ? s.frames / s.fps : 0;
        s.camera = i.uniqueModel;
        static const char* cfa[] = {"RGGB", "GRBG", "GBRG", "BGGR", "RGB (binned)"};
        s.cfa = cfa[std::min<int>(clip.isRgb() ? 4 : i.cfa & 3, 4)];
        s.black = i.black[0];
        s.white = i.white;
        if (s.frames && s.width) s.bitsPerPixel = 8.0 * s.fileBytes / (double(s.width) * s.height * s.frames);
        const auto& a = clip.audio();
        if (a.format) {
            char buf[64];
            std::snprintf(buf, sizeof buf, "%d ch %.1f kHz %s", a.channels, a.sampleRate / 1000.0,
                          a.format == 2 ? "float32" : a.format == 3 ? "24-bit" : "16-bit");
            s.audio = buf;
        } else {
            s.audio = "none";
        }
        for (const auto& c : clip.reader().chunks())
            if (std::memcmp(c.fourcc.data(), "LSHD", 4) == 0) { s.lensShading = true; break; }
        if (!s.lensShading && s.frames) {
            const auto rec = clip.reader().frame(0);
            s.lensShading = rec.metaJson.find("lensShadingMap") != std::string::npos;
        }
    } catch (const std::exception& e) {
        s.error = e.what();
    }
    return s;
}

std::vector<std::pair<std::string, std::string>> ClipSummary::rows() const {
    if (!error.empty()) return {{"error", error}};
    char buf[96];
    std::vector<std::pair<std::string, std::string>> r;
    r.emplace_back("file", name);
    std::snprintf(buf, sizeof buf, "%ux%u", width, height);
    r.emplace_back("resolution", buf);
    std::snprintf(buf, sizeof buf, "%zu @ %.3g fps (%.1f s)", frames, fps, seconds);
    r.emplace_back("frames", buf);
    r.emplace_back("camera", camera);
    std::snprintf(buf, sizeof buf, "%s  black %.0f  white %.0f", cfa.c_str(), black, white);
    r.emplace_back("sensor", buf);
    r.emplace_back("audio", audio);
    r.emplace_back("lens shading", lensShading ? "yes" : "no");
    std::snprintf(buf, sizeof buf, "%.1f MB  (%.2f bits/px)", fileBytes / 1e6, bitsPerPixel);
    r.emplace_back("size", buf);
    return r;
}

}  // namespace craw::tui
