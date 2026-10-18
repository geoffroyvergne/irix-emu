#pragma once

#include "indy/core/Log.hpp"

#include <chrono>
#include <cstdint>
#include <cstdlib>

namespace indy {

// INDY_TRACE_DISK=1: every 5 seconds, how much host time disk I/O took on
// the emulation thread (image reads and writes, the slowest single one) and
// how long the DMA copies to and from guest memory took. Emulation thread
// only, so plain counters.
class DiskStats {
public:
    using Clock = std::chrono::steady_clock;

    [[nodiscard]] static bool enabled() noexcept {
        static const bool on = std::getenv("INDY_TRACE_DISK") != nullptr;
        return on;
    }
    static DiskStats& get() noexcept {
        static DiskStats stats;
        return stats;
    }

    void image(Clock::time_point start, uint64_t bytes, bool write) noexcept {
        const auto ns = static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - start).count());
        (write ? writeNs_ : readNs_) += ns;
        (write ? writeBytes_ : readBytes_) += bytes;
        ++commands_;
        if (ns > slowestNs_) slowestNs_ = ns;
        report();
    }
    void dma(Clock::time_point start, uint64_t bytes) noexcept {
        dmaNs_ += static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - start).count());
        dmaBytes_ += bytes;
    }

private:
    static double ms(uint64_t ns) noexcept { return static_cast<double>(ns) / 1e6; }

    void report() {
        const auto now = Clock::now();
        if (now - last_ < std::chrono::seconds(5)) return;
        log::tagged("DISK", "{} commands: read {} KiB in {:.1f} ms, write {} KiB in {:.1f} ms, slowest {:.2f} ms; "
                            "DMA {} KiB in {:.1f} ms",
                    commands_, readBytes_ / 1024, ms(readNs_), writeBytes_ / 1024, ms(writeNs_), ms(slowestNs_),
                    dmaBytes_ / 1024, ms(dmaNs_));
        *this = DiskStats{};
        last_ = now;
    }

    Clock::time_point last_ = Clock::now();
    uint64_t commands_ = 0, readBytes_ = 0, writeBytes_ = 0, readNs_ = 0, writeNs_ = 0, slowestNs_ = 0;
    uint64_t dmaBytes_ = 0, dmaNs_ = 0;
};

} // namespace indy
