#pragma once

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <mutex>
#include <utility>
#include <variant>
#include <vector>

namespace indy {

// Requests from the UI thread to the machine. The UI never touches System
// directly; it posts commands that the emulation thread applies between slices.
namespace cmd {
struct InsertMedia { std::filesystem::path path; }; // swap CD-ROM image
struct EjectMedia {};
struct ResetSystem {};
// Host keyboard event; `hidUsage` is a USB HID keyboard usage (= SDL scancode).
struct KeyEvent { uint16_t hidUsage; bool pressed; };
// A byte typed on the host terminal, for the guest's serial console.
struct SerialInput { uint8_t byte; };
// Host mouse: PS/2 orientation (dy positive = up), buttons L=1 R=2 M=4.
struct MouseEvent { int16_t dx; int16_t dy; uint8_t buttons; };
} // namespace cmd

using Command = std::variant<cmd::InsertMedia, cmd::EjectMedia, cmd::ResetSystem, cmd::KeyEvent,
                             cmd::SerialInput, cmd::MouseEvent>;

// Multi-producer / single-consumer queue. Commands are rare (user actions), so
// a mutex is fine; the atomic flag lets the hot loop skip locking when empty.
class CommandQueue {
public:
    void push(Command command) {
        {
            std::scoped_lock lock(mutex_);
            pending_.push_back(std::move(command));
        }
        hasPending_.store(true, std::memory_order_release);
    }

    [[nodiscard]] bool hasPending() const noexcept {
        return hasPending_.load(std::memory_order_acquire);
    }

    // Swap queued commands into `out` (which should be empty) to keep the
    // critical section allocation-free.
    void drainInto(std::vector<Command>& out) {
        std::scoped_lock lock(mutex_);
        out.swap(pending_);
        hasPending_.store(false, std::memory_order_relaxed);
    }

private:
    std::mutex mutex_;
    std::vector<Command> pending_;
    std::atomic<bool> hasPending_{false};
};

} // namespace indy
