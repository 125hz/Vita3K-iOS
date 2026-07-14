#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>

namespace vita3k::ios {

class GuestMemory;
struct ExecutableProbeResult;

struct PlainElfLoadResult {
    bool attempted = false;
    bool loaded = false;
    bool module_info_valid = false;
    std::uint64_t file_bytes = 0;
    std::uint64_t memory_bytes = 0;
    std::size_t shared_page_pairs = 0;
    std::size_t relocation_segment_count = 0;
    std::uint64_t relocation_bytes = 0;
    std::string module_name;
    std::uint32_t module_nid = 0;
    std::string detail;
};

PlainElfLoadResult load_plain_elf(const std::filesystem::path &path,
    const ExecutableProbeResult &probe,
    GuestMemory &memory);

} // namespace vita3k::ios
