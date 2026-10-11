#pragma once

#include "indy/scsi/ScsiTarget.hpp"

#include <expected>
#include <filesystem>
#include <fstream>
#include <span>
#include <string>

namespace indy {

// SCSI CD-ROM backed by a hot-swappable disc image (2048-byte sectors).
//
// Presents itself as a Toshiba XM-3401TA, the drive SGI shipped. SGI drives
// operate with 512-byte logical blocks by default (the PROM and IRIX read the
// disc's SGI volume header in 512-byte units); MODE SELECT can switch to 2048.
//
// Commands: TEST UNIT READY, REQUEST SENSE, INQUIRY, MODE SELECT/SENSE(6),
// START STOP UNIT, PREVENT/ALLOW MEDIUM REMOVAL, READ CAPACITY, READ(6),
// READ(10), SEEK, READ TOC. Swapping discs raises UNIT ATTENTION.
class CdRomDrive final : public ScsiTarget {
public:
    static constexpr uint32_t kSectorSize       = 2048; // image granularity
    static constexpr uint32_t kDefaultBlockSize = 512;  // SGI-compatible default

    [[nodiscard]] std::string_view name() const noexcept override { return "CD-ROM"; }
    [[nodiscard]] ScsiDeviceType type() const noexcept override { return ScsiDeviceType::CdRom; }
    void reset() override;

    [[nodiscard]] size_t dataOutLength(std::span<const uint8_t> cdb) const override;
    [[nodiscard]] ScsiCommandResult execute(std::span<const uint8_t> cdb,
                                            std::span<const uint8_t> dataOut) override;

    // Replaces any currently loaded disc. On failure the drive is left empty.
    std::expected<void, std::string> insertMedia(const std::filesystem::path& image);
    void ejectMedia();

    [[nodiscard]] bool hasMedia() const noexcept { return imageBytes_ != 0; }
    [[nodiscard]] uint64_t imageBytes() const noexcept { return imageBytes_; }
    [[nodiscard]] uint32_t blockSize() const noexcept { return blockSize_; }
    [[nodiscard]] uint64_t blockCount() const noexcept { return imageBytes_ / blockSize_; }
    [[nodiscard]] const std::filesystem::path& mediaPath() const noexcept { return path_; }

    // Read raw bytes at `offset` in the image.
    [[nodiscard]] bool readBytes(uint64_t offset, std::span<uint8_t> out);

private:
    struct Sense {
        uint8_t key = 0, asc = 0, ascq = 0;
    };

    ScsiCommandResult checkCondition(uint8_t key, uint8_t asc, uint8_t ascq);
    ScsiCommandResult read(uint64_t lba, uint32_t blocks);
    ScsiCommandResult inquiry(std::span<const uint8_t> cdb) const;
    ScsiCommandResult modeSense(std::span<const uint8_t> cdb) const;
    ScsiCommandResult readToc(std::span<const uint8_t> cdb) const;
    ScsiCommandResult modeSelect(std::span<const uint8_t> data);

    std::ifstream image_;
    std::filesystem::path path_;
    uint64_t imageBytes_ = 0;
    uint32_t blockSize_ = kDefaultBlockSize;
    bool unitAttention_ = false; // medium changed since the host last looked
    Sense sense_;
};

} // namespace indy
