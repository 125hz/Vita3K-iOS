#include <vita3k_ios/CoreBridge.h>
#include <vita3k_ios/ArmExecution.h>
#include <vita3k_ios/ExecutableLoader.h>
#include <vita3k_ios/ExecutableProbe.h>
#include <vita3k_ios/GuestMemory.h>
#include <vita3k_ios/HostFilesystem.h>
#include <vita3k_ios/ModuleTableParser.h>
#include <vita3k_ios/RelocationEngine.h>

#include <nids/functions.h>
#include <util/arm.h>

#include <algorithm>
#include <array>
#include <cstring>
#include <limits>
#include <memory>
#include <mutex>
#include <sstream>
#include <string_view>
#include <utility>

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

template <typename T, std::size_t Size>
void write_value(std::array<std::uint8_t, Size> &bytes, std::size_t offset, T value) {
    std::memcpy(bytes.data() + offset, &value, sizeof(value));
}

bool run_segment_mapping_test(GuestMemory &memory, std::string &error) {
    constexpr std::uint32_t test_address = 0x10000;
    constexpr std::array<std::uint8_t, 16> first_file_bytes{
        0x7F, 'E', 'L', 'F', 0x56, 0x49, 0x54, 0x41,
        0x33, 0x4B, 0x2D, 0x69, 0x4F, 0x53, 0x01, 0x00
    };
    constexpr std::array<std::uint8_t, 16> second_file_bytes{
        0x4D, 0x4F, 0x44, 0x55, 0x4C, 0x45, 0x2D, 0x49,
        0x4E, 0x46, 0x4F, 0x2D, 0x54, 0x45, 0x53, 0x54
    };
    const auto half_page_64 = static_cast<std::uint64_t>(memory.host_page_size()) / 2;
    if (half_page_64 < 32 || half_page_64 > std::numeric_limits<std::uint32_t>::max()) {
        error = "The host page size is invalid for the shared-page segment diagnostic.";
        return false;
    }
    const auto half_page = static_cast<std::uint32_t>(half_page_64);
    const std::array mappings{
        GuestSegmentMapping{
            .guest_address = test_address,
            .file_data = first_file_bytes,
            .memory_size = half_page,
            .guest_flags = 5
        },
        GuestSegmentMapping{
            .guest_address = test_address + half_page,
            .file_data = second_file_bytes,
            .memory_size = half_page,
            .guest_flags = 6
        }
    };
    if (!memory.map_segments(mappings, error)) {
        return false;
    }

    std::array<std::uint8_t, 32> first_observed{};
    std::array<std::uint8_t, 32> second_observed{};
    const bool first_read_ok = memory.read(test_address, first_observed, error);
    const bool second_read_ok = memory.read(test_address + half_page, second_observed, error);
    const bool file_copy_ok = first_read_ok && second_read_ok &&
        std::equal(first_file_bytes.begin(), first_file_bytes.end(), first_observed.begin()) &&
        std::equal(second_file_bytes.begin(), second_file_bytes.end(), second_observed.begin());
    const bool first_bss_zeroed = first_read_ok &&
        std::all_of(first_observed.begin() + first_file_bytes.size(), first_observed.end(),
            [](std::uint8_t value) { return value == 0; });
    const bool second_bss_zeroed = second_read_ok &&
        std::all_of(second_observed.begin() + second_file_bytes.size(), second_observed.end(),
            [](std::uint8_t value) { return value == 0; });

    std::string unmap_error;
    const bool unmapped = memory.unmap_all_segments(unmap_error);
    if (!first_read_ok || !second_read_ok) {
        return false;
    }
    if (!file_copy_ok || !first_bss_zeroed || !second_bss_zeroed) {
        error = "The shared-page segment diagnostic failed copy or BSS verification.";
        return false;
    }
    if (!unmapped) {
        error = unmap_error;
        return false;
    }
    return true;
}

