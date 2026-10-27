// SPDX-License-Identifier: Apache-2.0
//
// Interactive terminal UI: file browser, media pool, metadata, export
// settings and a render queue.
#include "Tui.h"

#include "ClipSummary.h"
#include "Export.h"

#include <ftxui/component/component.hpp>
#include <ftxui/component/screen_interactive.hpp>
#include <ftxui/dom/elements.hpp>

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <filesystem>
#include <mutex>
#include <thread>

namespace fs = std::filesystem;
using namespace ftxui;

namespace craw::tui {
namespace {

struct QueueItem {
    std::string input, name;
    ExportOptions options;
    enum class State { Pending, Running, Done, Failed, Cancelled } state = State::Pending;
    size_t done = 0, total = 0;
    std::string message;
};

const char* stateName(QueueItem::State s) {
    switch (s) {
        case QueueItem::State::Pending: return "pending";
        case QueueItem::State::Running: return "rendering";
        case QueueItem::State::Done: return "done";
        case QueueItem::State::Failed: return "failed";
        default: return "cancelled";
    }
}

template <typename E>
E cycle(E v, int step) {
    const int n = static_cast<int>(E::Count);
    return static_cast<E>(((static_cast<int>(v) + step) % n + n) % n);
}

}  // namespace

int runTui(const std::string& startDir, const std::vector<std::string>& preload, const std::string& outputDir) {
    auto screen = ScreenInteractive::Fullscreen();

    // ---- browser ----
    fs::path cwd = fs::absolute(startDir.empty() ? fs::current_path() : fs::path(startDir));
    std::vector<std::string> browserEntries;
    std::vector<fs::path> browserPaths;
    int browserSel = 0;
    auto refreshBrowser = [&] {
        browserEntries.clear();
        browserPaths.clear();
        browserEntries.push_back("../");
        browserPaths.push_back(cwd.parent_path());
        std::vector<fs::directory_entry> dirs, files;
        std::error_code ec;
        for (const auto& e : fs::directory_iterator(cwd, fs::directory_options::skip_permission_denied, ec)) {
            const auto name = e.path().filename().string();
            if (name.empty() || name[0] == '.') continue;
            if (e.is_directory(ec)) dirs.push_back(e);
            else if (e.path().extension() == ".craw" || e.path().extension() == ".CRAW") files.push_back(e);
        }
        auto byName = [](const auto& a, const auto& b) { return a.path().filename() < b.path().filename(); };
        std::sort(dirs.begin(), dirs.end(), byName);
        std::sort(files.begin(), files.end(), byName);
        for (const auto& d : dirs) { browserEntries.push_back(d.path().filename().string() + "/"); browserPaths.push_back(d.path()); }
        for (const auto& f : files) { browserEntries.push_back(f.path().filename().string()); browserPaths.push_back(f.path()); }
        browserSel = std::min<int>(browserSel, static_cast<int>(browserEntries.size()) - 1);
    };
    refreshBrowser();

    // ---- media pool ----
    std::vector<ClipSummary> pool;
    std::vector<bool> checked;
    std::vector<std::string> poolEntries;
    int poolSel = 0;
    auto refreshPool = [&] {
        poolEntries.clear();
        for (size_t i = 0; i < pool.size(); ++i)
            poolEntries.push_back(std::string(checked[i] ? "[x] " : "[ ] ") + pool[i].name +
                                  (pool[i].error.empty() ? "" : "  (unreadable)"));
    };
    auto import = [&](const std::string& path) {
        for (const auto& p : pool)
            if (p.path == path) return;
        pool.push_back(ClipSummary::load(path));
        checked.push_back(false);
        refreshPool();
        poolSel = static_cast<int>(pool.size()) - 1;
    };
    for (const auto& p : preload) import(fs::absolute(p).string());

    // ---- export settings ----
    const auto profiles = availableProfiles();
    int profileIdx = 0;
    for (size_t i = 0; i < profiles.size(); ++i)
        if (std::string(profiles[i]->id) == "prores4444") profileIdx = static_cast<int>(i);
    DevelopSettings dev;
    std::string outDir = outputDir.empty() ? (cwd / "craw-export").string() : outputDir;
    std::string status = ffmpegAvailable() ? "ready" : "ffmpeg not found: only CinemaDNG export is available";
    bool showBrowser = true, showHelp = false;

    // ---- render queue ----
    std::mutex qm;
    std::vector<QueueItem> queue;
    std::atomic<bool> rendering{false}, cancel{false};
    std::thread worker;
    auto enqueue = [&](const ClipSummary& c) {
        if (!c.error.empty()) return;
        QueueItem it;
        it.input = c.path;
        it.name = c.name;
        it.options.input = c.path;
        it.options.outputDir = outDir;
        it.options.profile = profiles[profileIdx];
        it.options.develop = dev;
        it.total = c.frames;
        std::lock_guard l(qm);
        queue.push_back(it);
    };
    auto startRender = [&] {
        if (rendering) return;
        if (worker.joinable()) worker.join();
        rendering = true;
        cancel = false;
        worker = std::thread([&] {
            for (;;) {
                size_t idx = SIZE_MAX;
                ExportOptions opt;
                {
                    std::lock_guard l(qm);
                    for (size_t i = 0; i < queue.size(); ++i)
                        if (queue[i].state == QueueItem::State::Pending) { idx = i; break; }
                    if (idx == SIZE_MAX || cancel) break;
                    queue[idx].state = QueueItem::State::Running;
                    opt = queue[idx].options;
                }
                screen.PostEvent(Event::Custom);
                const auto res = runExport(opt, [&](size_t done, size_t total) {
                    { std::lock_guard l(qm); queue[idx].done = done; queue[idx].total = total; }
                    screen.PostEvent(Event::Custom);
                }, &cancel);
                {
                    std::lock_guard l(qm);
                    auto& q = queue[idx];
                    q.state = res.ok ? QueueItem::State::Done : cancel ? QueueItem::State::Cancelled : QueueItem::State::Failed;
                    char buf[64];
                    std::snprintf(buf, sizeof buf, " (%.1f s, %.1f fps)", res.seconds, res.seconds > 0 ? res.frames / res.seconds : 0.0);
                    q.message = res.ok ? res.output + buf : res.message;
                }
                screen.PostEvent(Event::Custom);
            }
            rendering = false;
            screen.PostEvent(Event::Custom);
        });
    };

    // ---- components ----
    MenuOption browserOpt;
    browserOpt.on_enter = [&] {
        if (browserSel < 0 || browserSel >= static_cast<int>(browserPaths.size())) return;
        const fs::path p = browserPaths[browserSel];
        std::error_code ec;
        if (fs::is_directory(p, ec)) {
            cwd = p;
            browserSel = 0;
            refreshBrowser();
        } else {
            import(p.string());
            status = "imported " + p.filename().string();
        }
    };
    auto browser = Menu(&browserEntries, &browserSel, browserOpt);
    auto poolMenu = Menu(&poolEntries, &poolSel);
    auto panes = Container::Horizontal({browser, poolMenu});

    auto settingsRows = [&] {
        char ev[32];
        std::snprintf(ev, sizeof ev, "%+.2f EV", dev.exposureEv);
        return vbox({
            hbox({text("codec      [c] "), text(profiles[profileIdx]->label) | bold}),
            hbox({text("gamut      [g] "), text(gamutName(dev.gamut)) | bold}),
            hbox({text("transfer   [t] "), text(transferName(dev.transfer)) | bold}),
            hbox({text("exposure [e/E] "), text(ev) | bold}),
            hbox({text("lens corr  [l] "), text(dev.lensCorrection ? "on" : "off") | bold}),
            hbox({text("highlights [y] "), text(dev.clipHighlights ? "neutral clip" : "raw") | bold}),
            hbox({text("resolution [h] "), text(dev.halfResolution ? "half (2x2 superpixel)" : "full (bilinear)") | bold}),
            hbox({text("output         "), text(outDir) | dim}),
        });
    };

    auto renderer = Renderer(panes, [&] {
        Elements meta;
        if (poolSel >= 0 && poolSel < static_cast<int>(pool.size()))
            for (const auto& [k, v] : pool[poolSel].rows())
                meta.push_back(hbox({text(k) | size(WIDTH, EQUAL, 13) | dim, text(v)}));
        else
            meta.push_back(text("Import .craw takes from the browser (Enter).") | dim);

        Elements q;
        {
            std::lock_guard l(qm);
            if (queue.empty()) q.push_back(text("empty — select takes (Space) and press a / A") | dim);
            for (const auto& it : queue) {
                const float f = it.total ? float(it.done) / it.total : 0.f;
                q.push_back(hbox({text(it.name) | size(WIDTH, EQUAL, 34), text(" " + std::string(it.options.profile->id) + " ") | dim,
                                  gauge(f) | size(WIDTH, EQUAL, 24), text(" " + std::string(stateName(it.state)) + " "),
                                  text(it.message) | dim}));
            }
        }
        auto browserPane = window(text(" Browser: " + cwd.string() + " "), browser->Render() | vscroll_indicator | frame);
        auto poolPane = window(text(" Media pool "), poolMenu->Render() | vscroll_indicator | frame);
        auto right = vbox({window(text(" Clip "), vbox(meta)), window(text(" Export "), settingsRows())});
        Element top = showBrowser ? hbox({browserPane | flex, poolPane | flex, right | size(WIDTH, GREATER_THAN, 52)})
                                  : hbox({poolPane | flex, right | size(WIDTH, GREATER_THAN, 52)});
        Element help = text(
            " Enter open/import  Tab pane  Space select  a add selected  A add all  v render  x cancel  "
            "c/g/t/e/E/l/y/h settings  b browser  ? help  q quit");
        Element main = vbox({text(" Cinematic RAW Studio — craw-tui ") | bold | center, top | flex,
                             window(text(rendering ? " Render queue (rendering) " : " Render queue "), vbox(q)),
                             text(" " + status) | dim, help | dim});
        if (showHelp) {
            auto box = window(text(" Help "), vbox({
                text("Browser: Enter opens folders and imports .craw takes into the media pool."),
                text("Media pool: Space marks takes; a queues marked takes, A queues all."),
                text("v starts rendering the queue, x cancels after the current frame."),
                text("c codec, g gamut, t transfer, e/E exposure -/+ 1/3 EV,"),
                text("l lens-shading correction, y highlight clipping, h half resolution."),
                text("CinemaDNG export is lossless; video codecs are rendered by FFmpeg."),
                text("Settings apply to items when they are queued.  ? closes help."),
            }));
            return dbox({main, box | clear_under | center});
        }
        return main;
    });

    auto app = CatchEvent(renderer, [&](Event e) {
        if (e == Event::Character('q')) {
            cancel = true;
            screen.Exit();
            return true;
        }
        if (e == Event::Character('?')) { showHelp = !showHelp; return true; }
        if (e == Event::Character('b')) { showBrowser = !showBrowser; if (!showBrowser) poolMenu->TakeFocus(); return true; }
        if (e == Event::Character(' ') && poolMenu->Focused() && poolSel < static_cast<int>(pool.size())) {
            checked[poolSel] = !checked[poolSel];
            refreshPool();
            return true;
        }
        if (e == Event::Character('a')) {
            int n = 0;
            for (size_t i = 0; i < pool.size(); ++i)
                if (checked[i]) { enqueue(pool[i]); ++n; }
            if (n == 0 && poolSel < static_cast<int>(pool.size())) { enqueue(pool[poolSel]); n = 1; }
            status = "queued " + std::to_string(n) + " take(s)";
            return true;
        }
        if (e == Event::Character('A')) {
            for (const auto& c : pool) enqueue(c);
            status = "queued all takes";
            return true;
        }
        if (e == Event::Character('v') || e == Event::Character('R')) { startRender(); status = "rendering"; return true; }
        if (e == Event::Character('x')) { cancel = true; status = "cancelling after the current frame"; return true; }
        if (e == Event::Character('c')) { profileIdx = (profileIdx + 1) % static_cast<int>(profiles.size()); return true; }
        if (e == Event::Character('g')) { dev.gamut = cycle(dev.gamut, 1); return true; }
        if (e == Event::Character('t')) { dev.transfer = cycle(dev.transfer, 1); return true; }
        if (e == Event::Character('e')) { dev.exposureEv -= 1.0 / 3; return true; }
        if (e == Event::Character('E')) { dev.exposureEv += 1.0 / 3; return true; }
        if (e == Event::Character('l')) { dev.lensCorrection = !dev.lensCorrection; return true; }
        if (e == Event::Character('y')) { dev.clipHighlights = !dev.clipHighlights; return true; }
        if (e == Event::Character('h')) { dev.halfResolution = !dev.halfResolution; return true; }
        return false;
    });

    screen.Loop(app);
    cancel = true;
    if (worker.joinable()) worker.join();
    return 0;
}

}  // namespace craw::tui
