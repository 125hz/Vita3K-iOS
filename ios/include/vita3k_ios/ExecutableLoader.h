#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace vita3k::ios {

class GuestMemory;
struct ExecutableProbeResult;

struct PlainElfLoadResult {
    bool attempted = false;
    bool loaded = false;
    bool module_info_valid = false;
    bool relocations_applied = false;
    bool module_tables_parsed = false;
    bool module_start_valid = false;
    bool module_start_from_export = false;
    bool import_stubs_bound = false;
    std::uint64_t file_bytes = 0;
    std::uint64_t memory_bytes = 0;
    std::size_t shared_page_pairs = 0;
    std::size_t relocation_segment_count = 0;
    std::uint64_t relocation_bytes = 0;
    std::size_t relocation_entry_count = 0;
    std::size_t relocation_patch_count = 0;
    std::size_t export_library_count = 0;
    std::size_t import_library_count = 0;
    std::size_t exported_nid_count = 0;
    std::size_t imported_nid_count = 0;
    std::size_t bound_import_stub_count = 0;
    std::string module_name;
    std::uint32_t module_nid = 0;
    std::uint32_t module_start_offset = 0;
    std::uint32_t module_start_address = 0;
    std::uint32_t temporary_stack_pointer = 0;
    std::vector<std::uint32_t> imported_nids;
    std::string detail;
};

PlainElfLoadResult load_plain_elf(const std::filesystem::path &path,
    const ExecutableProbeResult &probe,
    GuestMemory &memory);

} // namespace vita3k::ios
