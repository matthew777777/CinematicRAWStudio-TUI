// SPDX-License-Identifier: Apache-2.0
//
// RAW development for video export: levels, lens shading, white balance,
// demosaic, camera -> XYZ -> target gamut, exposure and transfer function.
#pragma once

#include "common/Clip.h"

#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace craw::tui {

using Mat3 = std::array<double, 9>;  // row-major

enum class Gamut { Rec709, Rec2020, P3D65, AcesAP1, SGamut3Cine, ArriWG3, Count };
enum class Transfer { Rec709, Srgb, Linear, SLog3, LogC3, Hlg, Pq, Count };

const char* gamutName(Gamut g);
const char* transferName(Transfer t);
bool parseGamut(const std::string& s, Gamut& out);
bool parseTransfer(const std::string& s, Transfer& out);

struct DevelopSettings {
    Gamut gamut = Gamut::Rec709;
    Transfer transfer = Transfer::Rec709;
    double exposureEv = 0;
    bool lensCorrection = true;     // apply the recorded lens-shading map
    bool clipHighlights = true;     // neutral (white) clipped highlights instead of colour casts
    bool halfResolution = false;    // 2x2 superpixel instead of bilinear demosaic
    double blackOverride = -1;      // < 0: use recorded levels
    double whiteOverride = -1;
};

// Camera (white-balanced) -> target gamut, from ForwardMatrix1 or ColorMatrix1.
Mat3 cameraToTarget(const tools::ClipInfo& info, const std::array<double, 3>& neutral, Gamut gamut);

double applyTransfer(Transfer t, double linear);

// Develops one frame into interleaved RGB, 16 bits per channel (rgb48le).
// Output is width x height (or half of each with halfResolution).
void develop(const tools::RawFrame& raw, const tools::ClipInfo& info, const DevelopSettings& s,
             std::vector<uint16_t>& rgb, uint32_t& outWidth, uint32_t& outHeight, unsigned threads);

Mat3 multiply(const Mat3& a, const Mat3& b);
Mat3 inverse(const Mat3& m);

}  // namespace craw::tui
