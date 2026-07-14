#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace vita3k::ios {

class GuestMemory;

struct ModuleTableSummary {
    bool success = false;
    std::size_t export_library_count = 0;
    std::size_t import_library_count = 0;
    std::size_t exported_function_count = 0;
    std::size_t imported_function_count = 0;
    std::size_t exported_variable_count = 0;
    std::size_t imported_variable_count = 0;
    std::vector<std::uint32_t> exported_nids;
    std::vector<std::uint32_t> imported_nids;
    std::string detail;
};

ModuleTableSummary parse_module_tables(GuestMemory &memory,
    std::uint32_t module_segment_base,
    std::uint32_t module_segment_size,
    std::span<const std::uint8_t> module_info);

} // namespace vita3k::ios
