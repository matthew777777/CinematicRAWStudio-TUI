// SPDX-License-Identifier: Apache-2.0
#include "Export.h"
#include "Process.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <deque>
#include <filesystem>
#include <future>
#include <memory>
#include <thread>

namespace fs = std::filesystem;

namespace craw::tui {
namespace {

std::string capture(const std::string& cmd) {
    std::string out;
    if (std::FILE* p = process::open(cmd, false)) {
        char buf[4096];
        size_t n;
        while ((n = std::fread(buf, 1, sizeof buf, p)) > 0) out.append(buf, n);
        process::close(p);
    }
    return out;
}

// FFmpeg colour tags for the chosen gamut/transfer; empty when FFmpeg has no name for it.
std::string colourArgs(Gamut g, Transfer t) {
    std::string primaries, trc, matrix = "bt709";
    switch (g) {
        case Gamut::Rec709: primaries = "bt709"; break;
        case Gamut::Rec2020: primaries = "bt2020"; matrix = "bt2020nc"; break;
        case Gamut::P3D65: primaries = "smpte432"; break;
        default: break;  // camera log gamuts: untagged, graded in the NLE
    }
    switch (t) {
        case Transfer::Rec709: trc = "bt709"; break;
        case Transfer::Srgb: trc = "iec61966-2-1"; break;
        case Transfer::Linear: trc = "linear"; break;
        case Transfer::Hlg: trc = "arib-std-b67"; break;
        case Transfer::Pq: trc = "smpte2084"; break;
        default: break;
    }
    // Tag the frames themselves (setparams): encoders take colour metadata from
    // frames, and the scale filter would otherwise emit them untagged.
    std::string params = "colorspace=" + matrix + ":range=tv";
    if (!primaries.empty()) params += ":color_primaries=" + primaries;
    if (!trc.empty()) params += ":color_trc=" + trc;
    std::string a = " -colorspace " + matrix;
    if (!primaries.empty()) a += " -color_primaries " + primaries;
    if (!trc.empty()) a += " -color_trc " + trc;
    return a + " -vf scale=out_color_matrix=" + matrix + ":out_range=tv,setparams=" + params;
}

}  // namespace

const std::vector<CodecProfile>& codecProfiles() {
    static const std::vector<CodecProfile> p = {
        {"prores4444", "ProRes 4444", ".mov", "prores_ks", "-profile:v 4 -vendor apl0", "yuv444p10le", "-c:a pcm_s24le"},
        {"prores422hq", "ProRes 422 HQ", ".mov", "prores_ks", "-profile:v 3 -vendor apl0", "yuv422p10le", "-c:a pcm_s24le"},
        {"dnxhr_hqx", "DNxHR HQX 10-bit", ".mov", "dnxhd", "-profile:v dnxhr_hqx", "yuv422p10le", "-c:a pcm_s24le"},
        {"hevc10", "HEVC 10-bit (x265)", ".mov", "libx265", "-crf 14 -preset medium -tag:v hvc1", "yuv420p10le", "-c:a aac -b:a 320k"},
        {"hevc10_vt", "HEVC 10-bit (VideoToolbox)", ".mov", "hevc_videotoolbox", "-profile:v main10 -q:v 75 -tag:v hvc1", "p010le", "-c:a aac -b:a 320k"},
        {"h264", "H.264 8-bit (x264)", ".mp4", "libx264", "-crf 14 -preset medium", "yuv420p", "-c:a aac -b:a 320k"},
        {"h264_vt", "H.264 (VideoToolbox)", ".mp4", "h264_videotoolbox", "-q:v 75", "yuv420p", "-c:a aac -b:a 320k"},
        {"av1", "AV1 10-bit (SVT-AV1)", ".mp4", "libsvtav1", "-crf 22 -preset 6", "yuv420p10le", "-c:a aac -b:a 320k"},
        {"cinemadng", "CinemaDNG (lossless)", "", "", "", "", ""},
    };
    return p;
}

const CodecProfile* findProfile(const std::string& id) {
    for (const auto& p : codecProfiles())
        if (id == p.id) return &p;
    return nullptr;
}

bool ffmpegAvailable() { return capture("ffmpeg -version").find("ffmpeg version") != std::string::npos; }

std::vector<const CodecProfile*> availableProfiles() {
    static const std::string encoders = ffmpegAvailable() ? capture("ffmpeg -hide_banner -encoders") : "";
    std::vector<const CodecProfile*> out;
    for (const auto& p : codecProfiles())
        if (!*p.encoder || encoders.find(std::string(" ") + p.encoder + " ") != std::string::npos) out.push_back(&p);
    return out;
}

ExportResult runExport(const ExportOptions& o, const std::function<void(size_t, size_t)>& progress,
                       const std::atomic<bool>* cancel) {
    ExportResult r;
    const auto t0 = std::chrono::steady_clock::now();
    const unsigned threads = o.threads ? o.threads : std::max(1u, std::thread::hardware_concurrency());
    try {
        if (!o.profile) throw std::runtime_error("no codec profile");
        const tools::Clip clip(o.input);
        const std::string stem = fs::path(o.input).stem().string();
        fs::create_directories(o.outputDir);
        const size_t total = clip.frameCount();
        const double fps = o.fpsOverride > 0 ? o.fpsOverride : clip.info().fpsNum / clip.info().fpsDen;

        if (!*o.profile->encoder) {  // CinemaDNG
            const fs::path dir = fs::path(o.outputDir) / stem;
            fs::create_directories(dir);
            for (size_t i = 0; i < total; ++i) {
                if (cancel && *cancel) throw std::runtime_error("cancelled");
                const auto dng = clip.frameDng(i, 0, threads);
                char name[32];
                std::snprintf(name, sizeof name, "_%06zu.dng", i);
                std::FILE* f = std::fopen((dir / (stem + name)).string().c_str(), "wb");
                if (!f || std::fwrite(dng.data(), 1, dng.size(), f) != dng.size()) {
                    if (f) std::fclose(f);
                    throw std::runtime_error("write failed");
                }
                std::fclose(f);
                ++r.frames;
                progress(r.frames, total);
            }
            if (o.audio && clip.audio().format) {
                std::vector<uint8_t> wav(clip.wavSize());
                clip.readWav(0, wav.data(), wav.size());
                std::FILE* f = std::fopen((dir / (stem + ".wav")).string().c_str(), "wb");
                if (f) { std::fwrite(wav.data(), 1, wav.size(), f); std::fclose(f); }
            }
            r.output = dir.string();
        } else {
            const auto [w, h] = clip.frameSize();
            const bool half = o.develop.halfResolution || clip.isRgb();  // RGB frames develop at picture size
            const uint32_t ow = half ? w / 2 : w, oh = half ? h / 2 : h;
            const fs::path out = fs::path(o.outputDir) /
                                 (stem + "_" + o.profile->id + "_" + transferName(o.develop.transfer) + o.profile->extension);
            std::string wavPath;
            if (o.audio && clip.audio().format) {
                wavPath = (fs::path(o.outputDir) / ("." + stem + ".tmp.wav")).string();
                std::vector<uint8_t> wav(clip.wavSize());
                clip.readWav(0, wav.data(), wav.size());
                std::FILE* f = std::fopen(wavPath.c_str(), "wb");
                if (!f || std::fwrite(wav.data(), 1, wav.size(), f) != wav.size()) wavPath.clear();
                if (f) std::fclose(f);
            }
            char rate[32];
            std::snprintf(rate, sizeof rate, "%.6g", fps);
            std::string cmd = "ffmpeg -hide_banner -loglevel error -y -f rawvideo -pix_fmt rgb48le -s " +
                              std::to_string(ow) + "x" + std::to_string(oh) + " -r " + rate + " -i -";
            if (!wavPath.empty()) cmd += " -i " + process::quote(wavPath) + " -map 0:v -map 1:a " + o.profile->audio;
            cmd += std::string(" -c:v ") + o.profile->encoder + " " + o.profile->args + " -pix_fmt " + o.profile->pixfmt +
                   colourArgs(o.develop.gamut, o.develop.transfer) + " " + process::quote(out.string());
            const std::string log = (fs::path(o.outputDir) / ("." + stem + ".ffmpeg.log")).string();
            std::FILE* pipe = process::open(cmd, true, log);
            if (!pipe) throw std::runtime_error("cannot start ffmpeg");
            // Pipeline: `window` frames decode + develop concurrently (each with a
            // share of the cores) while frames are written to FFmpeg in order.
            const unsigned window = std::clamp(threads / 2, 1u, 4u);
            const unsigned perFrame = std::max(1u, threads / window);
            struct Developed { std::vector<uint16_t> rgb; uint32_t w = 0, h = 0; };
            auto job = [&](size_t i) {
                Developed d;
                const auto raw = clip.frameRaw(i, perFrame);
                develop(raw, clip.info(), o.develop, d.rgb, d.w, d.h, perFrame);
                return d;
            };
            std::deque<std::future<Developed>> inflight;
            size_t next = 0;
            bool broken = false;
            try {
                while (!broken && (next < total || !inflight.empty())) {
                    while (next < total && inflight.size() < window && !(cancel && *cancel))
                        inflight.push_back(std::async(std::launch::async, job, next++));
                    if (inflight.empty()) break;
                    Developed d = inflight.front().get();
                    inflight.pop_front();
                    if (d.w != ow || d.h != oh) throw std::runtime_error("frame size changed mid-take");
                    broken = std::fwrite(d.rgb.data(), 2, d.rgb.size(), pipe) != d.rgb.size();
                    if (!broken) progress(++r.frames, total);
                    if (cancel && *cancel) broken = true;
                }
            } catch (...) {
                for (auto& f : inflight) if (f.valid()) f.wait();
                process::close(pipe);
                if (!wavPath.empty()) fs::remove(wavPath);
                throw;
            }
            for (auto& f : inflight) if (f.valid()) f.wait();
            const int status = process::close(pipe);
            if (!wavPath.empty()) fs::remove(wavPath);
            if (cancel && *cancel) throw std::runtime_error("cancelled");
            if (broken || status != 0) {
                std::string msg = "ffmpeg failed";
                if (std::FILE* f = std::fopen(log.c_str(), "r")) {
                    char line[512];
                    if (std::fgets(line, sizeof line, f)) msg += std::string(": ") + line;
                    std::fclose(f);
                }
                throw std::runtime_error(msg);
            }
            fs::remove(log);
            r.output = out.string();
        }
        r.ok = true;
        r.message = "done";
    } catch (const std::exception& e) {
        r.message = e.what();
    }
    r.seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    return r;
}

}  // namespace craw::tui
