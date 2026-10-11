#include "HostTerminal.hpp"

#include "indy/core/Log.hpp"

#include <cstdio>
#include <thread>

#if defined(__unix__) || defined(__APPLE__)
#include <termios.h>
#include <unistd.h>
#define INDY_HAVE_TERMIOS 1
#endif

namespace indy::frontend {

struct HostTerminal::SavedState {
#if INDY_HAVE_TERMIOS
    termios attrs{};
#endif
};

HostTerminal::HostTerminal(std::shared_ptr<CommandQueue> commands) : saved_(std::make_unique<SavedState>()) {
#if INDY_HAVE_TERMIOS
    if (isatty(STDIN_FILENO) && tcgetattr(STDIN_FILENO, &saved_->attrs) == 0) {
        termios raw = saved_->attrs;
        raw.c_lflag &= static_cast<tcflag_t>(~(ICANON | ECHO)); // per-key, no local echo; keep ISIG
        raw.c_cc[VMIN]  = 1;
        raw.c_cc[VTIME] = 0;
        rawMode_ = tcsetattr(STDIN_FILENO, TCSANOW, &raw) == 0;
    }
#endif
    if (rawMode_) {
        log::info("Serial console: type in this terminal (Ctrl-C quits the emulator)");
    }

    // A blocking read can't be interrupted portably, so the reader is detached
    // and shares ownership of the queue; it ends by itself when stdin closes.
    std::thread([queue = std::move(commands)] {
        for (int c = std::getchar(); c != EOF; c = std::getchar()) {
            queue->push(cmd::SerialInput{static_cast<uint8_t>(c)});
        }
    }).detach();
}

HostTerminal::~HostTerminal() {
#if INDY_HAVE_TERMIOS
    if (rawMode_) {
        tcsetattr(STDIN_FILENO, TCSANOW, &saved_->attrs);
    }
#endif
}

} // namespace indy::frontend
