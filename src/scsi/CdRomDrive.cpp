#include "indy/scsi/CdRomDrive.hpp"

#include "indy/core/Log.hpp"
#include "indy/scsi/ScsiUtil.hpp"

#include <algorithm>
#include <format>
#include <system_error>

namespace indy {

using namespace scsi;

namespace {

// SCSI opcodes.
enum : uint8_t {
    kTestUnitReady = 0x00, kRequestSense = 0x03, kRead6 = 0x08, kSeek6 = 0x0B,
    kInquiry = 0x12, kModeSelect6 = 0x15, kModeSense6 = 0x1A, kStartStop = 0x1B,
    kPreventAllow = 0x1E, kReadCapacity = 0x25, kRead10 = 0x28, kSeek10 = 0x2B,
    kReadToc = 0x43,
};



} // namespace

void CdRomDrive::reset() {
    blockSize_ = kDefaultBlockSize;
    sense_ = {};
}

// ---------------------------------------------------------------------------
// Media
// ---------------------------------------------------------------------------

std::expected<void, std::string> CdRomDrive::insertMedia(const std::filesystem::path& image) {
    ejectMedia();
    std::error_code ec;
    const uintmax_t size = std::filesystem::file_size(image, ec);
    if (ec) {
        return std::unexpected(std::format("cannot stat '{}': {}", image.string(), ec.message()));
    }
    if (size < kSectorSize || size % kSectorSize != 0) {
        return std::unexpected(std::format(
            "'{}' is not a whole number of {}-byte sectors (raw BIN images are unsupported)", image.string(),
            kSectorSize));
    }
    image_.open(image, std::ios::binary);
    if (!image_) {
        return std::unexpected(std::format("cannot open '{}'", image.string()));
    }
    path_ = image;
    imageBytes_ = static_cast<uint64_t>(size);
    unitAttention_ = true; // the host must notice the new disc
    return {};
}

void CdRomDrive::ejectMedia() {
    if (image_.is_open()) {
        image_.close();
        unitAttention_ = true;
    }
    image_.clear();
    path_.clear();
    imageBytes_ = 0;
}

bool CdRomDrive::readBytes(uint64_t offset, std::span<uint8_t> out) {
    if (!hasMedia() || offset > imageBytes_ || out.size() > imageBytes_ - offset) {
        return false;
    }
    image_.seekg(static_cast<std::streamoff>(offset));
    image_.read(reinterpret_cast<char*>(out.data()), static_cast<std::streamsize>(out.size()));
    if (!image_) {
        image_.clear();
        return false;
    }
    return true;
}

// ---------------------------------------------------------------------------
// Commands
// ---------------------------------------------------------------------------

ScsiCommandResult CdRomDrive::checkCondition(uint8_t key, uint8_t asc, uint8_t ascq) {
    sense_ = {key, asc, ascq};
    return {kScsiCheckCondition, {}};
}

size_t CdRomDrive::dataOutLength(std::span<const uint8_t> cdb) const {
    return !cdb.empty() && cdb[0] == kModeSelect6 && cdb.size() >= 5 ? cdb[4] : 0;
}

ScsiCommandResult CdRomDrive::execute(std::span<const uint8_t> cdb, std::span<const uint8_t> dataOut) {
    if (cdb.empty()) {
        return checkCondition(kSenseIllegalRequest, kAscInvalidOpcode, 0);
    }
    const uint8_t op = cdb[0];

    // INQUIRY and REQUEST SENSE are always answered; anything else first
    // reports a pending medium change, then "no medium" while empty.
    if (op != kInquiry && op != kRequestSense) {
        if (unitAttention_) {
            unitAttention_ = false;
            return checkCondition(kSenseUnitAttention, kAscMediumChanged, 0);
        }
        const bool needsMedium = op == kTestUnitReady || op == kRead6 || op == kRead10 ||
                                 op == kReadCapacity || op == kReadToc || op == kSeek6 || op == kSeek10;
        if (needsMedium && !hasMedia()) {
            return checkCondition(kSenseNotReady, kAscNoMedium, 0);
        }
    }

    switch (op) {
    case kTestUnitReady:
    case kStartStop:
    case kPreventAllow:
    case kSeek6:
    case kSeek10:
        return good();

    case kRequestSense: {
        auto data = senseData({sense_.key, sense_.asc, sense_.ascq});
        sense_ = {};
        return good(truncated(std::move(data), cdb.size() > 4 ? cdb[4] : 18));
    }

    case kInquiry:
        return inquiry(cdb);

    case kModeSelect6:
        return modeSelect(dataOut);

    case kModeSense6:
        return modeSense(cdb);

    case kReadCapacity: {
        std::vector<uint8_t> d(8, 0);
        putBe32(d, 0, static_cast<uint32_t>(blockCount() - 1)); // last LBA
        putBe32(d, 4, blockSize_);
        return good(std::move(d));
    }

    case kRead6: {
        const uint64_t lba = (uint32_t{cdb[1] & 0x1Fu} << 16) | be16(cdb, 2);
        const uint32_t blocks = cdb[4] == 0 ? 256 : cdb[4];
        return read(lba, blocks);
    }

    case kRead10:
        return read(be32(cdb, 2), be16(cdb, 7));

    case kReadToc:
        return readToc(cdb);

    default:
        log::debug("CD-ROM: unsupported SCSI command {:#04x}", op);
        return checkCondition(kSenseIllegalRequest, kAscInvalidOpcode, 0);
    }
}

ScsiCommandResult CdRomDrive::read(uint64_t lba, uint32_t blocks) {
    if (lba + blocks > blockCount()) {
        return checkCondition(kSenseIllegalRequest, kAscLbaOutOfRange, 0);
    }
    std::vector<uint8_t> data(static_cast<size_t>(blocks) * blockSize_);
    if (!readBytes(lba * blockSize_, data)) {
        return checkCondition(kSenseIllegalRequest, kAscLbaOutOfRange, 0);
    }
    return good(std::move(data));
}

ScsiCommandResult CdRomDrive::inquiry(std::span<const uint8_t> cdb) const {
    return good(truncated(inquiryData(ScsiDeviceType::CdRom, true, "TOSHIBA", "CD-ROM XM-3401TA", "0283"),
                          cdb.size() > 4 ? cdb[4] : 36));
}

ScsiCommandResult CdRomDrive::modeSense(std::span<const uint8_t> cdb) const {
    // Header + one block descriptor; no mode pages beyond that.
    std::vector<uint8_t> d(12, 0);
    d[0] = 11;    // mode data length (excluding this byte)
    d[2] = 0x80;  // write protected
    d[3] = 8;     // block descriptor length
    putBe32(d, 4, static_cast<uint32_t>(std::min<uint64_t>(blockCount(), 0x00FF'FFFF))); // density 0
    putBe32(d, 8, blockSize_);
    const size_t allocation = cdb.size() > 4 ? cdb[4] : d.size();
    d.resize(std::min(d.size(), allocation));
    return good(std::move(d));
}

ScsiCommandResult CdRomDrive::modeSelect(std::span<const uint8_t> data) {
    // 4-byte header, then an optional 8-byte block descriptor with the block length.
    if (data.size() >= 12 && data[3] >= 8) {
        const uint32_t requested = uint32_t{data[9]} << 16 | uint32_t{data[10]} << 8 | data[11];
        if (requested != 512 && requested != 2048) {
            log::warn("CD-ROM: MODE SELECT block size {} not supported, keeping {}", requested, blockSize_);
            return checkCondition(kSenseIllegalRequest, kAscInvalidField, 0);
        }
        blockSize_ = requested;
    }
    return good();
}

ScsiCommandResult CdRomDrive::readToc(std::span<const uint8_t> cdb) const {
    // One data track starting at LBA 0, plus the lead-out.
    const bool msf = (cdb[1] & 0x02) != 0;
    const auto address = [&](uint64_t lba2048) -> uint32_t {
        if (!msf) return static_cast<uint32_t>(lba2048);
        const uint64_t f = lba2048 + 150; // 2-second pregap
        return static_cast<uint32_t>(((f / 75 / 60) << 16) | (((f / 75) % 60) << 8) | (f % 75));
    };
    std::vector<uint8_t> d(4 + 8 * 2, 0);
    putBe16(d, 0, static_cast<uint32_t>(d.size() - 2));
    d[2] = 1; d[3] = 1;                                // first/last track
    d[5] = 0x14; d[6] = 1;                             // ADR=1, data track; track 1
    putBe32(d, 8, address(0));
    d[13] = 0x14; d[14] = 0xAA;                        // lead-out
    putBe32(d, 16, address(imageBytes_ / kSectorSize));
    const size_t allocation = be16(cdb, 7);
    d.resize(std::min(d.size(), allocation));
    return good(std::move(d));
}

} // namespace indy
