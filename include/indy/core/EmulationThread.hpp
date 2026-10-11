#pragma once

#include "indy/core/Commands.hpp"
#include "indy/core/System.hpp"

#include <atomic>
#include <memory>
#include <thread>

namespace indy {

// Runs the System on its own thread so the UI never stalls emulation and vice
// versa. Takes ownership of the System: from start() on, nothing else touches
// it. The UI talks to it only via the CommandQueue (in) and FrameBuffer (out).
class EmulationThread {
public:
    struct Options {
        bool throttle = true; // pace to real time; false = run as fast as possible
    };

    EmulationThread(std::unique_ptr<System> system, CommandQueue& commands, Options options);
    ~EmulationThread();
    EmulationThread(const EmulationThread&) = delete;
    EmulationThread& operator=(const EmulationThread&) = delete;

    void start();
    void stop();

    [[nodiscard]] bool running() const noexcept { return running_.load(std::memory_order_acquire); }
    // Total emulated cycles so far; readable from any thread (speed meter).
    [[nodiscard]] uint64_t emulatedCycles() const noexcept { return cycles_.load(std::memory_order_relaxed); }
    [[nodiscard]] uint64_t clockHz() const noexcept { return clockHz_; }

private:
    void loop(std::stop_token stop);

    std::unique_ptr<System> system_;
    CommandQueue& commands_;
    Options options_;
    uint64_t clockHz_;

    std::atomic<uint64_t> cycles_{0};
    std::atomic<bool> running_{false};
    std::jthread thread_; // last: joined before the members above are destroyed
};

} // namespace indy
