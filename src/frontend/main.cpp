// SGI Indy emulator - SDL3 desktop entry point.
//
// Threading model:
//   main thread       : SDL events, rendering, presentation (SdlFrontend)
//   emulation thread  : System (CPU + bus + devices), paced to real time
//   UI -> emu         : CommandQueue (media swaps, reset)
//   emu -> UI         : FrameBuffer (lock-free triple buffer)

#include "HostTerminal.hpp"
#include "SdlFrontend.hpp"

#include "indy/core/EmulationThread.hpp"
#include "indy/core/Log.hpp"
#include "indy/core/System.hpp"
#include "indy/hpc/SgiNvram.hpp"
#include "indy/input/TextKeys.hpp"

#include <SDL3/SDL_main.h>

#include <algorithm>
#include <charconv>
#include <csignal>
#include <cstdlib>
#include <chrono>
#include <exception>
#include <expected>
#include <format>
#include <memory>
#include <utility>
#include <vector>
#include <string_view>

namespace {

using namespace indy;

struct Options {
    SystemConfig system;
    bool throttle = true;
    bool vsync = true;
    std::string screenshot; // write the last frame here (PPM) on exit
    bool clockGiven = false; // --mhz seen (otherwise the CPU model picks the clock)
    KeyLayout keyLayout = KeyLayout::Us; // --keymap (or INDY_KEYMAP): IRIX's layout, for --type and paste
    std::vector<std::pair<double, std::string>> timedScreenshots; // --screenshot-at <sec>:<file>
    // --type (repeatable): at <seconds>, text sent through the PS/2 keyboard.
    std::vector<std::pair<double, std::string>> typing;
    // --mouse (repeatable): at <seconds>, PS/2 deltas (+y up) and buttons.
    std::vector<std::pair<double, cmd::MouseEvent>> mouseEvents;
};

// SIGUSR1 asks for a screenshot of the running emulator ($INDY_SNAPSHOT, or
// indy-snapshot.ppm in the current directory): kill -USR1 <pid>.
volatile std::sig_atomic_t g_snapshotRequested = 0;

void requestSnapshot(int) { g_snapshotRequested = 1; }

// Save the newest frame as a binary PPM (handy for headless checks).
void writeScreenshot(FrameBuffer& frame, const std::string& path) {
    (void)frame.acquire();
    std::FILE* f = std::fopen(path.c_str(), "wb");
    if (f == nullptr) {
        log::error("Cannot write screenshot '{}'", path);
        return;
    }
    std::fprintf(f, "P6\n%u %u\n255\n", frame.width(), frame.height());
    for (uint32_t px : frame.frontBuffer()) {
        const unsigned char rgb[3] = {static_cast<unsigned char>(px >> 16), static_cast<unsigned char>(px >> 8),
                                      static_cast<unsigned char>(px)};
        std::fwrite(rgb, 1, 3, f);
    }
    std::fclose(f);
    log::info("Screenshot saved to {}", path);
}

constexpr std::string_view kUsage =
    "usage: indy <prom.bin> [--cdrom <file.iso>] [--disk <image> ...] [--disk-size <MiB>] [--nvram <file>]\n"
    "            [--mac <xx:xx:xx:xx:xx:xx>] [--cpu r5000|r4600] [--mhz <MHz>] [--ram <MiB>] [--trace <N>]\n"
    "            [--no-gfx] [--screenshot <file.ppm>] [--screenshot-at <sec>:<file.ppm>] [--type <sec>:<text>] [--mouse <sec>:<dx>,<dy>[,<btn>]]\n"
    "            [--keymap us|fr]\n"
    "            [--unthrottled] [--no-vsync]\n"
    "       Type into this terminal to reach the serial console; the window feeds the PS/2 keyboard.\n"
    "       (the PROM may also be given with --prom or the INDY_PROM environment variable)";

std::expected<uint64_t, std::string> parseUnsigned(std::string_view option, std::string_view text) {
    uint64_t value = 0;
    const auto [end, ec] = std::from_chars(text.data(), text.data() + text.size(), value);
    if (ec != std::errc{} || end != text.data() + text.size()) {
        return std::unexpected(std::format("invalid {} value '{}'", option, text));
    }
    return value;
}

std::expected<Options, std::string> parseArgs(int argc, char* argv[]) {
    Options options;
    if (const char* env = std::getenv("INDY_KEYMAP"); env != nullptr && *env != '\0') { // default for --keymap
        const auto layout = keyLayoutFromName(env);
        if (!layout) return std::unexpected(std::format("invalid INDY_KEYMAP '{}' (us or fr)", env));
        options.keyLayout = *layout;
    }
    for (int i = 1; i < argc; ++i) {
        const std::string_view arg = argv[i];
        auto value = [&]() -> std::expected<std::string_view, std::string> {
            if (i + 1 >= argc) {
                return std::unexpected(std::format("{} requires a value", arg));
            }
            return std::string_view(argv[++i]);
        };

        if (arg == "--disk") { // repeatable: SCSI IDs 1, 2, 3, 5, 6, 7 in order
            auto v = value();
            if (!v) return std::unexpected(v.error());
            options.system.diskImages.emplace_back(*v);
        } else if (arg == "--prom" || arg == "--cdrom" || arg == "--nvram") {
            auto v = value();
            if (!v) return std::unexpected(v.error());
            (arg == "--prom"    ? options.system.promImage
             : arg == "--cdrom" ? options.system.cdromImage
                                : options.system.nvramFile) = *v;
        } else if (arg == "--disk-size") {
            auto v = value();
            if (!v) return std::unexpected(v.error());
            auto mib = parseUnsigned(arg, *v);
            if (!mib || *mib == 0) return std::unexpected(std::format("invalid --disk-size value '{}'", *v));
            options.system.diskCreateBytes = *mib * MiB;
        } else if (arg == "--mac") {
            auto v = value();
            if (!v) return std::unexpected(v.error());
            auto mac = sgi_nvram::parseMac(*v);
            if (!mac) return std::unexpected(std::format("invalid --mac value '{}' (expected xx:xx:xx:xx:xx:xx)", *v));
            options.system.macAddress = *mac;
        } else if (arg == "--cpu") {
            auto v = value();
            if (!v) return std::unexpected(v.error());
            if (*v == "r4600") {
                options.system.cpuModel = CpuModel::R4600;
                if (!options.clockGiven) options.system.cpuClockHz = 133'000'000; // the R4600 Indy's clock
            } else if (*v == "r5000") {
                options.system.cpuModel = CpuModel::R5000;
            } else {
                return std::unexpected(std::format("invalid --cpu value '{}' (r4600 or r5000)", *v));
            }
        } else if (arg == "--mhz") {
            auto v = value();
            if (!v) return std::unexpected(v.error());
            auto mhz = parseUnsigned(arg, *v);
            if (!mhz || *mhz < 50 || *mhz > 400) return std::unexpected(std::format("invalid --mhz value '{}'", *v));
            options.system.cpuClockHz = *mhz * 1'000'000;
            options.clockGiven = true;
        } else if (arg == "--ram") {
            auto v = value();
            if (!v) return std::unexpected(v.error());
            auto mib = parseUnsigned(arg, *v);
            if (!mib || *mib == 0) return std::unexpected(std::format("invalid --ram value '{}'", *v));
            options.system.ramBytes = *mib * MiB;
        } else if (arg == "--trace") {
            auto v = value();
            if (!v) return std::unexpected(v.error());
            auto count = parseUnsigned(arg, *v);
            if (!count) return std::unexpected(count.error());
            options.system.traceInstructions = *count;
        } else if (arg == "--keymap") { // the layout IRIX uses: us (default) or fr
            auto v = value();
            if (!v) return std::unexpected(v.error());
            const auto layout = keyLayoutFromName(*v);
            if (!layout) return std::unexpected(std::format("invalid --keymap '{}' (us or fr)", *v));
            options.keyLayout = *layout;
        } else if (arg == "--type") { // "<seconds>:<text>", \r and \n escapes allowed
            auto v = value();
            if (!v) return std::unexpected(v.error());
            const auto colon = v->find(':');
            if (colon == std::string_view::npos) return std::unexpected(std::string("--type expects <seconds>:<text>"));
            std::string text;
            for (size_t k = colon + 1; k < v->size(); ++k) {
                if ((*v)[k] == '\\' && k + 1 < v->size() && ((*v)[k + 1] == 'r' || (*v)[k + 1] == 'n')) {
                    text.push_back('\r');
                    ++k;
                } else {
                    text.push_back((*v)[k]);
                }
            }
            options.typing.emplace_back(std::stod(std::string(v->substr(0, colon))), std::move(text));
            std::ranges::sort(options.typing);
        } else if (arg == "--mouse") { // "<seconds>:<dx>,<dy>[,<buttons>]" (PS/2: +dy = up)
            auto v = value();
            if (!v) return std::unexpected(v.error());
            double at = 0;
            int dx = 0, dy = 0, buttons = 0;
            if (std::sscanf(std::string(*v).c_str(), "%lf:%d,%d,%d", &at, &dx, &dy, &buttons) < 3) {
                return std::unexpected(std::string("--mouse expects <seconds>:<dx>,<dy>[,<buttons>]"));
            }
            options.mouseEvents.emplace_back(
                at, cmd::MouseEvent{static_cast<int16_t>(dx), static_cast<int16_t>(dy), static_cast<uint8_t>(buttons)});
        } else if (arg == "--no-gfx") {
            options.system.graphics = false;
        } else if (arg == "--screenshot") {
            auto v = value();
            if (!v) return std::unexpected(v.error());
            options.screenshot = std::string(*v);
        } else if (arg == "--screenshot-at") { // "<seconds>:<file.ppm>", repeatable
            auto v = value();
            if (!v) return std::unexpected(v.error());
            const auto colon = v->find(':');
            if (colon == std::string_view::npos) {
                return std::unexpected(std::string("--screenshot-at expects <seconds>:<file.ppm>"));
            }
            options.timedScreenshots.emplace_back(std::stod(std::string(v->substr(0, colon))),
                                                  std::string(v->substr(colon + 1)));
            std::ranges::sort(options.timedScreenshots);
        } else if (arg == "--unthrottled") {
            options.throttle = false;
        } else if (arg == "--no-vsync") {
            options.vsync = false;
        } else if (arg == "--help" || arg == "-h") {
            return std::unexpected(std::string(kUsage));
        } else if (!arg.starts_with('-') && options.system.promImage.empty()) {
            options.system.promImage = arg; // positional: PROM dump path
        } else {
            return std::unexpected(std::format("unknown option '{}'\n{}", arg, kUsage));
        }
    }
    if (options.system.promImage.empty()) {
        if (const char* env = std::getenv("INDY_PROM"); env != nullptr && *env != '\0') {
            options.system.promImage = env;
        } else {
            // Without a PROM nothing is mapped at the reset vector and the CPU
            // can only halt on its first fetch.
            return std::unexpected(std::format("no PROM image given\n{}", kUsage));
        }
    }
    return options;
}

int run(const Options& options) {
    using Clock = std::chrono::steady_clock;

    const auto& sys = options.system;

    // Machine first: a bad PROM path or RAM size fails before any window appears.
    FrameBuffer display(sys.displayWidth, sys.displayHeight);
    auto commandQueue = std::make_shared<CommandQueue>();
    CommandQueue& commands = *commandQueue;
    auto machine = std::make_unique<System>(sys, display);

    // Host terminal <-> guest serial console (raw mode while we run).
    frontend::HostTerminal terminal(commandQueue);

    frontend::SdlFrontend ui({
        .title  = "SGI Indy",
        .width  = static_cast<int>(sys.displayWidth),
        .height = static_cast<int>(sys.displayHeight),
        .vsync  = options.vsync,
        .keyLayout = options.keyLayout,
    });

    EmulationThread emulator(std::move(machine), commands,
                             {.throttle = options.throttle});
    emulator.start();

    // Fallback pacing when the driver refuses vsync.
    const auto frameInterval = std::chrono::nanoseconds(
        static_cast<int64_t>(1e9 / sys.refreshHz));
    auto nextFrame = Clock::now();

    const auto start = Clock::now();
    const bool logSpeed = std::getenv("INDY_LOG_SPEED") != nullptr; // speed meter also on stderr
    size_t nextTyping = 0;
    size_t nextMouse = 0;
    auto lastStats = Clock::now();
    uint64_t lastCycles = 0;
    size_t nextShot = 0;

    std::signal(SIGUSR1, requestSnapshot);
    while (ui.pumpEvents(commands) && emulator.running()) {
        ui.present(display);
        if (g_snapshotRequested != 0) {
            g_snapshotRequested = 0;
            const char* path = std::getenv("INDY_SNAPSHOT");
            writeScreenshot(display, path != nullptr ? path : "indy-snapshot.ppm");
        }

        const double elapsed = std::chrono::duration<double>(Clock::now() - start).count();
        while (nextShot < options.timedScreenshots.size() && elapsed >= options.timedScreenshots[nextShot].first) {
            writeScreenshot(display, options.timedScreenshots[nextShot++].second);
        }
        while (nextMouse < options.mouseEvents.size() && elapsed >= options.mouseEvents[nextMouse].first) {
            commands.push(options.mouseEvents[nextMouse++].second);
        }
        while (nextTyping < options.typing.size() && elapsed >= options.typing[nextTyping].first) {
            for (const cmd::KeyEvent& key : keyEventsForText(options.typing[nextTyping++].second, options.keyLayout).events) {
                commands.push(key);
            }
        }

        // Speed meter in the title bar, refreshed once per second.
        const auto now = Clock::now();
        if (now - lastStats >= std::chrono::seconds(1)) {
            const uint64_t cycles = emulator.emulatedCycles();
            const double seconds = std::chrono::duration<double>(now - lastStats).count();
            const double speed = static_cast<double>(cycles - lastCycles) /
                                 (static_cast<double>(emulator.clockHz()) * seconds) * 100.0;
            if (logSpeed) {
                log::tagged("SPEED", "{:.1f}% at {:.0f} s", speed,
                            std::chrono::duration<double>(now - start).count());
            }
            ui.setTitle(std::format("SGI Indy - {:.1f}% speed - {}", speed,
                                    ui.mouseCaptured() ? "Ctrl+Alt+G releases the mouse"
                                                       : "click to capture the mouse"));
            lastCycles = cycles;
            lastStats = now;
        }

        if (!ui.vsyncActive()) {
            nextFrame += frameInterval;
            const auto remaining = nextFrame - Clock::now();
            if (remaining > std::chrono::nanoseconds::zero()) {
                SDL_DelayPrecise(static_cast<Uint64>(remaining.count()));
            } else {
                nextFrame = Clock::now();
            }
        }
    }

    log::info("Shutting down");
    emulator.stop(); // joins the emulation thread before display/commands go away
    if (!options.screenshot.empty()) {
        writeScreenshot(display, options.screenshot);
    }
    return 0;
}

} // namespace

int main(int argc, char* argv[]) {
    auto options = parseArgs(argc, argv);
    if (!options) {
        indy::log::error("{}", options.error());
        return 2;
    }
    try {
        return run(*options);
    } catch (const std::exception& e) {
        indy::log::error("Fatal: {}", e.what());
        return 1;
    }
}
