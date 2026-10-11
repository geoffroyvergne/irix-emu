#include "indy/scsi/ScsiDisk.hpp"

#include "indy/core/Log.hpp"

#include <format>
#include <system_error>

namespace indy {

using namespace scsi;

namespace {

enum : uint8_t {
    kTestUnitReady = 0x00, kRezero = 0x01, kRequestSense = 0x03, kFormatUnit = 0x04,
    kReassignBlocks = 0x07, kRead6 = 0x08, kWrite6 = 0x0A, kSeek6 = 0x0B, kInquiry = 0x12,
    kModeSelect6 = 0x15, kReserve = 0x16, kRelease = 0x17, kModeSense6 = 0x1A, kStartStop = 0x1B,
    kSendDiagnostic = 0x1D, kPreventAllow = 0x1E, kReadCapacity = 0x25, kRead10 = 0x28,
    kWrite10 = 0x2A, kSeek10 = 0x2B, kWriteVerify = 0x2E, kVerify = 0x2F, kSyncCache = 0x35,
    kReadDefectData = 0x37, kModeSelect10 = 0x55, kModeSense10 = 0x5A,
};

constexpr uint32_t sixByteCount(std::span<const uint8_t> cdb) { return cdb[4] == 0 ? 256u : cdb[4]; }

} // namespace

std::expected<std::unique_ptr<ScsiDisk>, std::string> ScsiDisk::open(const std::filesystem::path& image,
                                                                     uint64_t createBytes) {
    std::error_code ec;
    if (!std::filesystem::exists(image, ec)) {
        if (createBytes == 0) {
            return std::unexpected(std::format(
                "disk image '{}' does not exist (use --disk-size <MiB> to create a blank one)", image.string()));
        }
        { std::ofstream create(image, std::ios::binary); }
        std::filesystem::resize_file(image, createBytes, ec); // sparse on APFS/ext4/NTFS
        if (ec) {
            return std::unexpected(std::format("cannot create '{}': {}", image.string(), ec.message()));
        }
        log::info("Disk: created blank {} MiB image '{}'", createBytes / (1024 * 1024), image.string());
    }
    const uintmax_t size = std::filesystem::file_size(image, ec);
    if (ec) {
        return std::unexpected(std::format("cannot stat '{}': {}", image.string(), ec.message()));
    }
    if (size < kBlockSize || size % kBlockSize != 0) {
        return std::unexpected(std::format("'{}' is not a whole number of {}-byte blocks", image.string(), kBlockSize));
    }
    std::fstream file(image, std::ios::binary | std::ios::in | std::ios::out);
    if (!file) {
        return std::unexpected(std::format("cannot open '{}' for reading and writing", image.string()));
    }
    return std::unique_ptr<ScsiDisk>(new ScsiDisk(std::move(file), image, static_cast<uint64_t>(size / kBlockSize)));
}

ScsiDisk::~ScsiDisk() {
    file_.flush();
}

ScsiCommandResult ScsiDisk::checkCondition(uint8_t key, uint8_t asc, uint8_t ascq) {
    sense_ = {key, asc, ascq};
    return {kScsiCheckCondition, {}};
}

size_t ScsiDisk::dataOutLength(std::span<const uint8_t> cdb) const {
    if (cdb.empty()) return 0;
    switch (cdb[0]) {
    case kWrite6:         return size_t{sixByteCount(cdb)} * kBlockSize;
    case kWrite10:
    case kWriteVerify:    return size_t{be16(cdb, 7)} * kBlockSize;
    case kModeSelect6:    return cdb[4];
    case kModeSelect10:   return be16(cdb, 7);
    case kSendDiagnostic: return be16(cdb, 3);
    case kFormatUnit:     return (cdb[1] & 0x10) != 0 ? 4 : 0; // defect list header
    case kReassignBlocks: return 4;                             // defect list header
    default:              return 0;
    }
}

ScsiCommandResult ScsiDisk::execute(std::span<const uint8_t> cdb, std::span<const uint8_t> dataOut) {
    if (cdb.empty()) {
        return checkCondition(kSenseIllegalRequest, kAscInvalidOpcode);
    }
    const uint8_t op = cdb[0];
    if (op != kInquiry && op != kRequestSense && unitAttention_) {
        unitAttention_ = false;
        return checkCondition(kSenseUnitAttention, kAscPowerOnReset);
    }

    switch (op) {
    case kTestUnitReady: case kRezero: case kSeek6: case kSeek10: case kReserve: case kRelease:
    case kStartStop: case kSendDiagnostic: case kPreventAllow: case kVerify: case kSyncCache:
    case kModeSelect6: case kModeSelect10: case kReassignBlocks:
        return good(); // mode pages are fixed; nothing to reassign on an image

    case kFormatUnit:
        // An image has no media to format; contents are kept (fx formats are
        // for real drives, and zeroing gigabytes would gain nothing).
        return good();

    case kRequestSense: {
        auto data = senseData(sense_);
        sense_ = {};
        return good(truncated(std::move(data), cdb.size() > 4 ? cdb[4] : 18));
    }

    case kInquiry:
        return good(truncated(inquiryData(ScsiDeviceType::DirectAccess, false, "SEAGATE", "ST32550N", "0022"),
                              cdb.size() > 4 ? cdb[4] : 36));

    case kReadCapacity: {
        std::vector<uint8_t> d(8, 0);
        putBe32(d, 0, static_cast<uint32_t>(blocks_ - 1));
        putBe32(d, 4, kBlockSize);
        return good(std::move(d));
    }

    case kRead6:
        return read((uint32_t{cdb[1] & 0x1Fu} << 16) | be16(cdb, 2), sixByteCount(cdb));
    case kRead10:
        return read(be32(cdb, 2), be16(cdb, 7));
    case kWrite6:
        return write((uint32_t{cdb[1] & 0x1Fu} << 16) | be16(cdb, 2), sixByteCount(cdb), dataOut);
    case kWrite10:
    case kWriteVerify:
        return write(be32(cdb, 2), be16(cdb, 7), dataOut);

    case kModeSense6:
        return modeSense(cdb, false);
    case kModeSense10:
        return modeSense(cdb, true);

    case kReadDefectData: { // empty defect list in the requested format
        std::vector<uint8_t> d(4, 0);
        d[1] = cdb[2] & 0x1F;
        return good(truncated(std::move(d), be16(cdb, 7)));
    }

    default:
        log::debug("Disk: unsupported SCSI command {:#04x}", op);
        return checkCondition(kSenseIllegalRequest, kAscInvalidOpcode);
    }
}

ScsiCommandResult ScsiDisk::read(uint64_t lba, uint32_t blocks) {
    if (lba + blocks > blocks_) {
        return checkCondition(kSenseIllegalRequest, kAscLbaOutOfRange);
    }
    std::vector<uint8_t> data(size_t{blocks} * kBlockSize);
    file_.seekg(static_cast<std::streamoff>(lba * kBlockSize));
    file_.read(reinterpret_cast<char*>(data.data()), static_cast<std::streamsize>(data.size()));
    if (!file_) {
        file_.clear();
        return checkCondition(kSenseMediumError, kAscUnrecoveredRead);
    }
    return good(std::move(data));
}

ScsiCommandResult ScsiDisk::write(uint64_t lba, uint32_t blocks, std::span<const uint8_t> data) {
    if (lba + blocks > blocks_) {
        return checkCondition(kSenseIllegalRequest, kAscLbaOutOfRange);
    }
    // Only whole blocks actually received are written.
    const size_t bytes = std::min(data.size(), size_t{blocks} * kBlockSize) / kBlockSize * kBlockSize;
    file_.seekp(static_cast<std::streamoff>(lba * kBlockSize));
    file_.write(reinterpret_cast<const char*>(data.data()), static_cast<std::streamsize>(bytes));
    if (!file_) {
        file_.clear();
        return checkCondition(kSenseMediumError, 0x0C); // write error
    }
    return good();
}

std::vector<uint8_t> ScsiDisk::modePages(uint8_t page, bool changeable) const {
    // Fixed geometry: heads x sectors per track, cylinders from the capacity.
    const uint32_t cylinders =
        static_cast<uint32_t>(std::max<uint64_t>(1, blocks_ / (uint64_t{kHeads} * kSectorsPerTrack)));
    std::vector<uint8_t> out;
    auto append = [&](uint8_t code, size_t length, auto&& fill) {
        std::vector<uint8_t> p(length + 2, 0);
        p[0] = code;
        p[1] = static_cast<uint8_t>(length);
        if (!changeable) fill(p); // no field is changeable
        out.insert(out.end(), p.begin(), p.end());
    };
    const bool all = page == 0x3F;
    if (all || page == 0x01) append(0x01, 10, [](auto&) {});           // error recovery
    if (all || page == 0x02) append(0x02, 14, [](auto&) {});           // disconnect/reconnect
    if (all || page == 0x03) append(0x03, 22, [&](auto& p) {           // format device
        putBe16(p, 10, kSectorsPerTrack);
        putBe16(p, 12, kBlockSize);
        putBe16(p, 14, 1);                                             // interleave
        p[20] = 0x40;                                                  // hard-sectored
    });
    if (all || page == 0x04) append(0x04, 22, [&](auto& p) {           // rigid disk geometry
        putBe24(p, 2, cylinders);
        p[5] = static_cast<uint8_t>(kHeads);
        putBe16(p, 20, 5400);                                          // rotation rate (rpm)
    });
    if (all || page == 0x08) append(0x08, 10, [](auto&) {});           // caching
    return out;
}

ScsiCommandResult ScsiDisk::modeSense(std::span<const uint8_t> cdb, bool ten) {
    const uint8_t page = cdb[2] & 0x3F;
    const bool changeable = (cdb[2] >> 6) == 1;
    const bool noBlockDescriptor = (cdb[1] & 0x08) != 0;
    std::vector<uint8_t> pages = modePages(page, changeable);
    if (pages.empty() && page != 0x00) {
        return checkCondition(kSenseIllegalRequest, kAscInvalidField);
    }

    std::vector<uint8_t> bd;
    if (!noBlockDescriptor) {
        bd.assign(8, 0);
        putBe24(bd, 1, static_cast<uint32_t>(std::min<uint64_t>(blocks_, 0xFF'FFFF)));
        putBe24(bd, 5, kBlockSize);
    }
    const size_t header = ten ? 8 : 4;
    std::vector<uint8_t> d(header, 0);
    d.insert(d.end(), bd.begin(), bd.end());
    d.insert(d.end(), pages.begin(), pages.end());
    if (ten) {
        putBe16(d, 0, static_cast<uint32_t>(d.size() - 2));
        putBe16(d, 6, static_cast<uint32_t>(bd.size()));
    } else {
        d[0] = static_cast<uint8_t>(d.size() - 1);
        d[3] = static_cast<uint8_t>(bd.size());
    }
    const size_t allocation = ten ? be16(cdb, 7) : cdb[4];
    return good(truncated(std::move(d), allocation));
}

} // namespace indy
