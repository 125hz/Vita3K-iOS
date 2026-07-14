#pragma once

#include <filesystem>
#include <cstdint>
#include <string>
#include <vector>

namespace vita3k::ios {

struct LoadSegmentPlan {
    std::uint32_t file_offset;
    std::uint32_t virtual_address;
    std::uint32_t file_size;
    std::uint32_t memory_size;
    std::uint32_t flags;
};

struct ExecutableProbeResult {
    std::string kind;
    bool recognized = false;
    bool structurally_valid = false;
    std::vector<LoadSegmentPlan> load_segments;
    std::string detail;
};

ExecutableProbeResult probe_artifact(const std::filesystem::path &path);

} // namespace vita3k::ios
