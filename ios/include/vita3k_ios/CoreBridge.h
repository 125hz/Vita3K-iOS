#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace vita3k::ios {

struct ImportedArtifact {
    std::string filename;
    std::uintmax_t size;
    std::string kind;
    bool structurally_valid;
    std::size_t load_segment_count;
    bool load_attempted;
    bool loaded;
    bool module_info_valid;
    bool relocations_applied;
    bool module_tables_parsed;
    bool import_stubs_bound;
    bool module_start_valid;
    bool execution_attempted;
    bool thread_exited;
    std::string module_name;
    std::uint32_t module_nid;
    std::size_t relocation_segment_count;
    std::size_t relocation_entry_count;
    std::size_t relocation_patch_count;
    std::size_t export_library_count;
    std::size_t import_library_count;
    std::size_t exported_nid_count;
    std::size_t imported_nid_count;
    std::size_t bound_import_stub_count;
    std::uint32_t module_start_address;
    std::uint64_t executed_instruction_count;
    std::size_t hle_dispatch_count;
    std::int32_t thread_exit_status;
    std::string detail;
};

struct CoreStatus {
    bool linked;
    bool self_tests_passed;
    bool storage_ready;
    bool guest_memory_ready;
    bool segment_mapping_ready;
    bool loader_pipeline_ready;
    bool import_binding_ready;
    bool arm_execution_ready;
    bool guest_thread_ready;
    std::uint64_t guest_memory_size;
    std::size_t host_page_size;
    std::uint64_t arm_test_instruction_count;
    std::size_t hle_test_dispatch_count;
    std::uint64_t thread_test_instruction_count;
    std::size_t thread_test_hle_dispatch_count;
    std::int32_t thread_test_exit_status;
    std::size_t thread_test_bound_stub_count;
    std::string storage_root;
    std::vector<ImportedArtifact> imported_artifacts;
    std::string summary;
};

CoreStatus initialize_core(const std::filesystem::path &documents_root);
CoreStatus query_core_status();
CoreStatus rescan_imports();

} // namespace vita3k::ios
