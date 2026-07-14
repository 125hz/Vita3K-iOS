#pragma once

#include <filesystem>
#include <cstdint>
#include <string>
#include <vector>

namespace vita3k::ios {

struct LoadSegmentPlan {
    std::uint16_t program_index;
    std::uint32_t file_offset;
    std::uint32_t virtual_address;
    std::uint32_t file_size;
    std::uint32_t memory_size;
    std::uint32_t flags;
    std::uint32_t stored_size{};
    bool compressed{};
};

struct RelocationSegmentPlan {
    std::uint16_t program_index;
    std::uint32_t file_offset;
    std::uint32_t file_size;
    std::uint32_t stored_size{};
    bool compressed{};
};

struct ExecutableProbeResult {
    std::string kind;
    bool recognized = false;
    bool structurally_valid = false;
    std::uint16_t executable_type = 0;
    std::uint32_t entry_point = 0;
    std::uint32_t module_info_offset = 0;
    std::uint16_t module_info_segment_index = 0;
    std::vector<LoadSegmentPlan> load_segments;
    std::vector<RelocationSegmentPlan> relocation_segments;
    bool self_segments_plain = false;
    std::size_t encrypted_segment_count = 0;
    std::size_t compressed_segment_count = 0;
    std::string detail;
};

ExecutableProbeResult probe_artifact(const std::filesystem::path &path);

} // namespace vita3k::ios
