#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>
#include <vector>

namespace indy {

// SCSI peripheral device type codes as reported by INQUIRY (SPC standard).
enum class ScsiDeviceType : uint8_t {
    DirectAccess = 0x00, // hard disk
    CdRom        = 0x05,
};

// Target status bytes (SAM).
inline constexpr uint8_t kScsiGood           = 0x00;
inline constexpr uint8_t kScsiCheckCondition = 0x02;

// Outcome of one SCSI command at the target.
struct ScsiCommandResult {
    uint8_t status = kScsiGood;
    std::vector<uint8_t> dataIn; // bytes for the DATA IN phase (empty: none)
};

// A device sitting on the SCSI bus (disk, CD-ROM, tape...). The host adapter
// runs the bus phases; the target only interprets CDBs.
class ScsiTarget {
public:
    ScsiTarget() = default;
    virtual ~ScsiTarget() = default;
    ScsiTarget(const ScsiTarget&) = delete;
    ScsiTarget& operator=(const ScsiTarget&) = delete;

    [[nodiscard]] virtual std::string_view name() const noexcept = 0;
    [[nodiscard]] virtual ScsiDeviceType type() const noexcept = 0;
    virtual void reset() = 0;

    // Bytes the target expects in a DATA OUT phase for this CDB (0 = none).
    [[nodiscard]] virtual size_t dataOutLength(std::span<const uint8_t> cdb) const { (void)cdb; return 0; }

    // Execute a command; `dataOut` holds the DATA OUT bytes the initiator sent.
    [[nodiscard]] virtual ScsiCommandResult execute(std::span<const uint8_t> cdb,
                                                    std::span<const uint8_t> dataOut) = 0;
};

} // namespace indy
