#pragma once

#include "indy/core/MmioDevice.hpp"
#include "indy/core/Types.hpp"

#include <array>
#include <chrono>
#include <filesystem>
#include <functional>
#include <optional>

namespace indy {

// Dallas DS1386-8K: real-time clock + 8 KiB battery-backed SRAM, reached
// through HPC3 at 0x1FBE0000 (Linux asm/sgi/hpc3.h `rtcregs`/`bbram`). Each
// chip byte sits in the low byte lane of its own 32-bit slot, so chip address
// n is at offset 4n (+3 for a byte access). Register layout from the Dallas
// datasheet as modelled by MAME src/devices/machine/ds1386.cpp.
//
// The clock is derived from emulated time (starting at host UTC), not ticked:
// whenever software reads a clock register with TE set, the registers are
// refreshed. Writes to time registers take effect when TE is (or becomes) set.
// Not yet modelled: alarms, watchdog, INTA/INTB outputs, square wave.
class Ds1386 final : public MmioDevice {
public:
    static constexpr size_t kSize = 8 * 1024;

    enum Reg : uint8_t {
        kHundredths  = 0x00,
        kSeconds     = 0x01,
        kMinutes     = 0x02,
        kMinuteAlarm = 0x03,
        kHours       = 0x04, // bit 6: 12-hour mode, bit 5: PM (12-hour mode)
        kHourAlarm   = 0x05,
        kDays        = 0x06, // day of week 1..7
        kDayAlarm    = 0x07,
        kDate        = 0x08,
        kMonths      = 0x09, // bit 7: oscillator disabled, bit 6: square wave disabled
        kYears       = 0x0A,
        kCommand     = 0x0B, // TE IPSW IBH/LO PU/LVL WAM TDM WAF TDF
        kWatchdogHundredths = 0x0C,
        kWatchdogSeconds    = 0x0D,
        kUserRam     = 0x0E, // ...0x1FFF
    };

    static constexpr uint8_t kHours12h      = 0x40;
    static constexpr uint8_t kHoursPm       = 0x20;
    static constexpr uint8_t kMonthsOscOff  = 0x80;
    static constexpr uint8_t kCommandTE     = 0x80;
    static constexpr uint8_t kCommandWAM    = 0x08;
    static constexpr uint8_t kCommandTDM    = 0x04;

    using Clock = std::function<Cycles()>;
    using TimePoint = std::chrono::sys_time<std::chrono::milliseconds>;

    struct Config {
        uint64_t cpuClockHz = 133'000'000;
        std::filesystem::path backingFile;   // empty = volatile (tests)
        std::optional<TimePoint> powerOnTime; // default: host UTC now
        // The chip stores a 2-digit year; its epoch is a software convention.
        // SGI's PROM/IRIX count from 1940 (the PROM shows 1966 for a stored 26).
        int yearBase = 1940;
    };

    Ds1386(Config config, Clock clock);
    ~Ds1386() override;

    [[nodiscard]] std::string_view name() const noexcept override { return "DS1386"; }
    void reset() override {} // battery-backed: survives system reset
    [[nodiscard]] bool read(uint32_t offset, AccessSize size, uint64_t& value) override;
    [[nodiscard]] bool write(uint32_t offset, AccessSize size, uint64_t value) override;

    // Write the SRAM to the backing file (also done on destruction).
    void save() const;

    // Chip byte by address (inspection/tests; refreshes the clock first).
    [[nodiscard]] uint8_t peek(uint16_t address);
    // Store a chip byte as software would (setup, e.g. factory NVRAM data).
    void poke(uint16_t address, uint8_t value);
    // Current time-of-day as the chip counts it.
    [[nodiscard]] TimePoint now() const;

private:
    [[nodiscard]] uint8_t readByte(uint16_t address);
    void writeByte(uint16_t address, uint8_t value);

    void refreshRegisters();  // counters -> registers (when TE is set)
    void commitRegisters();   // registers -> counters (pending writes)
    void encode(TimePoint t);
    [[nodiscard]] std::optional<TimePoint> decode() const;
    [[nodiscard]] bool oscillatorRunning() const { return (ram_[kMonths] & kMonthsOscOff) == 0; }

    Config config_;
    Clock clock_;
    std::array<uint8_t, kSize> ram_{};

    // Counters: the time at `baseCycle_` is `baseTime_`.
    TimePoint baseTime_{};
    Cycles baseCycle_ = 0;
    bool pendingWrite_ = false; // time registers written, not yet committed
};

} // namespace indy
