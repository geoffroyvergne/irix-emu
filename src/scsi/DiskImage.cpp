#include "indy/scsi/DiskImage.hpp"

#include "indy/core/Log.hpp"

#include <algorithm>
#include <array>
#include <format>
#include <fstream>
#include <system_error>
#include <vector>

namespace indy {

namespace {

uint32_t loadBe32(const uint8_t* p) {
    return uint32_t{p[0]} << 24 | uint32_t{p[1]} << 16 | uint32_t{p[2]} << 8 | p[3];
}
uint64_t loadBe64(const uint8_t* p) { return uint64_t{loadBe32(p)} << 32 | loadBe32(p + 4); }

// Positioned I/O on an fstream (one emulation thread uses an image).
bool readAt(std::fstream& file, uint64_t offset, std::span<uint8_t> out) {
    file.seekg(static_cast<std::streamoff>(offset));
    file.read(reinterpret_cast<char*>(out.data()), static_cast<std::streamsize>(out.size()));
    if (!file) {
        file.clear();
        return false;
    }
    return true;
}
bool writeAt(std::fstream& file, uint64_t offset, std::span<const uint8_t> data) {
    file.seekp(static_cast<std::streamoff>(offset));
    file.write(reinterpret_cast<const char*>(data.data()), static_cast<std::streamsize>(data.size()));
    if (!file) {
        file.clear();
        return false;
    }
    return true;
}

// ---------------------------------------------------------------------------
// Raw image
// ---------------------------------------------------------------------------

class RawDiskImage final : public DiskImage {
public:
    RawDiskImage(std::fstream file, uint64_t size) : file_(std::move(file)), size_(size) {}
    ~RawDiskImage() override { flush(); }

    [[nodiscard]] uint64_t size() const noexcept override { return size_; }
    [[nodiscard]] std::string_view format() const noexcept override { return "raw"; }
    bool read(uint64_t offset, std::span<uint8_t> out) override {
        return offset + out.size() <= size_ && readAt(file_, offset, out);
    }
    bool write(uint64_t offset, std::span<const uint8_t> data) override {
        return offset + data.size() <= size_ && writeAt(file_, offset, data);
    }
    void flush() override { file_.flush(); }

private:
    std::fstream file_;
    uint64_t size_;
};

// ---------------------------------------------------------------------------
// MAME CHD v5, uncompressed
// ---------------------------------------------------------------------------

constexpr std::array<uint8_t, 8> kChdTag = {'M', 'C', 'o', 'm', 'p', 'r', 'H', 'D'};
constexpr size_t kChdV5HeaderSize = 124;

class ChdDiskImage final : public DiskImage {
public:
    ChdDiskImage(std::fstream file, uint64_t logicalBytes, uint64_t mapOffset, uint32_t hunkBytes, uint64_t fileEnd)
        : file_(std::move(file)), size_(logicalBytes), mapOffset_(mapOffset), hunkBytes_(hunkBytes),
          fileEnd_(fileEnd) {}
    ~ChdDiskImage() override { flush(); }

    [[nodiscard]] uint64_t size() const noexcept override { return size_; }
    [[nodiscard]] std::string_view format() const noexcept override { return "CHD (uncompressed)"; }

    bool read(uint64_t offset, std::span<uint8_t> out) override {
        if (offset + out.size() > size_) return false;
        return forEachHunk(offset, out.size(), [&](uint64_t /*hunk*/, uint32_t entry, uint32_t inner, size_t done,
                                                   size_t n) {
            const std::span<uint8_t> part = out.subspan(done, n);
            if (entry == 0) {
                std::ranges::fill(part, uint8_t{0}); // never written
                return true;
            }
            return readAt(file_, uint64_t{entry} * hunkBytes_ + inner, part);
        });
    }

