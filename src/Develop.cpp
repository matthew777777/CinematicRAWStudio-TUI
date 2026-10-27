// SPDX-License-Identifier: Apache-2.0
#include "Develop.h"

#include <algorithm>
#include <cstring>
#include <memory>
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

void applyGrade(const DevelopSettings& s, double& r, double& g, double& b) {
    double* c[3] = {&r, &g, &b};
    if (s.blacks != 0)
        for (double* v : c) *v = (*v - s.blacks) / (1 - s.blacks);
    if (s.contrast != 1)
        for (double* v : c)
            if (*v > 0) *v = 0.18 * std::pow(*v / 0.18, s.contrast);
    if (s.shadows != 0 || s.highlights != 0) {
        // Tone the luminance, keep the ratios (no hue shifts).
        const double y = 0.2126 * r + 0.7152 * g + 0.0722 * b;
        if (y > 0) {
            double t = y;
            if (y < 0.18) t = y * (1 + s.shadows * (1 - y / 0.18));
            else t = 0.18 + (y - 0.18) * std::exp2(s.highlights);
            const double k = t / y;
            for (double* v : c) *v *= k;
        }
    }
    if (s.saturation != 1) {
        const double y = 0.2126 * r + 0.7152 * g + 0.0722 * b;
        for (double* v : c) *v = y + (*v - y) * s.saturation;
    }
}

namespace {

// Transfer curve indexed by the float's bit pattern: the top 11 mantissa bits
// plus the exponent give 2048 entries per stop over [2^-20, 2^12]; the next
// 12 bits interpolate. No log2 per sample.
class TransferLut {
public:
    explicit TransferLut(Transfer t) : t_(t) {
        lut_.resize(kEntries + 1);
        for (uint32_t i = 0; i <= kEntries; ++i) {
            const uint32_t bits = (kBase + i) << kShift;
            float x;
            std::memcpy(&x, &bits, 4);
            lut_[i] = static_cast<float>(std::clamp(applyTransfer(t, x), 0.0, 1.0));
        }
    }
    uint16_t operator()(float x) const {
        uint32_t bits;
        std::memcpy(&bits, &x, 4);
        float v;
        if (!(x >= kMinValue)) {
            v = static_cast<float>(std::clamp(applyTransfer(t_, std::max(x, 0.f)), 0.0, 1.0));
        } else {
            const uint32_t i = (bits >> kShift) - kBase;
            if (i >= kEntries) v = lut_[kEntries];
            else {
                const float w = static_cast<float>(bits & ((1u << kShift) - 1)) * (1.f / (1u << kShift));
                v = lut_[i] + (lut_[i + 1] - lut_[i]) * w;
            }
        }
        return static_cast<uint16_t>(v * 65535.f + 0.5f);
    }

private:
    static constexpr uint32_t kShift = 12;                  // 23 - 11 mantissa bits kept
    static constexpr uint32_t kBase = (127u - 20u) << 11;   // 2^-20
    static constexpr uint32_t kEntries = 32u << 11;         // 32 stops
    static constexpr float kMinValue = 9.5367431640625e-07f;
    Transfer t_;
    std::vector<float> lut_;
};

}  // namespace

