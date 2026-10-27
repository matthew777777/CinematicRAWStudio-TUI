// SPDX-License-Identifier: Apache-2.0
//
// Render jobs: develop every frame and pipe rgb48le to FFmpeg, or write a
// CinemaDNG sequence (lossless). Audio comes from the take's AUDI chunks.
#pragma once

#include "Develop.h"

#include <atomic>
#include <functional>
#include <string>
#include <vector>

namespace craw::tui {

struct CodecProfile {
    const char* id;
    const char* label;
    const char* extension;  // "" for CinemaDNG (a folder)
    const char* encoder;    // FFmpeg encoder name, "" for CinemaDNG
    const char* args;       // extra FFmpeg output args
    const char* pixfmt;
    const char* audio;      // FFmpeg audio codec args
};

const std::vector<CodecProfile>& codecProfiles();
const CodecProfile* findProfile(const std::string& id);
// Profiles whose encoder this FFmpeg build provides (CinemaDNG always).
std::vector<const CodecProfile*> availableProfiles();
bool ffmpegAvailable();

struct ExportOptions {
    std::string input;
    std::string outputDir;
    const CodecProfile* profile = nullptr;
    DevelopSettings develop;
    double fpsOverride = 0;  // 0: clip frame rate
    bool audio = true;
    unsigned threads = 0;    // 0: all cores
};

struct ExportResult {
    bool ok = false;
    std::string output, message;
    size_t frames = 0;
    double seconds = 0;
};

// Runs synchronously; `progress(done, total)` is called after each frame.
// Setting `cancel` stops after the current frame.
ExportResult runExport(const ExportOptions& o, const std::function<void(size_t, size_t)>& progress,
                       const std::atomic<bool>* cancel = nullptr);

}  // namespace craw::tui
