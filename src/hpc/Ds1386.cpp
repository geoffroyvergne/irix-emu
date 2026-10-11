#include "indy/hpc/Ds1386.hpp"

#include "indy/core/Log.hpp"

#include <fstream>

namespace indy {

namespace {

using namespace std::chrono;

constexpr uint8_t toBcd(unsigned v) { return static_cast<uint8_t>(((v / 10) << 4) | (v % 10)); }
constexpr unsigned fromBcd(uint8_t v) { return (v >> 4) * 10u + (v & 0x0F); }

// Write masks per register (datasheet / MAME); bits outside read back as 0.
constexpr std::array<uint8_t, Ds1386::kUserRam> kWriteMask = {
    0xFF, 0x7F, 0x7F, 0xFF, 0x7F, 0xFF, 0x07, 0x87, 0x3F, 0xDF, 0xFF,
    0xFC, // command: WAF/TDF flags are not directly writable
    0xFF, 0xFF,
};

constexpr bool isTimeRegister(uint16_t a) {
    return a <= Ds1386::kYears && a != Ds1386::kMinuteAlarm && a != Ds1386::kHourAlarm &&
           a != Ds1386::kDayAlarm;
}

} // namespace

Ds1386::Ds1386(Config config, Clock clock) : config_(std::move(config)), clock_(std::move(clock)) {
    bool loaded = false;
    if (!config_.backingFile.empty()) {
        std::ifstream file(config_.backingFile, std::ios::binary);
        if (file && file.read(reinterpret_cast<char*>(ram_.data()), kSize)) {
            loaded = true;
            log::info("NVRAM: loaded {}", config_.backingFile.string());
        }
    }
    if (!loaded) {
        // Fresh chip: SRAM clear, transfers enabled, alarm outputs masked.
        ram_.fill(0);
        ram_[kCommand] = kCommandTE | kCommandWAM | kCommandTDM;
        ram_[kMonths]  = 0;
        if (!config_.backingFile.empty()) {
            log::info("NVRAM: no valid {} yet; starting blank (the PROM will initialise it)",
                      config_.backingFile.string());
        }
    }
    // The battery kept the oscillator running while "powered off": resume at
    // host UTC regardless of what the stored registers say.
    baseTime_  = config_.powerOnTime.value_or(time_point_cast<milliseconds>(system_clock::now()));
    baseCycle_ = clock_();
    ram_[kMonths] &= static_cast<uint8_t>(~kMonthsOscOff);
    encode(baseTime_);
}

Ds1386::~Ds1386() {
    try {
        save();
    } catch (...) {
        // Never throw from a destructor; save() already logs failures.
    }
}

void Ds1386::save() const {
    if (config_.backingFile.empty()) {
        return;
    }
    std::ofstream file(config_.backingFile, std::ios::binary | std::ios::trunc);
    if (!file || !file.write(reinterpret_cast<const char*>(ram_.data()), kSize)) {
        log::error("NVRAM: failed to save {}", config_.backingFile.string());
    }
}

// ---------------------------------------------------------------------------
// Bus interface: chip address n at offset 4n, data in bits 7:0
// ---------------------------------------------------------------------------

bool Ds1386::read(uint32_t offset, AccessSize size, uint64_t& value) {
    value = 0;
    if (size == AccessSize::Byte && (offset & 3) != 3) {
        return true; // other byte lanes float
    }
    const uint32_t address = offset >> 2;
    if (address < kSize) {
        value = readByte(static_cast<uint16_t>(address));
    }
    return true;
}

bool Ds1386::write(uint32_t offset, AccessSize size, uint64_t value) {
    if (size == AccessSize::Byte && (offset & 3) != 3) {
        return true;
    }
    const uint32_t address = offset >> 2;
    if (address < kSize) {
        writeByte(static_cast<uint16_t>(address), static_cast<uint8_t>(value));
    }
    return true;
}

uint8_t Ds1386::peek(uint16_t address) {
    return readByte(address);
}

// ---------------------------------------------------------------------------
// Registers
// ---------------------------------------------------------------------------

uint8_t Ds1386::readByte(uint16_t address) {
    if (address <= kYears) {
        refreshRegisters(); // frozen while TE is clear
    }
    return ram_[address];
}

void Ds1386::writeByte(uint16_t address, uint8_t value) {
    if (address >= kUserRam) {
        ram_[address] = value;
        return;
    }
    if (address == kMonths) {
        const bool wasRunning = oscillatorRunning();
        const bool willRun = (value & kMonthsOscOff) == 0;
        if (wasRunning && !willRun) {
            baseTime_  = now(); // stop: freeze the counters here
            baseCycle_ = clock_();
        } else if (!wasRunning && willRun) {
            baseCycle_ = clock_(); // restart from the frozen time
        }
    }
    if (address == kCommand) {
        ram_[kCommand] = static_cast<uint8_t>((ram_[kCommand] & ~kWriteMask[kCommand]) | (value & kWriteMask[kCommand]));
        refreshRegisters(); // setting TE applies pending writes and resumes updates
        return;
    }
    ram_[address] = value & kWriteMask[address];
    if (isTimeRegister(address)) {
        pendingWrite_ = true;
        if ((ram_[kCommand] & kCommandTE) != 0) {
            commitRegisters(); // takes effect "at the next tick"
        }
    }
}

// ---------------------------------------------------------------------------
// Time keeping
// ---------------------------------------------------------------------------

Ds1386::TimePoint Ds1386::now() const {
    if (!oscillatorRunning()) {
        return baseTime_;
    }
    const Cycles elapsed = clock_() - baseCycle_;
    const auto ms = static_cast<int64_t>(elapsed * 1000 / config_.cpuClockHz);
    return baseTime_ + milliseconds{ms};
}

void Ds1386::refreshRegisters() {
    if ((ram_[kCommand] & kCommandTE) == 0) {
        return;
    }
    if (pendingWrite_) {
        commitRegisters();
    }
    encode(now());
}

void Ds1386::commitRegisters() {
    pendingWrite_ = false;
    if (const auto t = decode()) {
        baseTime_  = *t;
        baseCycle_ = clock_();
    }
    // An invalid date (e.g. mid-sequence) is kept in the registers and
    // committed once the remaining fields have been written.
    else {
        pendingWrite_ = true;
    }
}

void Ds1386::encode(TimePoint t) {
    const auto day   = floor<days>(t);
    const year_month_day ymd{day};
    const hh_mm_ss hms{floor<milliseconds>(t - day)};
    const auto hours = static_cast<unsigned>(hms.hours().count());

    ram_[kHundredths] = toBcd(static_cast<unsigned>(hms.subseconds().count() / 10));
    ram_[kSeconds]    = toBcd(static_cast<unsigned>(hms.seconds().count()));
    ram_[kMinutes]    = toBcd(static_cast<unsigned>(hms.minutes().count()));
    if ((ram_[kHours] & kHours12h) != 0) {
        const unsigned h12 = hours % 12 == 0 ? 12 : hours % 12;
        ram_[kHours] = static_cast<uint8_t>(kHours12h | (hours >= 12 ? kHoursPm : 0) | toBcd(h12));
    } else {
        ram_[kHours] = toBcd(hours);
    }
    ram_[kDays]   = static_cast<uint8_t>(weekday{day}.c_encoding() + 1); // 1 = Sunday
    ram_[kDate]   = toBcd(static_cast<unsigned>(ymd.day()));
    ram_[kMonths] = static_cast<uint8_t>((ram_[kMonths] & 0xC0) | toBcd(static_cast<unsigned>(ymd.month())));
    ram_[kYears]  = toBcd(static_cast<unsigned>(((static_cast<int>(ymd.year()) - config_.yearBase) % 100 + 100) % 100));
}

std::optional<Ds1386::TimePoint> Ds1386::decode() const {
    const int fullYear = config_.yearBase + static_cast<int>(fromBcd(ram_[kYears]));
    const year_month_day ymd{year{fullYear}, month{fromBcd(ram_[kMonths] & 0x1F)},
                             day{fromBcd(ram_[kDate])}};
    if (!ymd.ok()) {
        return std::nullopt;
    }
    unsigned hours;
    if ((ram_[kHours] & kHours12h) != 0) {
        hours = fromBcd(ram_[kHours] & 0x1F) % 12 + ((ram_[kHours] & kHoursPm) != 0 ? 12 : 0);
    } else {
        hours = fromBcd(ram_[kHours] & 0x3F);
    }
    const unsigned minutes = fromBcd(ram_[kMinutes]);
    const unsigned seconds = fromBcd(ram_[kSeconds]);
    if (hours > 23 || minutes > 59 || seconds > 59) {
        return std::nullopt;
    }
    return sys_days{ymd} + std::chrono::hours{hours} + std::chrono::minutes{minutes} +
           std::chrono::seconds{seconds} + milliseconds{fromBcd(ram_[kHundredths]) * 10};
}

} // namespace indy
