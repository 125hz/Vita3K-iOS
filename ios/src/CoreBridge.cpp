#include <vita3k_ios/CoreBridge.h>
#include <vita3k_ios/ArmExecution.h>
#include <vita3k_ios/ExecutableLoader.h>
#include <vita3k_ios/ExecutableProbe.h>
#include <vita3k_ios/GuestMemory.h>
#include <vita3k_ios/GuestThread.h>
#include <vita3k_ios/HostDisplay.h>
#include <vita3k_ios/HostFilesystem.h>
#include <vita3k_ios/HostInput.h>
#include <vita3k_ios/ImportBinder.h>
#include <vita3k_ios/ModuleTableParser.h>
#include <vita3k_ios/RelocationEngine.h>
#include <vita3k_ios/VitaAppMetadata.h>
#include <vita3k_ios/VitaAppArchive.h>

#include <nids/functions.h>
#include <packages/archive.h>
#include <util/arm.h>

#include <algorithm>
#include <array>
#include <cstring>
#include <iomanip>
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
HostDisplay host_display;
HostInput host_input;
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

bool run_upstream_metadata_test() {
    const auto fixture = make_synthetic_param_sfo(
        "M13TEST01", "Vita3K iOS upstream metadata probe");
    const auto metadata = parse_vita_app_metadata(fixture);
    return metadata.parsed && metadata.title_id == "M13TEST01" &&
        metadata.title == "Vita3K iOS upstream metadata probe" &&
        metadata.category == "gd" && metadata.app_version == "01.00";
}

bool run_upstream_archive_test() {
    const auto fixture = make_synthetic_vpk();
    const auto archive = packages::inspect_archive(fixture);
    const auto unsafe = packages::inspect_archive(make_synthetic_vpk(true));
    return archive.inspected && archive.valid && archive.file_count == 3 &&
        archive.applications.size() == 1 &&
        archive.applications.front().title_id == "M14TEST01" &&
        archive.applications.front().title == "Vita3K iOS archive inspection probe" &&
        archive.applications.front().install_target == "ux0/app/M14TEST01" &&
        unsafe.inspected && !unsafe.valid && unsafe.unsafe_path_count == 1;
}

void update_renderer_status() {
    const auto display = host_display.status();
    core_status.renderer_attached = display.attached;
    core_status.renderer_frame_presented = display.first_frame_presented;
    core_status.renderer_submitted_frame_count = display.submitted_frame_count;
    core_status.renderer_presented_frame_count = display.presented_frame_count;

    const auto marker = core_status.summary.find("\nRenderer:");
    if (marker != std::string::npos) {
        const auto end = core_status.summary.find('\n', marker + 1);
        core_status.summary.erase(marker,
            end == std::string::npos ? std::string::npos : end - marker);
    }
    std::ostringstream line;
    line << "\nRenderer: ";
    if (display.first_frame_presented) {
        line << "passed (first core-owned Metal frame presented)";
    } else if (display.frame_in_flight) {
        line << "ready (diagnostic frame in flight)";
    } else if (display.attached) {
        line << "ready (Metal host attached)";
    } else {
        line << "waiting for MTKView host";
    }
    const auto storage = core_status.summary.find("\nStorage:");
    core_status.summary.insert(storage == std::string::npos ? core_status.summary.size() : storage,
        line.str());
}

