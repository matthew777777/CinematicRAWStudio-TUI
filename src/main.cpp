// SPDX-License-Identifier: Apache-2.0
//
// craw-tui                                   interactive UI in the current folder
// craw-tui [-d DIR] [-o OUTDIR] [take.craw ...]
// craw-tui info -f take.craw
// craw-tui export -f take.craw [-F codec] [-g gamut] [-t transfer] [-e EV]
//                 [--half] [--no-lens] [--raw-highlights] [--fps N] [--no-audio] [-o OUTDIR]
// craw-tui profiles
#include "ClipSummary.h"
#include "Export.h"
#include "Tui.h"

#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

using namespace craw::tui;

namespace {

int usage() {
    std::fprintf(stderr,
                 "usage:\n"
                 "  craw-tui [-d DIR] [-o OUTDIR] [take.craw ...]\n"
                 "  craw-tui info -f take.craw\n"
                 "  craw-tui export -f take.craw [-F codec] [-g gamut] [-t transfer] [-e EV] [--half]\n"
                 "                  [--no-lens] [--raw-highlights] [--fps N] [--no-audio] [-o OUTDIR]\n"
                 "  craw-tui profiles\n");
    return 2;
}

}  // namespace

int main(int argc, char** argv) {
    std::vector<std::string> args(argv + 1, argv + argc);
    const std::string cmd = args.empty() ? "" : args[0];

    if (cmd == "profiles") {
        std::printf("codecs:");
        for (const auto* p : availableProfiles()) std::printf(" %s", p->id);
        std::printf("\ngamuts:");
        for (int i = 0; i < static_cast<int>(Gamut::Count); ++i) std::printf(" %s", gamutName(static_cast<Gamut>(i)));
        std::printf("\ntransfers:");
        for (int i = 0; i < static_cast<int>(Transfer::Count); ++i) std::printf(" %s", transferName(static_cast<Transfer>(i)));
        std::printf("\n");
        return 0;
    }

    if (cmd == "info" || cmd == "export") {
        ExportOptions o;
        o.outputDir = "craw-export";
        std::string profile = "prores4444";
        for (size_t i = 1; i < args.size(); ++i) {
            const std::string& a = args[i];
            auto next = [&]() -> std::string { return i + 1 < args.size() ? args[++i] : std::string(); };
            if (a == "-f") o.input = next();
            else if (a == "-o") o.outputDir = next();
            else if (a == "-F") profile = next();
            else if (a == "-g") { if (!parseGamut(next(), o.develop.gamut)) return usage(); }
            else if (a == "-t") { if (!parseTransfer(next(), o.develop.transfer)) return usage(); }
            else if (a == "-e") o.develop.exposureEv = std::stod(next());
            else if (a == "--fps") o.fpsOverride = std::stod(next());
            else if (a == "--half") o.develop.halfResolution = true;
            else if (a == "--no-lens") o.develop.lensCorrection = false;
            else if (a == "--raw-highlights") o.develop.clipHighlights = false;
            else if (a == "--no-audio") o.audio = false;
            else return usage();
        }
        if (o.input.empty()) return usage();
        if (cmd == "info") {
            const auto s = ClipSummary::load(o.input);
            for (const auto& [k, v] : s.rows()) std::printf("%-13s %s\n", k.c_str(), v.c_str());
            return s.error.empty() ? 0 : 1;
        }
        o.profile = findProfile(profile);
        bool available = false;
        for (const auto* p : availableProfiles()) available |= p == o.profile;
        if (!o.profile || !available) {
            std::fprintf(stderr, "codec '%s' unavailable (see `craw-tui profiles`)\n", profile.c_str());
            return 2;
        }
        size_t last = 0;
        const auto r = runExport(o, [&](size_t done, size_t total) {
            if (done == total || done - last >= 10) {
                std::fprintf(stderr, "\r%zu/%zu frames", done, total);
                last = done;
            }
        });
        std::fprintf(stderr, "\n");
        std::printf("{\"ok\":%s,\"output\":\"%s\",\"frames\":%zu,\"seconds\":%.2f,\"message\":\"%s\"}\n", r.ok ? "true" : "false",
                    r.output.c_str(), r.frames, r.seconds, r.message.c_str());
        return r.ok ? 0 : 1;
    }

    std::string dir, out;
    std::vector<std::string> preload;
    for (size_t i = 0; i < args.size(); ++i) {
        if (args[i] == "-d" && i + 1 < args.size()) dir = args[++i];
        else if (args[i] == "-o" && i + 1 < args.size()) out = args[++i];
        else if (args[i] == "-f" && i + 1 < args.size()) preload.push_back(args[++i]);
        else if (!args[i].empty() && args[i][0] != '-') preload.push_back(args[i]);
        else return usage();
    }
    if (dir.empty() && !preload.empty()) dir = std::filesystem::absolute(preload[0]).parent_path().string();
    return runTui(dir, preload, out);
}