bool run_loader_pipeline_test(GuestMemory &memory, std::string &error) {
    constexpr std::uint32_t test_address = 0x20000;
    const auto memory_size_64 = static_cast<std::uint64_t>(memory.host_page_size());
    if (memory_size_64 < 0x200 || memory_size_64 > std::numeric_limits<std::uint32_t>::max()) {
        error = "The host page size is invalid for the loader pipeline diagnostic.";
        return false;
    }
    const auto memory_size = static_cast<std::uint32_t>(memory_size_64);

    std::array<std::uint8_t, 0x100> payload{};
    write_value(payload, 2, static_cast<std::uint16_t>(0x0101));
    constexpr char module_name[] = "ios-loader-selftest";
    std::memcpy(payload.data() + 4, module_name, sizeof(module_name) - 1);
    write_value(payload, 0x24, static_cast<std::uint32_t>(0x80));
    write_value(payload, 0x28, static_cast<std::uint32_t>(0xA0));
    write_value(payload, 0x2C, static_cast<std::uint32_t>(0xA0));
    write_value(payload, 0x30, static_cast<std::uint32_t>(0xD4));
    write_value(payload, 0x34, static_cast<std::uint32_t>(0x51F7A4D2));

    write_value(payload, 0x80, static_cast<std::uint16_t>(0x20));
    write_value(payload, 0x82, static_cast<std::uint16_t>(1));
    write_value(payload, 0x86, static_cast<std::uint16_t>(1));
    write_value(payload, 0x90, static_cast<std::uint32_t>(0xAABBCCDD));
    write_value(payload, 0x98, test_address + static_cast<std::uint32_t>(0xD4));
    write_value(payload, 0x9C, test_address + static_cast<std::uint32_t>(0xD8));

    write_value(payload, 0xA0, static_cast<std::uint16_t>(0x34));
    write_value(payload, 0xA2, static_cast<std::uint16_t>(1));
    write_value(payload, 0xA6, static_cast<std::uint16_t>(1));
    write_value(payload, 0xB0, static_cast<std::uint32_t>(0x11223344));
    write_value(payload, 0xBC, test_address + static_cast<std::uint32_t>(0xDC));
    write_value(payload, 0xC0, test_address + static_cast<std::uint32_t>(0xE0));
    write_value(payload, 0xD4, static_cast<std::uint32_t>(0x935CD196));
    write_value(payload, 0xD8, test_address + static_cast<std::uint32_t>(0x40));
    write_value(payload, 0xDC, static_cast<std::uint32_t>(0x210C0046));
    write_value(payload, 0xE0, test_address + static_cast<std::uint32_t>(0x44));

    constexpr std::uint32_t read_execute_flags = 5;
    if (!memory.map_segment(test_address, payload, memory_size, read_execute_flags, error)) {
        return false;
    }

    const std::array segment_plans{
        LoadSegmentPlan{
            .program_index = 0,
            .file_offset = 0,
            .virtual_address = test_address,
            .file_size = static_cast<std::uint32_t>(payload.size()),
            .memory_size = memory_size,
            .flags = read_execute_flags
        }
    };
    std::array<std::uint8_t, 12> relocation{};
    write_value(relocation, 0, static_cast<std::uint32_t>(0x00000200));
    write_value(relocation, 4, static_cast<std::uint32_t>(0x1234));
    write_value(relocation, 8, static_cast<std::uint32_t>(0xF0));
    const auto applied = apply_relocations(relocation, segment_plans, memory);
    const auto tables = parse_module_tables(memory, test_address, memory_size,
        std::span(payload).first(0x5C));

    std::array<std::uint8_t, 4> patched_bytes{};
    std::uint32_t patched_value = 0;
    const bool patch_read = memory.read(test_address + 0xF0, patched_bytes, error);
    if (patch_read) {
        std::memcpy(&patched_value, patched_bytes.data(), sizeof(patched_value));
    }

    std::string unmap_error;
    const bool unmapped = memory.unmap_all_segments(unmap_error);
    if (!applied.success) {
        error = applied.detail;
        return false;
    }
    if (!tables.success) {
        error = tables.detail;
        return false;
    }
    if (!patch_read || patched_value != test_address + 0x1234 ||
        applied.entry_count != 1 || applied.patched_value_count != 1 ||
        tables.export_library_count != 1 || tables.import_library_count != 1 ||
        tables.exported_nids.size() != 1 || tables.imported_nids.size() != 1) {
        error = "The relocation/module-table diagnostic produced unexpected results.";
        return false;
    }
    if (!unmapped) {
        error = unmap_error;
        return false;
    }
    return true;
}