void update_input_status() {
    const auto input = host_input.status();
    core_status.input_surface_attached = input.touch_surface_attached;
    core_status.input_touch_received = input.touch_sample_received;
    core_status.input_controller_connected = input.controller_connected;
    core_status.input_controller_received = input.controller_sample_received;
    core_status.input_touch_sample_count = input.touch_sample_count;
    core_status.input_controller_sample_count = input.controller_sample_count;
    core_status.input_last_touch_x = input.last_touch_x;
    core_status.input_last_touch_y = input.last_touch_y;

    const auto marker = core_status.summary.find("\nInput:");
    if (marker != std::string::npos) {
        const auto end = core_status.summary.find('\n', marker + 1);
        core_status.summary.erase(marker,
            end == std::string::npos ? std::string::npos : end - marker);
    }

    std::ostringstream line;
    line << "\nInput: ";
    if (!input.touch_surface_attached) {
        line << "waiting for UIKit touch surface";
    } else if (!input.touch_sample_received && !input.controller_sample_received) {
        line << "ready (tap the blue background; controller "
             << (input.controller_connected ? "connected" : "waiting") << ")";
    } else {
        line << "passed (" << input.touch_sample_count << " touch samples";
        if (input.touch_sample_received) {
            line << "; last x=" << std::fixed << std::setprecision(3) << input.last_touch_x
                 << " y=" << input.last_touch_y;
        }
        line << "; controller ";
        if (input.controller_sample_received) {
            line << input.controller_sample_count << " samples/"
                 << (input.controller_connected ? "connected" : "disconnected");
        } else {
            line << (input.controller_connected ? "connected" : "waiting");
        }
        line << ")";
    }
    const auto storage = core_status.summary.find("\nStorage:");
    core_status.summary.insert(storage == std::string::npos ? core_status.summary.size() : storage,
        line.str());
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

bool run_loader_pipeline_test(GuestMemory &memory, ImportBindingResult &binding_result,
    GuestThreadRunResult &thread_result, std::string &error) {
    constexpr std::uint32_t test_address = 0x20000;
    constexpr std::uint32_t module_start = 0x61;
    constexpr std::uint32_t get_thread_id_nid = 0x0FB972F9;
    constexpr std::uint32_t exit_thread_nid = 0x0C8A38E1;
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
    write_value(payload, 0x44, module_start);

    const std::array<std::uint16_t, 9> guest_program{
        0xB510u, // PUSH {r4, lr}
        0x4B04u, // LDR r3, [pc, #16]: address of ARM get-thread-ID stub
        0x4798u, // BLX r3: enter the ARM import trampoline
        0x4604u, // MOV r4, r0
        0x9400u, // STR r4, [sp]
        0x9A00u, // LDR r2, [sp]
        0x202Au, // MOVS r0, #42
        0x4B02u, // LDR r3, [pc, #8]: address of ARM exit-thread stub
        0x4798u // BLX r3
    };
    std::memcpy(payload.data() + (module_start & ~1u), guest_program.data(),
        sizeof(guest_program));
    write_value(payload, 0x74, test_address + static_cast<std::uint32_t>(0x40));
    write_value(payload, 0x78, test_address + static_cast<std::uint32_t>(0x50));

    write_value(payload, 0x80, static_cast<std::uint16_t>(0x20));
    write_value(payload, 0x82, static_cast<std::uint16_t>(1));
    write_value(payload, 0x86, static_cast<std::uint16_t>(1));
    write_value(payload, 0x90, static_cast<std::uint32_t>(0xAABBCCDD));
    write_value(payload, 0x98, test_address + static_cast<std::uint32_t>(0xD4));
    write_value(payload, 0x9C, test_address + static_cast<std::uint32_t>(0xD8));

    write_value(payload, 0xA0, static_cast<std::uint16_t>(0x34));
    write_value(payload, 0xA2, static_cast<std::uint16_t>(1));
    write_value(payload, 0xA6, static_cast<std::uint16_t>(2));
    write_value(payload, 0xB0, static_cast<std::uint32_t>(0x11223344));
    write_value(payload, 0xBC, test_address + static_cast<std::uint32_t>(0xDC));
    write_value(payload, 0xC0, test_address + static_cast<std::uint32_t>(0xE4));
    write_value(payload, 0xD4, static_cast<std::uint32_t>(0x935CD196));
    write_value(payload, 0xD8, test_address + static_cast<std::uint32_t>(0x40));
    write_value(payload, 0xDC, get_thread_id_nid);
    write_value(payload, 0xE0, exit_thread_nid);
    write_value(payload, 0xE4, test_address + static_cast<std::uint32_t>(0x40));
    write_value(payload, 0xE8, test_address + static_cast<std::uint32_t>(0x50));

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
    if (tables.success) {
        binding_result = bind_import_stubs(memory, tables.imported_function_stubs);
        if (binding_result.success) {
            thread_result = run_guest_module_start(memory, test_address + module_start,
                test_address + memory_size, tables.imported_nids,
                "ios-compiled-homebrew-selftest");
        }
    }

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
        tables.exported_nids.size() != 1 || tables.imported_nids.size() != 2 ||
        tables.imported_function_stubs.size() != 2) {
        error = "The relocation/module-table diagnostic produced unexpected results.";
        return false;
    }
    if (!binding_result.success || binding_result.bound_function_count != 2) {
        error = "The compiled import-stub diagnostic failed: " + binding_result.detail;
        return false;
    }
    if (!thread_result.started || !thread_result.exited ||
        thread_result.instruction_count != 12 ||
        thread_result.hle_dispatch_count != 2 || thread_result.exit_status != 42 ||
        thread_result.observed_thread_id != static_cast<std::uint32_t>(thread_result.thread_id)) {
        error = "The loaded module_start/thread diagnostic failed: " + thread_result.detail;
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
        VitaAppMetadata metadata;
        packages::ArchiveInspection archive;
        if (probe.kind == "PARAM.SFO" && probe.recognized) {
            metadata = parse_vita_app_metadata(entry.path());
        } else if (probe.kind == "VPK/ZIP" && probe.recognized) {
            archive = packages::inspect_archive(entry.path());
            if (archive.valid && !archive.applications.empty()) {
                const auto &app = archive.applications.front();
                metadata = {
                    .parsed = true,
                    .title_id = app.title_id,
                    .title = app.title,
                    .category = app.category,
                    .app_version = app.app_version,
                    .detail = archive.detail
                };
            }
        }
        PlainElfLoadResult load;
        GuestThreadRunResult thread;
        if (loaded_filename.empty() && guest_memory && core_status.loader_pipeline_ready &&
            probe.structurally_valid && probe.kind == "Vita ELF") {
            load = load_plain_elf(entry.path(), probe, *guest_memory);
            if (load.loaded) {
                loaded_filename = entry.path().filename().string();
                if (load.module_start_valid && load.temporary_stack_pointer != 0) {
                    thread = run_guest_module_start(*guest_memory, load.module_start_address,
                        load.temporary_stack_pointer, load.imported_nids, load.module_name);
                }
            }
        }

        std::string detail = probe.detail;
        if (probe.kind == "PARAM.SFO" && probe.recognized) {
            detail += " " + metadata.detail;
        } else if (archive.inspected) {
            detail += " " + archive.detail;
        }
        if (load.attempted) {
            detail += " Loader: " + load.detail;
        }
        if (thread.attempted) {
            detail += " Thread: " + thread.detail;
        }
        core_status.imported_artifacts.push_back({
            .filename = entry.path().filename().string(),
            .size = error ? 0 : size,
            .kind = probe.kind,
            .app_metadata_parsed = metadata.parsed,
            .app_title_id = std::move(metadata.title_id),
            .app_title = std::move(metadata.title),
            .app_category = std::move(metadata.category),
            .app_version = std::move(metadata.app_version),
            .archive_inspected = archive.inspected,
            .archive_valid = archive.valid,
            .archive_file_count = archive.file_count,
            .archive_application_count = archive.applications.size(),
            .archive_unsafe_path_count = archive.unsafe_path_count,
            .archive_uncompressed_size = archive.uncompressed_size,
            .archive_install_target = archive.applications.empty()
                ? std::string{} : archive.applications.front().install_target,
            .structurally_valid = probe.structurally_valid,
            .load_segment_count = probe.load_segments.size(),
            .load_attempted = load.attempted,
            .loaded = load.loaded,
            .module_info_valid = load.module_info_valid,
            .relocations_applied = load.relocations_applied,
            .module_tables_parsed = load.module_tables_parsed,
            .import_stubs_bound = load.import_stubs_bound,
            .module_start_valid = load.module_start_valid,
            .execution_attempted = thread.attempted,
            .thread_exited = thread.exited,
            .thread_returned = thread.returned,
            .module_name = load.module_name,
            .module_nid = load.module_nid,
            .relocation_segment_count = probe.relocation_segments.size(),
            .relocation_entry_count = load.relocation_entry_count,
            .relocation_patch_count = load.relocation_patch_count,
            .export_library_count = load.export_library_count,
            .import_library_count = load.import_library_count,
            .exported_nid_count = load.exported_nid_count,
            .imported_nid_count = load.imported_nid_count,
            .bound_import_stub_count = load.bound_import_stub_count,
            .module_start_address = load.module_start_address,
            .executed_instruction_count = thread.instruction_count,
            .hle_dispatch_count = thread.hle_dispatch_count,
            .thread_exit_status = thread.exit_status,
            .thread_return_value = thread.return_value,
            .detail = std::move(detail)
        });
    }

    std::ostringstream summary;
    summary << "Core slice: ARM encoder + Vita NID database\n"
            << "Self-tests: " << (core_status.self_tests_passed ? "passed" : "FAILED") << "\n"
            << "Upstream app metadata: " << (core_status.upstream_metadata_ready
                ? "passed (Vita3K packages/SFO parser linked)"
                : "FAILED") << "\n"
            << "Upstream app archive: " << (core_status.upstream_archive_ready
                ? "passed (miniz + Vita3K package inspector linked)"
                : "FAILED") << "\n"
            << "Guest memory: " << (core_status.guest_memory_ready ? "ready" : "FAILED");
    if (core_status.guest_memory_ready) {
        summary << " (" << (core_status.guest_memory_size >> 30) << " GiB reserved, "
                << core_status.host_page_size << "-byte pages)";
    }
    summary << "\nBatch segment map: " << (core_status.segment_mapping_ready
        ? "passed (shared-page permission merge)"
        : "FAILED");
    summary << "\nRelocation/tables: " << (core_status.loader_pipeline_ready
        ? "passed (verified patch + 1 export/1 import library)"
        : "FAILED");
    summary << "\nCompiled import stubs: " << (core_status.import_binding_ready
        ? "passed (" + std::to_string(core_status.thread_test_bound_stub_count) +
            " SVC trampolines)"
        : "FAILED");
    summary << "\nARM execution/HLE: " << (core_status.arm_execution_ready
        ? "passed (" + std::to_string(core_status.arm_test_instruction_count) +
            " instructions + " + std::to_string(core_status.hle_test_dispatch_count) +
            " bound NID call)"
        : "FAILED");
    summary << "\nThumb/ARM entry/thread: " << (core_status.guest_thread_ready
        ? "passed (" + std::to_string(core_status.thread_test_instruction_count) +
            " instructions + " + std::to_string(core_status.thread_test_hle_dispatch_count) +
            " kernel HLE calls + exit " + std::to_string(core_status.thread_test_exit_status) + ")"
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
                << " - " << artifact.load_segment_count << " load segment"
                << (artifact.load_segment_count == 1 ? "" : "s")
                << " - " << (artifact.loaded ? "MAPPED" : "not mapped")
                << "\n    " << artifact.detail;
    }
    summary << "\n\nNext: add an explicit sandbox installation transaction, then connect encrypted SELF loading. General Vita homebrew and games are not active yet.";
    if (!host_storage.error.empty()) {
        summary << "\nStorage error: " << host_storage.error;
    }
    core_status.summary = summary.str();
    update_renderer_status();
    update_input_status();
}

} // namespace