void develop(const tools::RawFrame& raw, const tools::ClipInfo& info, const DevelopSettings& s,
             std::vector<uint16_t>& rgb, uint32_t& outW, uint32_t& outH, unsigned threads) {
    const uint32_t W = raw.width, H = raw.height;
    // Colour of each cell position (py * 2 + px): 0 R, 1 G, 2 B; shading plane 0 R, 1 Gr, 2 Gb, 3 B.
    // RGB frames (cfa 4, SPEC 3.8): cells R, G / B, unused; one pixel per cell.
    static const int kColour[5][4] = {{0, 1, 1, 2}, {1, 0, 2, 1}, {1, 2, 0, 1}, {2, 1, 1, 0}, {0, 1, 2, -1}};
    const bool rgbCells = raw.cfa == 4;
    const int* colour = kColour[rgbCells ? 4 : raw.cfa & 3];
    int plane[4];
    for (int pos = 0; pos < 4; ++pos) {
        const int c = colour[pos];
        if (rgbCells) { plane[pos] = c == 0 ? 0 : c == 2 ? 3 : 1; continue; }
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
    // Temperature / tint as gains on white-balanced camera RGB (1 stop at +-100).
    const double wbR = std::exp2(s.temperature / 100), wbB = std::exp2(-s.temperature / 100);
    const double wbG = std::exp2(-s.tint / 100);
    static thread_local std::unique_ptr<TransferLut> lutCache;
    static thread_local Transfer lutFor = Transfer::Count;
    if (!lutCache || lutFor != s.transfer) { lutCache = std::make_unique<TransferLut>(s.transfer); lutFor = s.transfer; }
    const TransferLut& lut = *lutCache;
    const bool graded = s.contrast != 1 || s.saturation != 1 || s.highlights != 0 || s.shadows != 0 || s.blacks != 0;
    // Per pixel: divide by neutral, clamp, then one 3x3 matrix that already holds
    // temperature/tint gains and exposure.
    const float inv[3] = {static_cast<float>(1 / raw.neutral[0]), static_cast<float>(1 / raw.neutral[1]),
                          static_cast<float>(1 / raw.neutral[2])};
    const float wbGain[3] = {static_cast<float>(wbR), static_cast<float>(wbG), static_cast<float>(wbB)};
    float mf[9];
    for (int i = 0; i < 9; ++i) mf[i] = static_cast<float>(m[i] * gain) * wbGain[i % 3];
    const float clipF = static_cast<float>(clip);

    // 1. Normalized, lens-corrected CFA as float.
    std::vector<float> cfa(size_t{W} * H);
    // Lens-shading bilinear weights per column and row (grid points at cell centres).
    struct Tap { int i0, i1; float w; };
    std::vector<Tap> colTap, rowTap;
    if (shade) {
        auto taps = [](uint32_t n, int points, std::vector<Tap>& out) {
            out.resize(n);
            for (uint32_t k = 0; k < n; ++k) {
                const double g = std::clamp((k + 0.5) / n * points - 0.5, 0.0, points - 1.0);
                const int i0 = static_cast<int>(g);
                out[k] = {i0, std::min(i0 + 1, points - 1), static_cast<float>(g - i0)};
            }
        };
        taps(W, raw.shadingColumns, colTap);
        taps(H, raw.shadingRows, rowTap);
    }
    auto shadingGain = [&](uint32_t x, uint32_t y, int p) -> float {
        const Tap& cx = colTap[x];
        const Tap& ry = rowTap[y];
        auto at = [&](int xx, int yy) { return raw.shadingGains[(size_t(yy) * raw.shadingColumns + xx) * 4 + p]; };
        const float top = at(cx.i0, ry.i0) + (at(cx.i1, ry.i0) - at(cx.i0, ry.i0)) * cx.w;
        const float bot = at(cx.i0, ry.i1) + (at(cx.i1, ry.i1) - at(cx.i0, ry.i1)) * cx.w;
        return top + (bot - top) * ry.w;
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
    const bool half = s.halfResolution || rgbCells;
    outW = half ? W / 2 : W;
    outH = half ? H / 2 : H;
    rgb.resize(size_t{outW} * outH * 3);
    auto finish = [&](float r, float g, float b, uint16_t* o) {
        r *= inv[0];
        g *= inv[1];
        b *= inv[2];
        if (s.clipHighlights) {
            r = std::min(r, clipF);
            g = std::min(g, clipF);
            b = std::min(b, clipF);
        }
        float R = mf[0] * r + mf[1] * g + mf[2] * b;
        float G = mf[3] * r + mf[4] * g + mf[5] * b;
        float B = mf[6] * r + mf[7] * g + mf[8] * b;
        if (graded) {
            double dr = R, dg = G, db = B;
            applyGrade(s, dr, dg, db);
            R = static_cast<float>(dr); G = static_cast<float>(dg); B = static_cast<float>(db);
        }
        o[0] = lut(R);
        o[1] = lut(G);
        o[2] = lut(B);
    };
    if (half) {
        parallelRows(outH, [&](uint32_t y) {
            for (uint32_t x = 0; x < outW; ++x) {
                double c[3] = {0, 0, 0};
                int n[3] = {0, 0, 0};
                for (int pos = 0; pos < 4; ++pos) {
                    if (colour[pos] < 0) continue;
                    c[colour[pos]] += cfa[size_t(2 * y + (pos >> 1)) * W + 2 * x + (pos & 1)];
                    ++n[colour[pos]];
                }
                finish(c[0] / n[0], c[1] / n[1], c[2] / n[2], &rgb[(size_t(y) * outW + x) * 3]);
            }
        });
    } else {
        // Bilinear: own sample for its colour, mean of same-colour 3x3 neighbours
        // otherwise. Offsets per cell position are precomputed; borders clamp.
        struct Taps { int own; int n[3]; int off[3][8]; };
        Taps taps[4];
        for (int pos = 0; pos < 4; ++pos) {
            Taps& t = taps[pos];
            t.own = colour[pos];
            t.n[0] = t.n[1] = t.n[2] = 0;
            const int py = pos >> 1, px = pos & 1;
            for (int dy = -1; dy <= 1; ++dy)
                for (int dx = -1; dx <= 1; ++dx) {
                    if (!dx && !dy) continue;
                    const int c = colour[((py + dy) & 1) * 2 + ((px + dx) & 1)];
                    t.off[c][t.n[c]++] = dy * static_cast<int>(W) + dx;
                }
        }
        auto pixelAt = [&](uint32_t x, uint32_t y, double v[3]) {
            double sum[3] = {0, 0, 0};
            int n[3] = {0, 0, 0};
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
            for (int c = 0; c < 3; ++c) v[c] = n[c] ? sum[c] / n[c] : 0;
            v[colour[(y & 1) * 2 + (x & 1)]] = cfa[size_t(y) * W + x];
        };
        parallelRows(H, [&](uint32_t y) {
            uint16_t* out = &rgb[size_t(y) * W * 3];
            double v[3];
            if (y == 0 || y + 1 >= H) {
                for (uint32_t x = 0; x < W; ++x) { pixelAt(x, y, v); finish(v[0], v[1], v[2], out + 3 * x); }
                return;
            }
            pixelAt(0, y, v);
            finish(v[0], v[1], v[2], out);
            const float* row = cfa.data() + size_t(y) * W;
            for (uint32_t x = 1; x + 1 < W; ++x) {
                const Taps& t = taps[(y & 1) * 2 + (x & 1)];
                const float* p = row + x;
                for (int c = 0; c < 3; ++c) {
                    if (c == t.own) { v[c] = *p; continue; }
                    float acc = 0;
                    for (int k = 0; k < t.n[c]; ++k) acc += p[t.off[c][k]];
                    v[c] = acc / t.n[c];
                }
                finish(v[0], v[1], v[2], out + 3 * x);
            }
            pixelAt(W - 1, y, v);
            finish(v[0], v[1], v[2], out + 3 * (W - 1));
        });
    }
}

}  // namespace craw::tui
