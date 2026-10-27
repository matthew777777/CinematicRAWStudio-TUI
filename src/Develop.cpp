// SPDX-License-Identifier: Apache-2.0
#include "Develop.h"

#include <algorithm>
#include <cmath>
#include <thread>

namespace craw::tui {
namespace {

struct Primaries { double rx, ry, gx, gy, bx, by, wx, wy; };

constexpr Primaries kD50White{0, 0, 0, 0, 0, 0, 0.34567, 0.35850};

Primaries primariesOf(Gamut g) {
    switch (g) {
        case Gamut::Rec2020: return {0.708, 0.292, 0.170, 0.797, 0.131, 0.046, 0.3127, 0.3290};
        case Gamut::P3D65: return {0.680, 0.320, 0.265, 0.690, 0.150, 0.060, 0.3127, 0.3290};
        case Gamut::AcesAP1: return {0.713, 0.293, 0.165, 0.830, 0.128, 0.044, 0.32168, 0.33767};
        case Gamut::SGamut3Cine: return {0.766, 0.275, 0.225, 0.800, 0.089, -0.087, 0.3127, 0.3290};
        case Gamut::ArriWG3: return {0.6840, 0.3130, 0.2210, 0.8480, 0.0861, -0.1020, 0.3127, 0.3290};
        default: return {0.640, 0.330, 0.300, 0.600, 0.150, 0.060, 0.3127, 0.3290};
    }
}

std::array<double, 3> xyToXYZ(double x, double y) { return {x / y, 1.0, (1 - x - y) / y}; }

Mat3 rgbToXyz(const Primaries& p) {
    const auto r = xyToXYZ(p.rx, p.ry), g = xyToXYZ(p.gx, p.gy), b = xyToXYZ(p.bx, p.by);
    const Mat3 m{r[0], g[0], b[0], r[1], g[1], b[1], r[2], g[2], b[2]};
    const auto w = xyToXYZ(p.wx, p.wy);
    const Mat3 inv = inverse(m);
    const double s[3] = {inv[0] * w[0] + inv[1] * w[1] + inv[2] * w[2], inv[3] * w[0] + inv[4] * w[1] + inv[5] * w[2],
                         inv[6] * w[0] + inv[7] * w[1] + inv[8] * w[2]};
    Mat3 out{};
    for (int r2 = 0; r2 < 3; ++r2)
        for (int c = 0; c < 3; ++c) out[r2 * 3 + c] = m[r2 * 3 + c] * s[c];
    return out;
}

// Bradford chromatic adaptation between two white points (XYZ, Y = 1).
Mat3 bradford(const std::array<double, 3>& from, const std::array<double, 3>& to) {
    const Mat3 b{0.8951, 0.2664, -0.1614, -0.7502, 1.7135, 0.0367, 0.0389, -0.0685, 1.0296};
    auto mul = [&](const std::array<double, 3>& v) {
        return std::array<double, 3>{b[0] * v[0] + b[1] * v[1] + b[2] * v[2], b[3] * v[0] + b[4] * v[1] + b[5] * v[2],
                                     b[6] * v[0] + b[7] * v[1] + b[8] * v[2]};
    };
    const auto s = mul(from), d = mul(to);
    const Mat3 scale{d[0] / s[0], 0, 0, 0, d[1] / s[1], 0, 0, 0, d[2] / s[2]};
    return multiply(inverse(b), multiply(scale, b));
}

std::vector<double> numbersOrEmpty(const std::vector<double>& v) { return v.size() == 9 ? v : std::vector<double>{}; }

}  // namespace

const char* gamutName(Gamut g) {
    static const char* n[] = {"rec709", "rec2020", "p3d65", "acescg", "sgamut3cine", "arriwg3"};
    return n[static_cast<int>(g)];
}

const char* transferName(Transfer t) {
    static const char* n[] = {"rec709", "srgb", "linear", "slog3", "logc3", "hlg", "pq"};
    return n[static_cast<int>(t)];
}

bool parseGamut(const std::string& s, Gamut& out) {
    for (int i = 0; i < static_cast<int>(Gamut::Count); ++i)
        if (s == gamutName(static_cast<Gamut>(i))) { out = static_cast<Gamut>(i); return true; }
    return false;
}

bool parseTransfer(const std::string& s, Transfer& out) {
    for (int i = 0; i < static_cast<int>(Transfer::Count); ++i)
        if (s == transferName(static_cast<Transfer>(i))) { out = static_cast<Transfer>(i); return true; }
    return false;
}

Mat3 multiply(const Mat3& a, const Mat3& b) {
    Mat3 o{};
    for (int r = 0; r < 3; ++r)
        for (int c = 0; c < 3; ++c)
            for (int k = 0; k < 3; ++k) o[r * 3 + c] += a[r * 3 + k] * b[k * 3 + c];
    return o;
}

Mat3 inverse(const Mat3& m) {
    const double a = m[0], b = m[1], c = m[2], d = m[3], e = m[4], f = m[5], g = m[6], h = m[7], i = m[8];
    const double A = e * i - f * h, B = -(d * i - f * g), C = d * h - e * g;
    const double det = a * A + b * B + c * C;
    if (std::abs(det) < 1e-12) return {1, 0, 0, 0, 1, 0, 0, 0, 1};
    const double inv = 1.0 / det;
    return {A * inv, -(b * i - c * h) * inv, (b * f - c * e) * inv,
            B * inv, (a * i - c * g) * inv, -(a * f - c * d) * inv,
            C * inv, -(a * h - b * g) * inv, (a * e - b * d) * inv};
}

Mat3 cameraToTarget(const tools::ClipInfo& info, const std::array<double, 3>& neutral, Gamut gamut) {
    // Input: white-balanced camera RGB, v = camera / AsShotNeutral (white -> 1,1,1).
    const Primaries target = primariesOf(gamut);
    const Mat3 toTarget = multiply(inverse(rgbToXyz(target)),
                                   bradford(xyToXYZ(kD50White.wx, kD50White.wy), xyToXYZ(target.wx, target.wy)));
    if (auto fm = numbersOrEmpty(info.forwardMatrix1); !fm.empty()) {
        // DNG ForwardMatrix maps white-balanced camera RGB to XYZ (D50).
        Mat3 f;
        std::copy(fm.begin(), fm.end(), f.begin());
        return multiply(toTarget, f);
    }
    if (auto cm = numbersOrEmpty(info.colorMatrix1); !cm.empty()) {
        // ColorMatrix maps XYZ to camera RGB: undo white balance, invert, adapt the scene white to D50.
        Mat3 c;
        std::copy(cm.begin(), cm.end(), c.begin());
        const Mat3 camToXyz = multiply(inverse(c), Mat3{neutral[0], 0, 0, 0, neutral[1], 0, 0, 0, neutral[2]});
        std::array<double, 3> w{camToXyz[0] + camToXyz[1] + camToXyz[2], camToXyz[3] + camToXyz[4] + camToXyz[5],
                                camToXyz[6] + camToXyz[7] + camToXyz[8]};
        const double y = w[1] > 0 ? w[1] : 1;
        for (auto& v : w) v /= y;
        Mat3 scaled = camToXyz;
        for (auto& v : scaled) v /= y;
        return multiply(toTarget, multiply(bradford(w, xyToXYZ(kD50White.wx, kD50White.wy)), scaled));
    }
    // No colour data: treat white-balanced camera RGB as linear Rec.709 primaries at D50.
    return multiply(toTarget, rgbToXyz({0.64, 0.33, 0.30, 0.60, 0.15, 0.06, kD50White.wx, kD50White.wy}));
}

double applyTransfer(Transfer t, double x) {
    switch (t) {
        case Transfer::Linear: return x;
        case Transfer::Srgb:
            x = std::max(x, 0.0);
            return x <= 0.0031308 ? 12.92 * x : 1.055 * std::pow(x, 1 / 2.4) - 0.055;
        case Transfer::SLog3:
            return x >= 0.01125 ? (420.0 + std::log10((x + 0.01) / 0.19) * 261.5) / 1023.0
                                : (x * (171.2102946929 - 95.0) / 0.01125 + 95.0) / 1023.0;
        case Transfer::LogC3:  // ARRI LogC3, EI 800
            return x > 0.010591 ? 0.247190 * std::log10(5.555556 * x + 0.052272) + 0.385537 : 5.367655 * x + 0.092809;
        case Transfer::Hlg: {  // BT.2100; scene grey 0.18 -> ~38 % signal (BT.2408)
            const double e = std::max(x * 0.2672, 0.0);
            return e <= 1.0 / 12 ? std::sqrt(3 * e) : 0.17883277 * std::log(12 * e - 0.28466892) + 0.55991073;
        }
        case Transfer::Pq: {  // ST 2084; scene 1.0 -> 203 cd/m2 (BT.2408 reference white)
            const double y = std::clamp(x * 203.0 / 10000.0, 0.0, 1.0);
            const double m1 = 0.1593017578125, m2 = 78.84375, c1 = 0.8359375, c2 = 18.8515625, c3 = 18.6875;
            const double p = std::pow(y, m1);
            return std::pow((c1 + c2 * p) / (1 + c3 * p), m2);
        }
        default:  // BT.709 OETF
            x = std::max(x, 0.0);
            return x < 0.018 ? 4.5 * x : 1.099 * std::pow(x, 0.45) - 0.099;
    }
}

void develop(const tools::RawFrame& raw, const tools::ClipInfo& info, const DevelopSettings& s,
             std::vector<uint16_t>& rgb, uint32_t& outW, uint32_t& outH, unsigned threads) {
    const uint32_t W = raw.width, H = raw.height;
    // Colour of each cell position (py * 2 + px): 0 R, 1 G, 2 B; shading plane 0 R, 1 Gr, 2 Gb, 3 B.
    static const int kColour[4][4] = {{0, 1, 1, 2}, {1, 0, 2, 1}, {1, 2, 0, 1}, {2, 1, 1, 0}};
    const int* colour = kColour[raw.cfa & 3];
    int plane[4];
    for (int pos = 0; pos < 4; ++pos) {
        const int c = colour[pos];
        if (c != 1) { plane[pos] = c == 0 ? 0 : 3; continue; }
        const int rowHasRed = colour[(pos & 2)] == 0 || colour[(pos & 2) | 1] == 0;
        plane[pos] = rowHasRed ? 1 : 2;
    }
    std::array<double, 4> black = raw.black;
    if (s.blackOverride >= 0) black.fill(s.blackOverride);
    const double white = s.whiteOverride > 0 ? s.whiteOverride : raw.white;
    const bool shade = s.lensCorrection && raw.shadingColumns > 0 && raw.shadingRows > 0;
    const Mat3 m = cameraToTarget(info, raw.neutral, s.gamut);
    const double gain = std::exp2(s.exposureEv);
    // A sensor-clipped sample (1.0) white-balances to 1 / neutral; clamp every
    // channel to the lowest of those so clipped highlights stay neutral.
    const double clip = std::min({1 / raw.neutral[0], 1 / raw.neutral[1], 1 / raw.neutral[2]});

    // 1. Normalized, lens-corrected CFA as float.
    std::vector<float> cfa(size_t{W} * H);
    auto shadingGain = [&](uint32_t x, uint32_t y, int p) -> float {
        const double gx = std::clamp((x + 0.5) / W * raw.shadingColumns - 0.5, 0.0, raw.shadingColumns - 1.0);
        const double gy = std::clamp((y + 0.5) / H * raw.shadingRows - 0.5, 0.0, raw.shadingRows - 1.0);
        const int x0 = static_cast<int>(gx), y0 = static_cast<int>(gy);
        const int x1 = std::min(x0 + 1, raw.shadingColumns - 1), y1 = std::min(y0 + 1, raw.shadingRows - 1);
        const double tx = gx - x0, ty = gy - y0;
        auto at = [&](int xx, int yy) { return raw.shadingGains[(size_t(yy) * raw.shadingColumns + xx) * 4 + p]; };
        return static_cast<float>((at(x0, y0) * (1 - tx) + at(x1, y0) * tx) * (1 - ty) + (at(x0, y1) * (1 - tx) + at(x1, y1) * tx) * ty);
    };
    auto parallelRows = [&](uint32_t rows, auto&& fn) {
        const unsigned n = std::max(1u, std::min(threads, rows));
        std::vector<std::thread> pool;
        for (unsigned t = 0; t < n; ++t)
            pool.emplace_back([&, t] { for (uint32_t y = t; y < rows; y += n) fn(y); });
        for (auto& th : pool) th.join();
    };
    parallelRows(H, [&](uint32_t y) {
        for (uint32_t x = 0; x < W; ++x) {
            const int pos = static_cast<int>((y & 1) * 2 + (x & 1));
            const double range = std::max(white - black[pos], 1.0);
            float v = static_cast<float>((raw.pixels[size_t(y) * W + x] - black[pos]) / range);
            if (shade) v *= shadingGain(x, y, plane[pos]);
            cfa[size_t(y) * W + x] = v;
        }
    });

    // 2. Demosaic, colour, transfer.
    outW = s.halfResolution ? W / 2 : W;
    outH = s.halfResolution ? H / 2 : H;
    rgb.resize(size_t{outW} * outH * 3);
    auto finish = [&](double r, double g, double b, uint16_t* o) {
        r /= raw.neutral[0];
        g /= raw.neutral[1];
        b /= raw.neutral[2];
        if (s.clipHighlights) {
            r = std::min(r, clip);
            g = std::min(g, clip);
            b = std::min(b, clip);
        }
        const double R = (m[0] * r + m[1] * g + m[2] * b) * gain;
        const double G = (m[3] * r + m[4] * g + m[5] * b) * gain;
        const double B = (m[6] * r + m[7] * g + m[8] * b) * gain;
        auto q = [&](double v) {
            return static_cast<uint16_t>(std::lround(std::clamp(applyTransfer(s.transfer, v), 0.0, 1.0) * 65535));
        };
        o[0] = q(R);
        o[1] = q(G);
        o[2] = q(B);
    };
    if (s.halfResolution) {
        parallelRows(outH, [&](uint32_t y) {
            for (uint32_t x = 0; x < outW; ++x) {
                double c[3] = {0, 0, 0};
                int n[3] = {0, 0, 0};
                for (int pos = 0; pos < 4; ++pos) {
                    c[colour[pos]] += cfa[size_t(2 * y + (pos >> 1)) * W + 2 * x + (pos & 1)];
                    ++n[colour[pos]];
                }
                finish(c[0] / n[0], c[1] / n[1], c[2] / n[2], &rgb[(size_t(y) * outW + x) * 3]);
            }
        });
    } else {
        // Bilinear: own sample for its colour, mean of same-colour 3x3 neighbours otherwise.
        parallelRows(H, [&](uint32_t y) {
            for (uint32_t x = 0; x < W; ++x) {
                double sum[3] = {0, 0, 0};
                int n[3] = {0, 0, 0};
                const int own = colour[(y & 1) * 2 + (x & 1)];
                for (int dy = -1; dy <= 1; ++dy) {
                    const int yy = std::clamp(static_cast<int>(y) + dy, 0, static_cast<int>(H) - 1);
                    for (int dx = -1; dx <= 1; ++dx) {
                        if (!dx && !dy) continue;
                        const int xx = std::clamp(static_cast<int>(x) + dx, 0, static_cast<int>(W) - 1);
                        const int c = colour[(yy & 1) * 2 + (xx & 1)];
                        sum[c] += cfa[size_t(yy) * W + xx];
                        ++n[c];
                    }
                }
                double v[3];
                for (int c = 0; c < 3; ++c) v[c] = n[c] ? sum[c] / n[c] : 0;
                v[own] = cfa[size_t(y) * W + x];
                finish(v[0], v[1], v[2], &rgb[(size_t(y) * W + x) * 3]);
            }
        });
    }
}

}  // namespace craw::tui
