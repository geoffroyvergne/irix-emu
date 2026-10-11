#include "indy/core/EmulationThread.hpp"

#include "indy/core/Log.hpp"

#include <chrono>
#include <exception>
#include <vector>

namespace indy {

namespace {
// Emulated time per slice. Short slices bound input/command latency to ~1 ms
// while still amortising loop overhead over ~100k CPU cycles.
constexpr auto kSlice = std::chrono::milliseconds(1);
// If the host falls this far behind real time, resynchronise rather than
// trying to "catch up" in a burst.
constexpr auto kMaxLag = std::chrono::milliseconds(50);
} // namespace

EmulationThread::EmulationThread(std::unique_ptr<System> system, CommandQueue& commands, Options options)
    : system_(std::move(system)),
      commands_(commands),
      options_(options),
      clockHz_(system_->config().cpuClockHz) {}

EmulationThread::~EmulationThread() {
    stop();
}

void EmulationThread::start() {
    if (thread_.joinable()) {
        return;
    }
    running_.store(true, std::memory_order_release);
    thread_ = std::jthread([this](std::stop_token stop) { loop(stop); });
}

void EmulationThread::stop() {
    if (thread_.joinable()) {
        thread_.request_stop();
        thread_.join();
    }
}

void EmulationThread::loop(std::stop_token stop) {
    using Clock = std::chrono::steady_clock;

    const auto sliceCycles = static_cast<Cycles>(
        clockHz_ * static_cast<uint64_t>(kSlice.count()) / 1000);
    std::vector<Command> pending;
    auto deadline = Clock::now();

    try {
        while (!stop.stop_requested()) {
            if (commands_.hasPending()) {
                commands_.drainInto(pending);
                for (const Command& command : pending) {
                    system_->handle(command);
                }
                pending.clear();
            }

            system_->runFor(sliceCycles);
            cycles_.store(system_->now(), std::memory_order_relaxed);

            if (options_.throttle) {
                deadline += kSlice;
                const auto now = Clock::now();
                if (now - deadline > kMaxLag) {
                    deadline = now;
                } else {
                    std::this_thread::sleep_until(deadline);
                }
            }
        }
    } catch (const std::exception& e) {
        log::error("Emulation thread terminated: {}", e.what());
    }
    log::info("Emulation stopped at cycle {}, CPU PC {:#018x}", system_->now(), system_->cpu().pc());
    running_.store(false, std::memory_order_release);
}

} // namespace indy
