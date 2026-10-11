#pragma once

#include "indy/core/Commands.hpp"

#include <memory>

namespace indy::frontend {

// Bridges the host terminal to the guest's serial console.
//
// A detached thread reads stdin byte by byte and posts cmd::SerialInput; the
// emulation thread hands each byte to the Z85C30, whose RR0 "Rx character
// available" bit and data register the PROM polls. The CommandQueue is the
// only shared state, so the per-instruction path never takes a lock.
//
// When stdin is a terminal it is switched to non-canonical, no-echo mode for
// the emulator's lifetime: keys arrive immediately ("press any key" works) and
// only the guest's own echo is shown. Ctrl-C still quits the emulator (SDL turns
// SIGINT into a quit event), and the terminal is restored on destruction.
class HostTerminal {
public:
    explicit HostTerminal(std::shared_ptr<CommandQueue> commands);
    ~HostTerminal();
    HostTerminal(const HostTerminal&) = delete;
    HostTerminal& operator=(const HostTerminal&) = delete;

    [[nodiscard]] bool interactive() const noexcept { return rawMode_; }

private:
    bool rawMode_ = false;
    struct SavedState;
    std::unique_ptr<SavedState> saved_;
};

} // namespace indy::frontend