    bool write(uint64_t offset, std::span<const uint8_t> data) override {
        if (offset + data.size() > size_) return false;
        return forEachHunk(offset, data.size(), [&](uint64_t hunk, uint32_t entry, uint32_t inner, size_t done,
                                                    size_t n) {
            const std::span<const uint8_t> part = data.subspan(done, n);
            if (entry != 0) {
                return writeAt(file_, uint64_t{entry} * hunkBytes_ + inner, part);
            }
            if (std::ranges::all_of(part, [](uint8_t b) { return b == 0; })) {
                return true; // zeros into a missing hunk: nothing to store
            }
            // Append a new hunk (hunk-aligned) at the end of the file, then
            // point the map at it.
            const uint64_t slot = (fileEnd_ + hunkBytes_ - 1) / hunkBytes_;
            if (slot > 0xFFFF'FFFFu) return false; // beyond what a 32-bit map entry can address
            std::vector<uint8_t> block(hunkBytes_, 0);
            std::ranges::copy(part, block.begin() + inner);
            if (!writeAt(file_, slot * hunkBytes_, block)) return false;
            const std::array<uint8_t, 4> be = {static_cast<uint8_t>(slot >> 24), static_cast<uint8_t>(slot >> 16),
                                               static_cast<uint8_t>(slot >> 8), static_cast<uint8_t>(slot)};
            if (!writeAt(file_, mapOffset_ + hunk * 4, be)) return false;
            fileEnd_ = (slot + 1) * hunkBytes_;
            return true;
        });
    }

    void flush() override { file_.flush(); }

private:
    // Calls fn(hunk, mapEntry, offsetInHunk, bytesDone, bytes) for each hunk
    // the range touches; the map entries are read in one go.
    template <class Fn> bool forEachHunk(uint64_t offset, size_t length, Fn&& fn) {
        if (length == 0) return true;
        const uint64_t first = offset / hunkBytes_;
        const uint64_t last = (offset + length - 1) / hunkBytes_;
        std::vector<uint8_t> map((last - first + 1) * 4);
        if (!readAt(file_, mapOffset_ + first * 4, map)) return false;
        size_t done = 0;
        for (uint64_t hunk = first; hunk <= last; ++hunk) {
            const auto inner = static_cast<uint32_t>((offset + done) % hunkBytes_);
            const size_t n = std::min<size_t>(hunkBytes_ - inner, length - done);
            if (!fn(hunk, loadBe32(&map[(hunk - first) * 4]), inner, done, n)) return false;
            done += n;
        }
        return true;
    }

    std::fstream file_;
    uint64_t size_;
    uint64_t mapOffset_;
    uint32_t hunkBytes_;
    uint64_t fileEnd_;
};

std::expected<std::unique_ptr<DiskImage>, std::string> openChd(std::fstream file, const std::filesystem::path& path,
                                                               uint64_t fileSize) {
    std::array<uint8_t, kChdV5HeaderSize> h{};
    const auto convertHint = std::format(
        "convert it with MAME's chdman, e.g. 'chdman copy -i \"{0}\" -o uncompressed.chd -c none' "
        "(or 'chdman extracthd -i \"{0}\" -o disk.img' for a raw image)",
        path.string());
    if (fileSize < 16 || !readAt(file, 0, std::span(h).first(16))) {
        return std::unexpected(std::format("'{}': truncated CHD header", path.string()));
    }
    const uint32_t version = loadBe32(&h[12]);
    if (version != 5) {
        return std::unexpected(std::format("'{}' is a version {} CHD; only version 5 is supported: {}", path.string(),
                                           version, convertHint));
    }
    if (fileSize < kChdV5HeaderSize || !readAt(file, 0, h)) {
        return std::unexpected(std::format("'{}': truncated CHD header", path.string()));
    }
    // v5 header: compressors[4] at 16, logical bytes 32, map offset 40, meta
    // offset 48, hunk bytes 56, unit bytes 60, raw SHA1 64, SHA1 84, parent SHA1 104.
    for (size_t i = 0; i < 4; ++i) {
        if (loadBe32(&h[16 + 4 * i]) != 0) {
            return std::unexpected(std::format("'{}' is a compressed CHD, which this emulator cannot write to: {}",
                                               path.string(), convertHint));
        }
    }
    if (std::ranges::any_of(std::span(h).subspan(104, 20), [](uint8_t b) { return b != 0; })) {
        return std::unexpected(std::format("'{}' is a CHD with a parent (a diff image); merge it first: {}",
                                           path.string(), convertHint));
    }
    const uint64_t logical = loadBe64(&h[32]);
    const uint64_t mapOffset = loadBe64(&h[40]);
    const uint32_t hunkBytes = loadBe32(&h[56]);
    const uint32_t unitBytes = loadBe32(&h[60]);
    if (hunkBytes == 0 || unitBytes != 512 || hunkBytes % unitBytes != 0 || logical % hunkBytes != 0 ||
        mapOffset + (logical / hunkBytes) * 4 > fileSize) {
        return std::unexpected(std::format("'{}': unsupported CHD layout (hunk {} bytes, unit {} bytes)",
                                           path.string(), hunkBytes, unitBytes));
    }
    return std::make_unique<ChdDiskImage>(std::move(file), logical, mapOffset, hunkBytes, fileSize);
}

} // namespace

std::expected<std::unique_ptr<DiskImage>, std::string> openDiskImage(const std::filesystem::path& path,
                                                                     uint64_t createBytes) {
    std::error_code ec;
    if (!std::filesystem::exists(path, ec)) {
        if (createBytes == 0) {
            return std::unexpected(std::format(
                "disk image '{}' does not exist (use --disk-size <MiB> to create a blank one)", path.string()));
        }
        { std::ofstream create(path, std::ios::binary); }
        std::filesystem::resize_file(path, createBytes, ec); // sparse on APFS/ext4/NTFS
        if (ec) {
            return std::unexpected(std::format("cannot create '{}': {}", path.string(), ec.message()));
        }
        log::info("Disk: created blank {} MiB image '{}'", createBytes / (1024 * 1024), path.string());
    }
    const uintmax_t size = std::filesystem::file_size(path, ec);
    if (ec) {
        return std::unexpected(std::format("cannot stat '{}': {}", path.string(), ec.message()));
    }
    std::fstream file(path, std::ios::binary | std::ios::in | std::ios::out);
    if (!file) {
        return std::unexpected(std::format("cannot open '{}' for reading and writing", path.string()));
    }
    std::array<uint8_t, 8> tag{};
    if (size >= tag.size() && readAt(file, 0, tag) && tag == kChdTag) {
        return openChd(std::move(file), path, size);
    }
    return std::make_unique<RawDiskImage>(std::move(file), static_cast<uint64_t>(size));
}

} // namespace indy