bool run_arm_execution_test(GuestMemory &memory, std::uint64_t &instruction_count,
    std::size_t &hle_dispatch_count, std::string &error) {
    constexpr std::uint32_t test_address = 0x30000;
    constexpr std::uint32_t test_nid = 0x210C0046;
    constexpr std::uint32_t expected_argument = 0x1244;
    constexpr std::uint32_t expected_return = 0x600D;
    constexpr std::uint32_t expected_secondary_register = 0xBEEF;

    const std::array<std::uint32_t, 7> instructions{
        encode_arm_inst(INSTRUCTION_MOVW, 0x1234, 0),
        0xE2800010u, // ADD r0, r0, #0x10
        encode_arm_inst(INSTRUCTION_MOVW, test_nid & 0xFFFFu, 12),
        encode_arm_inst(INSTRUCTION_MOVT, test_nid >> 16, 12),
        encode_arm_inst(INSTRUCTION_SYSCALL, 0, 0),
        encode_arm_inst(INSTRUCTION_MOVW, expected_secondary_register, 1),
        encode_arm_inst(INSTRUCTION_BRANCH, 0, 14)
    };
    std::array<std::uint8_t, sizeof(instructions)> program{};
    std::memcpy(program.data(), instructions.data(), program.size());

    const auto memory_size_64 = static_cast<std::uint64_t>(memory.host_page_size());
    if (memory_size_64 < program.size() ||
        memory_size_64 > std::numeric_limits<std::uint32_t>::max()) {
        error = "The host page size is invalid for the ARM execution diagnostic.";
        return false;
    }
    if (!memory.map_segment(test_address, program,
            static_cast<std::uint32_t>(memory_size_64), 5, error)) {
        return false;
    }

    HLEDispatcher dispatcher;
    bool hle_argument_valid = false;
    hle_dispatch_count = 0;
    const bool bound = dispatcher.bind(test_nid, "__sceAppMgrGetAppState diagnostic",
        [&](ArmCpuState &state) -> std::int32_t {
            ++hle_dispatch_count;
            hle_argument_valid = state.registers[0] == expected_argument;
            return static_cast<std::int32_t>(expected_return);
        });

    ArmInterpreter interpreter(memory, dispatcher);
    interpreter.reset(test_address, test_address + static_cast<std::uint32_t>(memory_size_64));
    const auto execution = interpreter.run(32);
    const auto &state = interpreter.state();
    instruction_count = execution.instructions_executed;

    std::string unmap_error;
    const bool unmapped = memory.unmap_all_segments(unmap_error);
    if (!bound || dispatcher.binding_count() != 1) {
        error = "The diagnostic HLE binding could not be registered.";
        return false;
    }
    if (!execution.halted()) {
        error = execution.detail;
        return false;
    }
    if (!hle_argument_valid || hle_dispatch_count != 1 ||
        state.registers[0] != expected_return ||
        state.registers[1] != expected_secondary_register ||
        state.registers[12] != test_nid ||
        instruction_count != instructions.size()) {
        error = "The ARM execution/HLE diagnostic produced unexpected register state.";
        return false;
    }
    if (!unmapped) {
        error = unmap_error;
        return false;
    }
    return true;
}

void append_storage_error(std::string message) {
    if (!host_storage.error.empty()) {
        host_storage.error += " | ";
    }
    host_storage.error += std::move(message);
}

