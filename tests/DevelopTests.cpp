// SPDX-License-Identifier: Apache-2.0
#include "Develop.h"

#include <cmath>
#include <cstdio>

using namespace craw::tui;

static int failures = 0;
static void check(bool ok, const char* what) {
    std::printf("%-6s %s\n", ok ? "PASS" : "FAIL", what);
    failures += !ok;
}
static bool near(double a, double b, double eps) { return std::abs(a - b) <= eps; }

int main() {
    // Transfer functions at reference points.
    check(near(applyTransfer(Transfer::Rec709, 1.0), 1.0, 1e-9), "rec709 maps 1 to 1");
    check(near(applyTransfer(Transfer::Srgb, 0.18), 0.4613, 1e-3), "srgb 18% grey");
    check(near(applyTransfer(Transfer::SLog3, 0.18), 420.0 / 1023.0, 1e-6), "S-Log3 18% grey = 420/1023");
    check(near(applyTransfer(Transfer::LogC3, 0.18), 0.3910, 1e-3), "LogC3 EI800 18% grey ~0.391");
    check(near(applyTransfer(Transfer::Hlg, 0.18), 0.38, 0.01), "HLG 18% grey ~38%");
    check(near(applyTransfer(Transfer::Pq, 1.0), 0.5807, 2e-3), "PQ 203 nit reference white ~0.58");

    // Matrix inverse.
    const Mat3 m{2, 0, 1, 1, 3, 0, 0, 1, 4};
    const Mat3 id = multiply(m, inverse(m));
    bool ok = true;
    for (int i = 0; i < 9; ++i) ok &= near(id[i], i % 4 == 0 ? 1 : 0, 1e-12);
    check(ok, "3x3 inverse");

    // White maps to white: ForwardMatrix of a camera that already sees XYZ D50.
    craw::tools::ClipInfo info;
    info.forwardMatrix1 = {0.4360747, 0.3850649, 0.1430804, 0.2225045, 0.7168786, 0.0606169, 0.0139322, 0.0971045, 0.7141733};
    for (auto g : {Gamut::Rec709, Gamut::Rec2020, Gamut::P3D65, Gamut::AcesAP1}) {
        const Mat3 c = cameraToTarget(info, {0.5, 1.0, 0.7}, g);
        const double r = c[0] + c[1] + c[2], gg = c[3] + c[4] + c[5], b = c[6] + c[7] + c[8];
        char what[64];
        std::snprintf(what, sizeof what, "white-balanced white is neutral in %s", gamutName(g));
        check(near(r, 1, 2e-3) && near(gg, 1, 2e-3) && near(b, 1, 2e-3), what);
    }

    // Develop a flat grey frame: every output pixel neutral; clipped stays neutral.
    craw::tools::RawFrame raw;
    raw.width = 8; raw.height = 8; raw.cfa = 0; raw.black = {64, 64, 64, 64}; raw.white = 1023;
    raw.neutral = {0.5, 1.0, 0.7};
    raw.pixels.resize(64);
    for (uint32_t y = 0; y < 8; ++y)
        for (uint32_t x = 0; x < 8; ++x) {
            const int c = (y & 1) * 2 + (x & 1);  // RGGB
            const double n = c == 0 ? 0.5 : c == 3 ? 0.7 : 1.0;
            raw.pixels[y * 8 + x] = static_cast<uint16_t>(64 + n * 0.2 * (1023 - 64) + 0.5);
        }
    DevelopSettings s;
    s.transfer = Transfer::Linear;
    std::vector<uint16_t> rgb;
    uint32_t w = 0, h = 0;
    develop(raw, info, s, rgb, w, h, 2);
    ok = w == 8 && h == 8;
    for (size_t i = 0; i < rgb.size(); i += 3) ok &= near(rgb[i], rgb[i + 1], 300) && near(rgb[i + 1], rgb[i + 2], 300);
    check(ok, "grey card develops neutral (linear Rec.709)");
    for (auto& v : raw.pixels) v = 1023;  // fully clipped
    develop(raw, info, s, rgb, w, h, 2);
    ok = true;
    for (size_t i = 0; i < rgb.size(); i += 3) ok &= near(rgb[i], rgb[i + 1], 300) && near(rgb[i + 1], rgb[i + 2], 300);
    check(ok, "clipped highlights stay neutral");
    s.halfResolution = true;
    develop(raw, info, s, rgb, w, h, 2);
    check(w == 4 && h == 4 && rgb.size() == 48, "half resolution is 2x2 superpixel");

    std::printf("%d failures\n", failures);
    return failures != 0;
}
