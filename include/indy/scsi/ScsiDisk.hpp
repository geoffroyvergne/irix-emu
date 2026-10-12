#pragma once

#include "indy/scsi/DiskImage.hpp"
#include "indy/scsi/ScsiTarget.hpp"
#include "indy/scsi/ScsiUtil.hpp"

#include <expected>
#include <filesystem>
#include <memory>
#include <span>
#include <string>

namespace indy {

// SCSI hard disk (direct-access device, 512-byte blocks) backed by a disk
// image (raw, or an uncompressed MAME CHD; see DiskImage), read and written
// in place. On the Indy the system disk is
// conventionally SCSI ID 1 (dksc(0,1,...)).
//
// Commands: TEST UNIT READY, REZERO, REQUEST SENSE, FORMAT UNIT, REASSIGN
// BLOCKS, READ/WRITE(6/10), WRITE AND VERIFY, SEEK, INQUIRY, MODE SELECT/SENSE
// (6/10) with geometry pages, RESERVE/RELEASE, START STOP UNIT, SEND
// DIAGNOSTIC, PREVENT/ALLOW, READ CAPACITY, VERIFY, SYNCHRONIZE CACHE,
// READ DEFECT DATA. The first command after power-on reports UNIT ATTENTION
// (power on / reset), as real drives do.
class ScsiDisk final : public ScsiTarget {
public:
    static constexpr uint32_t kBlockSize = 512;

    // Opens `image` (raw or CHD); if it doesn't exist and `createBytes` is
    // non-zero, a new sparse raw image of that size is created first.
    [[nodiscard]] static std::expected<std::unique_ptr<ScsiDisk>, std::string>
    open(const std::filesystem::path& image, uint64_t createBytes = 0);

    ~ScsiDisk() override;

    [[nodiscard]] std::string_view name() const noexcept override { return "Disk"; }
    [[nodiscard]] ScsiDeviceType type() const noexcept override { return ScsiDeviceType::DirectAccess; }
    void reset() override { unitAttention_ = true; }

    [[nodiscard]] size_t dataOutLength(std::span<const uint8_t> cdb) const override;
    [[nodiscard]] ScsiCommandResult execute(std::span<const uint8_t> cdb,
                                            std::span<const uint8_t> dataOut) override;

    [[nodiscard]] uint64_t blockCount() const noexcept { return blocks_; }
    [[nodiscard]] const std::filesystem::path& path() const noexcept { return path_; }
    [[nodiscard]] std::string_view format() const noexcept { return image_->format(); }

    // Geometry reported in MODE SENSE pages 3/4 (a fixed, plausible layout;
    // all addressing is by logical block).
    static constexpr uint32_t kHeads = 16;
    static constexpr uint32_t kSectorsPerTrack = 63;

private:
    ScsiDisk(std::unique_ptr<DiskImage> image, std::filesystem::path path)
        : image_(std::move(image)), path_(std::move(path)), blocks_(image_->size() / kBlockSize) {}

    ScsiCommandResult checkCondition(uint8_t key, uint8_t asc, uint8_t ascq = 0);
    ScsiCommandResult read(uint64_t lba, uint32_t blocks);
    ScsiCommandResult write(uint64_t lba, uint32_t blocks, std::span<const uint8_t> data);
    [[nodiscard]] std::vector<uint8_t> modePages(uint8_t page, bool changeable) const;
    ScsiCommandResult modeSense(std::span<const uint8_t> cdb, bool ten);

    std::unique_ptr<DiskImage> image_;
    std::filesystem::path path_;
    uint64_t blocks_;
    bool unitAttention_ = true; // power-on reset
    scsi::Sense sense_;
};

} // namespace indy