void update_status_from_storage() {
    if (guest_memory && guest_memory->mapped_segment_count() != 0) {
        std::string unmap_error;
        if (!guest_memory->unmap_all_segments(unmap_error)) {
            append_storage_error("Guest executable unload: " + unmap_error);
        }
    }

    core_status.storage_ready = host_storage.ready;
    core_status.storage_root = host_storage.root.string();
    core_status.imported_artifacts.clear();
    core_status.imported_artifacts.reserve(host_storage.imported_files.size());

    std::string loaded_filename;
    for (const auto &entry : host_storage.imported_files) {
        std::error_code error;
        const auto size = entry.file_size(error);
        const auto probe = probe_artifact(entry.path());
        PlainElfLoadResult load;
        if (loaded_filename.empty() && guest_memory && core_status.loader_pipeline_ready &&
            probe.structurally_valid && probe.kind == "Vita ELF") {
            load = load_plain_elf(entry.path(), probe, *guest_memory);
            if (load.loaded) {
                loaded_filename = entry.path().filename().string();
            }
        }

        std::string detail = probe.detail;
        if (load.attempted) {
            detail += " Loader: " + load.detail;
        }
        core_status.imported_artifacts.push_back({
            .filename = entry.path().filename().string(),
            .size = error ? 0 : size,
            .kind = probe.kind,
            .structurally_valid = probe.structurally_valid,
            .load_segment_count = probe.load_segments.size(),
            .load_attempted = load.attempted,
            .loaded = load.loaded,
            .module_info_valid = load.module_info_valid,
            .relocations_applied = load.relocations_applied,
            .module_tables_parsed = load.module_tables_parsed,
            .module_name = load.module_name,
            .module_nid = load.module_nid,
            .relocation_segment_count = probe.relocation_segments.size(),
            .relocation_entry_count = load.relocation_entry_count,
            .relocation_patch_count = load.relocation_patch_count,
            .export_library_count = load.export_library_count,
            .import_library_count = load.import_library_count,
            .exported_nid_count = load.exported_nid_count,
            .imported_nid_count = load.imported_nid_count,
            .detail = std::move(detail)
        });
    }

    std::ostringstream summary;
    summary << "Core slice: ARM encoder + Vita NID database\n"
            << "Self-tests: " << (core_status.self_tests_passed ? "passed" : "FAILED") << "\n"
            << "Guest memory: " << (core_status.guest_memory_ready ? "ready" : "FAILED");
    if (core_status.guest_memory_ready) {
        summary << " (" << (core_status.guest_memory_size >> 30) << " GiB reserved, "
                << core_status.host_page_size << "-byte pages)";
    }
    summary << "\nBatch segment map: " << (core_status.segment_mapping_ready
        ? "passed (shared-page permission merge)"
        : "FAILED");
    summary << "\nRelocation/tables: " << (core_status.loader_pipeline_ready
        ? "passed (verified patch + 1 export/1 import)"
        : "FAILED");
    summary << "\nARM execution/HLE: " << (core_status.arm_execution_ready
        ? "passed (" + std::to_string(core_status.arm_test_instruction_count) +
            " instructions + " + std::to_string(core_status.hle_test_dispatch_count) +
            " bound NID call)"
        : "FAILED");
    summary << "\nStorage: " << (core_status.storage_ready ? "ready" : "FAILED") << "\n"
            << "Import candidates: " << core_status.imported_artifacts.size() << "\n"
            << "ELF loader: ";
    if (loaded_filename.empty()) {
        summary << "ready (waiting for a fixed-address plain ELF)";
    } else {
        summary << "loaded " << loaded_filename;
    }

    for (const auto &artifact : core_status.imported_artifacts) {
        summary << "\n  * " << artifact.filename << " - " << artifact.kind
                << " - " << (artifact.structurally_valid ? "header valid" : "not loadable")
                << " - " << artifact.load_segment_count << " load segments"
                << " - " << (artifact.loaded ? "MAPPED" : "not mapped")
                << "\n    " << artifact.detail;
    }
    summary << "\n\nNext: run a mapped homebrew entry point with thread state and real HLE stubs. Rendering is not active yet.";
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
    const bool loader_pipeline_passed = segment_mapping_passed &&
        run_loader_pipeline_test(*guest_memory, memory_error);
    std::string execution_error;
    const bool arm_execution_passed = loader_pipeline_passed &&
        run_arm_execution_test(*guest_memory, core_status.arm_test_instruction_count,
            core_status.hle_test_dispatch_count, execution_error);
    core_status.guest_memory_ready = reserved && protection_test_passed;
    core_status.segment_mapping_ready = segment_mapping_passed;
    core_status.loader_pipeline_ready = loader_pipeline_passed;
    core_status.arm_execution_ready = arm_execution_passed;
    core_status.guest_memory_size = reserved ? guest_memory->size() : 0;
    core_status.host_page_size = reserved ? guest_memory->host_page_size() : 0;
    host_storage = initialize_host_storage(documents_root);
    if (!memory_error.empty()) {
        append_storage_error("Guest memory: " + memory_error);
    }
    if (!execution_error.empty()) {
        append_storage_error("ARM execution: " + execution_error);
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