CoreStatus initialize_core(const std::filesystem::path &documents_root) {
    std::lock_guard lock(core_mutex);
    host_display = HostDisplay{};
    host_input = HostInput{};
    core_status.self_tests_passed = run_upstream_self_tests();
    core_status.upstream_metadata_ready = run_upstream_metadata_test();
    core_status.upstream_archive_ready = run_upstream_archive_test();
    guest_memory = std::make_unique<GuestMemory>();
    std::string memory_error;
    constexpr std::uint64_t vita_address_space_size = 1ULL << 32;
    const bool reserved = guest_memory->reserve(vita_address_space_size, memory_error);
    const bool protection_test_passed = reserved && guest_memory->run_commit_protection_test(memory_error);
    const bool segment_mapping_passed = protection_test_passed &&
        run_segment_mapping_test(*guest_memory, memory_error);
    GuestThreadRunResult thread_test;
    ImportBindingResult binding_test;
    const bool loader_pipeline_passed = segment_mapping_passed &&
        run_loader_pipeline_test(*guest_memory, binding_test, thread_test, memory_error);
    std::string execution_error;
    const bool arm_execution_passed = loader_pipeline_passed &&
        run_arm_execution_test(*guest_memory, core_status.arm_test_instruction_count,
            core_status.hle_test_dispatch_count, execution_error);
    core_status.guest_memory_ready = reserved && protection_test_passed;
    core_status.segment_mapping_ready = segment_mapping_passed;
    core_status.loader_pipeline_ready = loader_pipeline_passed;
    core_status.import_binding_ready = loader_pipeline_passed && binding_test.success &&
        binding_test.bound_function_count == 2;
    core_status.arm_execution_ready = arm_execution_passed;
    core_status.guest_thread_ready = loader_pipeline_passed && thread_test.exited;
    core_status.thread_test_instruction_count = thread_test.instruction_count;
    core_status.thread_test_hle_dispatch_count = thread_test.hle_dispatch_count;
    core_status.thread_test_exit_status = thread_test.exit_status;
    core_status.thread_test_bound_stub_count = binding_test.bound_function_count;
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

bool attach_host_display(std::uint32_t width, std::uint32_t height, std::string &error) {
    std::lock_guard lock(core_mutex);
    const bool attached = host_display.attach(width, height, error);
    update_renderer_status();
    return attached;
}

std::optional<HostDisplayFrame> acquire_host_display_frame(std::uint32_t width,
    std::uint32_t height, std::string &error) {
    std::lock_guard lock(core_mutex);
    auto frame = host_display.acquire_frame(width, height, error);
    update_renderer_status();
    return frame;
}

bool complete_host_display_frame(std::uint64_t identifier, bool presented,
    std::string detail, std::string &error) {
    std::lock_guard lock(core_mutex);
    const bool completed = host_display.complete_frame(identifier, presented,
        std::move(detail), error);
    update_renderer_status();
    return completed;
}

bool attach_host_input_surface(double width, double height, std::string &error) {
    std::lock_guard lock(core_mutex);
    const bool attached = host_input.attach_touch_surface(width, height, error);
    update_input_status();
    return attached;
}

bool submit_host_touch(std::uint64_t identifier, double x, double y, HostTouchPhase phase,
    std::string &error) {
    std::lock_guard lock(core_mutex);
    const bool submitted = host_input.submit_touch(identifier, x, y, phase, error);
    update_input_status();
    return submitted;
}

void set_host_controller_connected(bool connected) {
    std::lock_guard lock(core_mutex);
    host_input.set_controller_connected(connected);
    update_input_status();
}

bool submit_host_controller(HostControllerSample sample, std::string &error) {
    std::lock_guard lock(core_mutex);
    const bool submitted = host_input.submit_controller(sample, error);
    update_input_status();
    return submitted;
}

} // namespace vita3k::ios
