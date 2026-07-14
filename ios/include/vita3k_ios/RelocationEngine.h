#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>

namespace vita3k::ios {

class GuestMemory;
struct LoadSegmentPlan;

struct RelocationApplyResult {
    bool success = false;
    std::size_t entry_count = 0;
    std::size_t patched_value_count = 0;
    std::string detail;
};

RelocationApplyResult apply_relocations(std::span<const std::uint8_t> entries,
    std::span<const LoadSegmentPlan> segments,
    GuestMemory &memory);

} // namespace vita3k::ios
