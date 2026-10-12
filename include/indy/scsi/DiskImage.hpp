#pragma once

#include <cstdint>
#include <expected>
#include <filesystem>
#include <memory>
#include <span>
#include <string>
#include <string_view>

namespace indy {

// Backing store for a SCSI disk: a byte-addressed, fixed-size image.
//
// Two formats are recognised by their contents:
//   * raw: the disk's bytes, as written by `dd` or `--disk-size` (sparse files
//     stay sparse);
//   * MAME CHD v5, uncompressed ("MComprHD", compressors 0): the disk is cut
//     into hunks, and a map of 32-bit big-endian entries gives each hunk's
//     file offset in units of the hunk size, 0 meaning "never written, reads
//     as zeros". Writes go in place; a write to a missing hunk appends it at
//     the end of the file and records it in the map, as MAME does.
// Compressed CHDs, CHDs with a parent and older CHD versions are refused with
// a hint on converting them with chdman.
class DiskImage {
public:
    virtual ~DiskImage() = default;

    [[nodiscard]] virtual uint64_t size() const noexcept = 0;
    [[nodiscard]] virtual std::string_view format() const noexcept = 0;
    // Whole requests only: false on any I/O error or out-of-range access.
    [[nodiscard]] virtual bool read(uint64_t offset, std::span<uint8_t> out) = 0;
    [[nodiscard]] virtual bool write(uint64_t offset, std::span<const uint8_t> data) = 0;
    virtual void flush() = 0;
};

// Opens `path` (raw or CHD). A missing file is created as a blank sparse raw
// image of `createBytes` when that is non-zero.
[[nodiscard]] std::expected<std::unique_ptr<DiskImage>, std::string>
openDiskImage(const std::filesystem::path& path, uint64_t createBytes = 0);

} // namespace indy
