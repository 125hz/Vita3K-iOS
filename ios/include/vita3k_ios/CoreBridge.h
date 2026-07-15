#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

#include <vita3k_ios/HostDisplay.h>
#include <vita3k_ios/HostInput.h>

namespace vita3k::ios {

struct InstalledTitle {
    std::string title_id;
    std::string title;
    std::string app_version;
    bool patch_installed{};
    bool base_eboot_present{};
    bool patch_eboot_present{};
    std::string app_path;
};

struct TitlePreparationResult {
    bool attempted{};
    bool selected{};
    bool patch_selected{};
    bool probe_valid{};
    bool self_segments_plain{};
    bool loaded{};
    bool module_start_from_export{};
    std::string title_id;
    std::string title;
    std::string source;
    std::string executable_path;
    std::string kind;
    std::string module_name;
    std::size_t load_segment_count{};
    std::size_t encrypted_segment_count{};
    std::size_t compressed_segment_count{};
    std::size_t imported_nid_count{};
    std::size_t bound_import_stub_count{};
    std::uint32_t module_start_address{};
    std::string detail;
};

struct TitleBootResult {
    bool attempted{};
    bool started{};
    bool exited{};
    bool returned{};
    std::uint64_t instruction_count{};
    std::size_t hle_dispatch_count{};
    std::uint32_t last_hle_nid{};
    std::uint32_t last_guest_pc{};
    std::size_t unique_pc_count{};
    std::uint32_t hottest_pc{};
    std::uint64_t hottest_pc_hits{};
    std::uint64_t non_forward_pc_count{};
    std::uint32_t libc_dso_handle_main{};
    std::size_t libc_atexit_registration_count{};
    std::size_t libc_finalize_call_count{};
    std::size_t libc_guard_acquire_count{};
    std::size_t libc_guard_release_count{};
    std::size_t libc_guard_abort_count{};
    std::size_t libc_guard_initialization_count{};
    std::size_t libc_guard_recursive_acquire_count{};
    std::size_t libc_heap_allocation_count{};
    std::size_t libc_heap_free_count{};
    std::size_t libc_heap_realloc_count{};
    std::size_t libc_heap_failure_count{};
    std::size_t cxx_new_call_count{};
    std::size_t cxx_new_array_call_count{};
    std::size_t cxx_delete_call_count{};
    std::size_t cxx_delete_array_call_count{};
    std::size_t cxx_nothrow_failure_count{};
    std::size_t cxx_placement_delete_call_count{};
    bool app_util_initialized{};
    std::size_t app_util_init_call_count{};
    std::size_t app_util_shutdown_call_count{};
    std::uint64_t libc_heap_live_bytes{};
    std::uint64_t libc_heap_peak_bytes{};
    std::uint32_t last_libc_atexit_object{};
    std::uint32_t last_libc_atexit_destructor{};
    std::uint32_t last_libc_atexit_dso{};
    std::uint32_t last_libc_finalize_dso{};
    std::uint32_t last_libc_guard_address{};
    std::uint32_t last_libc_guard_word{};
    std::int32_t last_libc_guard_result{};
    std::uint32_t last_libc_heap_address{};
    std::uint32_t last_libc_heap_size{};
    std::uint32_t last_libc_heap_alignment{};
    std::uint32_t last_app_util_init_param{};
    std::uint32_t last_app_util_boot_param{};
    std::uint32_t last_app_util_work_buffer_size{};
    std::uint32_t last_app_util_boot_attribute{};
    std::uint32_t last_app_util_app_version{};
    std::int32_t last_app_util_result{};
    std::int32_t exit_status{};
    std::uint32_t return_value{};
    std::string title_id;
    std::string detail;
};

struct GameInstallResult {
    bool attempted{};
    bool success{};
    std::size_t application_count{};
    std::size_t file_count{};
    std::uint64_t bytes_written{};
    std::vector<std::string> installed_targets;
    std::string detail;
};

struct ImportedArtifact {
    std::string filename;
    std::uintmax_t size;
    std::string kind;
    bool app_metadata_parsed;
    std::string app_title_id;
    std::string app_title;
    std::string app_category;
    std::string app_version;
    bool archive_inspected;
    bool archive_valid;
    std::size_t archive_file_count;
    std::size_t archive_application_count;
    std::size_t archive_unsafe_path_count;
    std::uint64_t archive_uncompressed_size;
    std::string archive_install_target;
    bool structurally_valid;
    std::size_t load_segment_count;
    bool load_attempted;
    bool loaded;
    bool module_info_valid;
    bool relocations_applied;
    bool module_tables_parsed;
    bool import_stubs_bound;
    bool module_start_valid;
    bool module_start_from_export;
    bool execution_attempted;
    bool thread_exited;
    bool thread_returned;
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
    std::uint32_t thread_return_value;
    std::string detail;
};

struct CoreStatus {
    bool linked;
    bool self_tests_passed;
    bool upstream_metadata_ready;
    bool upstream_archive_ready;
    bool package_installer_ready;
    bool storage_ready;
    bool guest_memory_ready;
    bool segment_mapping_ready;
    bool loader_pipeline_ready;
    bool import_binding_ready;
    bool arm_execution_ready;
    bool guest_thread_ready;
    bool renderer_attached;
    bool renderer_frame_presented;
    bool input_surface_attached;
    bool input_touch_received;
    bool input_controller_connected;
    bool input_controller_received;
    std::uint64_t guest_memory_size;
    std::size_t host_page_size;
    std::uint64_t arm_test_instruction_count;
    std::size_t hle_test_dispatch_count;
    std::uint64_t thread_test_instruction_count;
    std::size_t thread_test_hle_dispatch_count;
    std::int32_t thread_test_exit_status;
    std::size_t thread_test_bound_stub_count;
    std::uint64_t renderer_submitted_frame_count;
    std::uint64_t renderer_presented_frame_count;
    std::uint64_t input_touch_sample_count;
    std::uint64_t input_controller_sample_count;
    double input_last_touch_x;
    double input_last_touch_y;
    std::string storage_root;
    std::vector<InstalledTitle> installed_titles;
    std::string package_install_status;
    std::string selected_title_id;
    bool selected_executable_loaded{};
    bool selected_boot_available{};
    bool selected_boot_attempted{};
    std::string title_preparation_status;
    std::string title_boot_status;
    std::vector<ImportedArtifact> imported_artifacts;
    std::string summary;
};

CoreStatus initialize_core(const std::filesystem::path &documents_root);
CoreStatus query_core_status();
CoreStatus rescan_imports();
GameInstallResult install_game_archive(const std::filesystem::path &archive_path);
TitlePreparationResult prepare_installed_title(std::string title_id, bool prefer_patch);
TitleBootResult attempt_prepared_title_boot(std::size_t instruction_limit = 65536);
bool attach_host_display(std::uint32_t width, std::uint32_t height, std::string &error);
std::optional<HostDisplayFrame> acquire_host_display_frame(std::uint32_t width,
    std::uint32_t height, std::string &error);
bool complete_host_display_frame(std::uint64_t identifier, bool presented,
    std::string detail, std::string &error);
bool attach_host_input_surface(double width, double height, std::string &error);
bool submit_host_touch(std::uint64_t identifier, double x, double y, HostTouchPhase phase,
    std::string &error);
void set_host_controller_connected(bool connected);
bool submit_host_controller(HostControllerSample sample, std::string &error);

} // namespace vita3k::ios
