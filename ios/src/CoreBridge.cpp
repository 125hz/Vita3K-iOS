#include <vita3k_ios/CoreBridge.h>
#include <vita3k_ios/ExecutableProbe.h>
#include <vita3k_ios/GuestMemory.h>
#include <vita3k_ios/HostFilesystem.h>

#include <nids/functions.h>
#include <util/arm.h>

#include <algorithm>
#include <array>
#include <limits>
#include <memory>
#include <mutex>
#include <sstream>
#include <string_view>

namespace vita3k::ios {
namespace {

std::mutex core_mutex;
HostStorage host_storage;
std::unique_ptr<GuestMemory> guest_memory;
CoreStatus core_status{
    .linked = true,
    .self_tests_passed = false,
    .storage_ready = false,
    .summary = "Vita3K core slice linked; initialization has not run yet."
};

bool run_upstream_self_tests() {
    const bool arm_encoder_ok = encode_arm_inst(INSTRUCTION_MOVW, 0x1234, 0) == 0xE3010234u;
    const bool thumb_encoder_ok = encode_thumb_inst(INSTRUCTION_BRANCH, 0, 3) != 0;
    const bool nid_database_ok = std::string_view(import_name(0x210C0046u)) == "__sceAppMgrGetAppState";
    const bool unknown_nid_ok = std::string_view(import_name(0xFFFFFFFFu)) == "UNRECOGNISED";
    return arm_encoder_ok && thumb_encoder_ok && nid_database_ok && unknown_nid_ok;
}

bool run_segment_mapping_test(GuestMemory &memory, std::string &error) {
    constexpr std::uint32_t test_address = 0x10000;
    constexpr std::array<std::uint8_t, 16> file_bytes{
        0x7F, 'E', 'L', 'F', 0x56, 0x49, 0x54, 0x41,
        0x33, 0x4B, 0x2D, 0x69, 0x4F, 0x53, 0x01, 0x00
    };
    const auto memory_size_64 = static_cast<std::uint64_t>(memory.host_page_size()) * 2;
    if (memory_size_64 > std::numeric_limits<std::uint32_t>::max()) {
        error = "The host page size is too large for the segment diagnostic.";
        return false;
    }
    const auto memory_size = static_cast<std::uint32_t>(memory_size_64);
    constexpr std::uint32_t read_execute_flags = 5;
    if (!memory.map_segment(test_address, file_bytes, memory_size, read_execute_flags, error)) {
        return false;
    }

    std::array<std::uint8_t, 32> observed{};
    const bool read_ok = memory.read(test_address, observed, error);
    const bool file_copy_ok = read_ok && std::equal(file_bytes.begin(), file_bytes.end(), observed.begin());
    const bool bss_zeroed = read_ok && std::all_of(observed.begin() + file_bytes.size(), observed.end(),
        [](std::uint8_t value) { return value == 0; });

    std::string unmap_error;
    const bool unmapped = memory.unmap_segment(test_address, memory_size, unmap_error);
    if (!read_ok) {
        return false;
    }
    if (!file_copy_ok || !bss_zeroed) {
        error = "The segment diagnostic failed file-copy or BSS zero-fill verification.";
        return false;
    }
    if (!unmapped) {
        error = unmap_error;
        return false;
    }
    return true;
}

void update_status_from_storage() {
    core_status.storage_ready = host_storage.ready;
    core_status.storage_root = host_storage.root.string();
    core_status.imported_artifacts.clear();
    core_status.imported_artifacts.reserve(host_storage.imported_files.size());

    for (const auto &entry : host_storage.imported_files) {
        std::error_code error;
        const auto size = entry.file_size(error);
        const auto probe = probe_artifact(entry.path());
        core_status.imported_artifacts.push_back({
            .filename = entry.path().filename().string(),
            .size = error ? 0 : size,
            .kind = probe.kind,
            .structurally_valid = probe.structurally_valid,
            .load_segment_count = probe.load_segments.size(),
            .detail = probe.detail
        });
    }

    std::ostringstream summary;
    summary << "Core slice: ARM encoder + " << "Vita NID database\n"
            << "Self-tests: " << (core_status.self_tests_passed ? "passed" : "FAILED") << "\n"
            << "Guest memory: " << (core_status.guest_memory_ready ? "ready" : "FAILED");
    if (core_status.guest_memory_ready) {
        summary << " (" << (core_status.guest_memory_size >> 30) << " GiB reserved, "
                << core_status.host_page_size << "-byte pages)";
    }
    summary << "\nSegment map: " << (core_status.segment_mapping_ready
        ? "passed (copy + BSS zero-fill + protect + readback + unmap)"
        : "FAILED");
    summary << "\n"
            << "Storage: " << (core_status.storage_ready ? "ready" : "FAILED") << "\n"
            << "Import candidates: " << core_status.imported_artifacts.size();
    for (const auto &artifact : core_status.imported_artifacts) {
        summary << "\n  • " << artifact.filename << " — " << artifact.kind
                << " — " << (artifact.structurally_valid ? "header valid" : "not loadable")
                << " — " << artifact.load_segment_count << " load segments"
                << "\n    " << artifact.detail;
    }
    summary << "\n\nNext: map a real imported plain ELF and extract Vita module metadata/relocations. Rendering and execution are not active yet.";
    if (!host_storage.error.empty()) {
        summary << "\nStorage error: " << host_storage.error;
    }
    core_status.summary = summary.str();
}

} // namespace

CoreStatus initialize_core(const std::filesystem::path &documents_root) {
    std::lock_guard lock(core_mutex);
    core_status.self_tests_passed = run_upstream_self_tests();
    guest_memory = std::make_unique<GuestMemory>();
    std::string memory_error;
    constexpr std::uint64_t vita_address_space_size = 1ULL << 32;
    const bool reserved = guest_memory->reserve(vita_address_space_size, memory_error);
    const bool protection_test_passed = reserved && guest_memory->run_commit_protection_test(memory_error);
    const bool segment_mapping_passed = protection_test_passed &&
        run_segment_mapping_test(*guest_memory, memory_error);
    core_status.guest_memory_ready = reserved && protection_test_passed;
    core_status.segment_mapping_ready = segment_mapping_passed;
    core_status.guest_memory_size = reserved ? guest_memory->size() : 0;
    core_status.host_page_size = reserved ? guest_memory->host_page_size() : 0;
    host_storage = initialize_host_storage(documents_root);
    if (!memory_error.empty()) {
        if (!host_storage.error.empty()) {
            host_storage.error += " | ";
        }
        host_storage.error += "Guest memory: " + memory_error;
    }
    update_status_from_storage();
    return core_status;
}

CoreStatus query_core_status() {
    std::lock_guard lock(core_mutex);
    return core_status;
}

CoreStatus rescan_imports() {
    std::lock_guard lock(core_mutex);
    scan_imports(host_storage);
    update_status_from_storage();
    return core_status;
}

} // namespace vita3k::ios
