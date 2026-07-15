#include <vita3k_ios/ArmExecution.h>
#include <vita3k_ios/CoreBridge.h>
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
#include <vita3k_ios/VitaAppArchive.h>
#include <vita3k_ios/VitaAppMetadata.h>

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

struct PreparedExecutableState {
    bool ready{};
    std::string title_id;
    std::string module_name;
    std::uint32_t module_start_address{};
    std::uint32_t temporary_stack_pointer{};
    std::vector<std::uint32_t> imported_nids;
};

PreparedExecutableState prepared_executable;
constexpr std::size_t maximum_controlled_boot_instructions = 256;
constexpr std::uint32_t flag_negative = 1u << 31;
constexpr std::uint32_t flag_zero = 1u << 30;
constexpr std::uint32_t flag_carry = 1u << 29;
constexpr std::uint32_t flag_overflow = 1u << 28;

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
    return metadata.parsed && metadata.title_id == "M13TEST01" && metadata.title == "Vita3K iOS upstream metadata probe" && metadata.category == "gd" && metadata.app_version == "01.00";
}

bool run_upstream_archive_test() {
    const auto fixture = make_synthetic_vpk();
    const auto archive = packages::inspect_archive(fixture);
    const auto unsafe = packages::inspect_archive(make_synthetic_vpk(true));
    return archive.inspected && archive.valid && archive.file_count == 3 && archive.applications.size() == 1 && archive.applications.front().title_id == "M14TEST01" && archive.applications.front().title == "Vita3K iOS archive inspection probe" && archive.applications.front().install_target == "ux0/app/M14TEST01" && unsafe.inspected && !unsafe.valid && unsafe.unsafe_path_count == 1;
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
            .guest_flags = 5 },
        GuestSegmentMapping{
            .guest_address = test_address + half_page,
            .file_data = second_file_bytes,
            .memory_size = half_page,
            .guest_flags = 6 }
    };
    if (!memory.map_segments(mappings, error)) {
        return false;
    }

    std::array<std::uint8_t, 32> first_observed{};
    std::array<std::uint8_t, 32> second_observed{};
    const bool first_read_ok = memory.read(test_address, first_observed, error);
    const bool second_read_ok = memory.read(test_address + half_page, second_observed, error);
    const bool file_copy_ok = first_read_ok && second_read_ok && std::equal(first_file_bytes.begin(), first_file_bytes.end(), first_observed.begin()) && std::equal(second_file_bytes.begin(), second_file_bytes.end(), second_observed.begin());
    const bool first_bss_zeroed = first_read_ok && std::all_of(first_observed.begin() + first_file_bytes.size(), first_observed.end(), [](std::uint8_t value) { return value == 0; });
    const bool second_bss_zeroed = second_read_ok && std::all_of(second_observed.begin() + second_file_bytes.size(), second_observed.end(), [](std::uint8_t value) { return value == 0; });

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

    const std::array<std::uint16_t, 10> guest_program{
        0xE92Du, // PUSH.W {r8, lr}, first halfword
        0x4100u, // PUSH.W register list: high register r8 and lr
        0x4B03u, // LDR r3, [pc, #12]: address of ARM get-thread-ID stub
        0x4798u, // BLX r3: enter the ARM import trampoline
        0x4604u, // MOV r4, r0
        0x9400u, // STR r4, [sp]
        0x9A00u, // LDR r2, [sp]
        0x202Au, // MOVS r0, #42
        0x4B01u, // LDR r3, [pc, #4]: address of ARM exit-thread stub
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
            .flags = read_execute_flags }
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
    if (!patch_read || patched_value != test_address + 0x1234 || applied.entry_count != 1 || applied.patched_value_count != 1 || tables.export_library_count != 1 || tables.import_library_count != 1 || tables.exported_nids.size() != 1 || tables.imported_nids.size() != 2 || tables.imported_function_stubs.size() != 2) {
        error = "The relocation/module-table diagnostic produced unexpected results.";
        return false;
    }
    if (!binding_result.success || binding_result.bound_function_count != 2) {
        error = "The compiled import-stub diagnostic failed: " + binding_result.detail;
        return false;
    }
    if (!thread_result.started || !thread_result.exited || thread_result.instruction_count != 12 || thread_result.hle_dispatch_count != 2 || thread_result.exit_status != 42 || thread_result.observed_thread_id != static_cast<std::uint32_t>(thread_result.thread_id)) {
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
    if (memory_size_64 < program.size() || memory_size_64 > std::numeric_limits<std::uint32_t>::max()) {
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
    if (!hle_argument_valid || hle_dispatch_count != 1 || state.registers[0] != expected_return || state.registers[1] != expected_secondary_register || state.registers[12] != test_nid || instruction_count != instructions.size()) {
        error = "The ARM execution/HLE diagnostic produced unexpected register state.";
        return false;
    }
    if (!unmapped) {
        error = unmap_error;
        return false;
    }
    return true;
}

bool run_inline_hle_nid_diagnostic_test(GuestMemory &memory, std::string &error) {
    constexpr std::uint32_t test_address = 0x38000;
    constexpr std::uint32_t test_nid = 0x210C0046;
    constexpr std::uint32_t link_address = 0x12345679;
    const std::array<std::uint32_t, 3> trampoline{
        0xEF000000u, // SVC #0
        0xE1A0F00Eu, // MOV pc, lr
        test_nid
    };
    std::array<std::uint8_t, sizeof(trampoline)> program{};
    std::memcpy(program.data(), trampoline.data(), program.size());

    const auto memory_size_64 = static_cast<std::uint64_t>(memory.host_page_size());
    if (memory_size_64 < program.size() || memory_size_64 > std::numeric_limits<std::uint32_t>::max()) {
        error = "The host page size is invalid for the inline HLE NID diagnostic.";
        return false;
    }
    if (!memory.map_segment(test_address, program,
            static_cast<std::uint32_t>(memory_size_64), 5, error)) {
        return false;
    }

    HLEDispatcher dispatcher;
    ArmInterpreter interpreter(memory, dispatcher);
    interpreter.reset(test_address, test_address + static_cast<std::uint32_t>(memory_size_64), link_address);
    interpreter.state().registers[0] = 0x11111111u;
    interpreter.state().registers[1] = 0x22222222u;
    interpreter.state().registers[2] = 0x33333333u;
    interpreter.state().registers[3] = 0x44444444u;
    interpreter.state().registers[12] = 0;
    const auto execution = interpreter.run(1);

    std::string unmap_error;
    const bool unmapped = memory.unmap_all_segments(unmap_error);
    const bool valid = execution.reason == ArmStopReason::unbound_hle && execution.instructions_executed == 1 && execution.last_hle_nid == test_nid && execution.final_pc == test_address + 4u && execution.detail.find("resolved from inline import trampoline") != std::string::npos && execution.detail.find("LR=0x12345679") != std::string::npos && execution.detail.find("r0=0x11111111") != std::string::npos && execution.detail.find("r3=0x44444444") != std::string::npos && execution.detail.find("r12=0x00000000") != std::string::npos;
    if (!valid) {
        error = "The inline HLE NID diagnostic did not preserve the trampoline identity and call context.";
        return false;
    }
    if (!unmapped) {
        error = unmap_error;
        return false;
    }
    return true;
}

bool run_thumb2_wide_push_test(GuestMemory &memory, std::string &error) {
    constexpr std::uint32_t test_address = 0x40000;
    constexpr std::uint32_t r0_value = 0x11111111;
    constexpr std::uint32_t r8_value = 0x88888888;
    constexpr std::uint32_t lr_value = 0xEEEEEEEE;
    std::array<std::uint8_t, 12> program{};
    write_value(program, 0, static_cast<std::uint16_t>(0xE92D));
    write_value(program, 2, static_cast<std::uint16_t>(0x4101)); // PUSH.W {r0, r8, lr}
    write_value(program, 8, static_cast<std::uint16_t>(0xE8BF));
    write_value(program, 10, static_cast<std::uint16_t>(0xA55A));

    const auto memory_size_64 = static_cast<std::uint64_t>(memory.host_page_size());
    if (memory_size_64 < program.size() || memory_size_64 > std::numeric_limits<std::uint32_t>::max()) {
        error = "The host page size is invalid for the Thumb-2 PUSH.W diagnostic.";
        return false;
    }
    const auto memory_size = static_cast<std::uint32_t>(memory_size_64);
    if (!memory.map_segment(test_address, program, memory_size, 7, error)) {
        return false;
    }

    HLEDispatcher dispatcher;
    ArmInterpreter interpreter(memory, dispatcher);
    const auto initial_sp = test_address + memory_size;
    interpreter.reset(test_address | 1u, initial_sp, lr_value);
    interpreter.state().registers[0] = r0_value;
    interpreter.state().registers[8] = r8_value;
    const auto wide_execution = interpreter.run(1);
    const auto wide_state = interpreter.state();

    std::array<std::uint8_t, 12> saved_bytes{};
    std::array<std::uint32_t, 3> saved_registers{};
    const bool stack_read = memory.read(
        initial_sp - static_cast<std::uint32_t>(saved_bytes.size()), saved_bytes, error);
    if (stack_read) {
        std::memcpy(saved_registers.data(), saved_bytes.data(), saved_bytes.size());
    }

    interpreter.reset((test_address + 8u) | 1u, initial_sp);
    const auto unsupported_execution = interpreter.run(1);

    std::string unmap_error;
    const bool unmapped = memory.unmap_all_segments(unmap_error);
    if (wide_execution.reason != ArmStopReason::instruction_limit || wide_execution.instructions_executed != 1 || wide_state.registers[13] != initial_sp - 12u || wide_state.registers[15] != test_address + 4u || !stack_read || saved_registers[0] != r0_value || saved_registers[1] != r8_value || saved_registers[2] != lr_value) {
        error = "Thumb-2 PUSH.W produced unexpected stack or register state.";
        return false;
    }
    if (unsupported_execution.reason != ArmStopReason::unsupported_instruction || unsupported_execution.last_instruction != 0xA55AE8BFu || unsupported_execution.detail.find("0xE8BFA55A") == std::string::npos) {
        error = "The unpredictable Thumb-2 load-multiple diagnostic did not retain both halfwords.";
        return false;
    }
    if (!unmapped) {
        error = unmap_error;
        return false;
    }
    return true;
}

bool run_thumb_compiler_baseline_test(GuestMemory &memory, std::string &error) {
    constexpr std::uint32_t test_address = 0x50000;
    std::array<std::uint8_t, 0x100> program{};

    write_value(program, 0x00, static_cast<std::uint16_t>(0xB082)); // SUB sp, #8

    write_value(program, 0x20, static_cast<std::uint16_t>(0x2005)); // MOVS r0, #5
    write_value(program, 0x22, static_cast<std::uint16_t>(0x3003)); // ADDS r0, #3
    write_value(program, 0x24, static_cast<std::uint16_t>(0x3801)); // SUBS r0, #1
    write_value(program, 0x26, static_cast<std::uint16_t>(0x2807)); // CMP r0, #7
    write_value(program, 0x28, static_cast<std::uint16_t>(0xD000)); // BEQ +0
    write_value(program, 0x2A, static_cast<std::uint16_t>(0x21EE)); // skipped
    write_value(program, 0x2C, static_cast<std::uint16_t>(0x2101)); // MOVS r1, #1

    write_value(program, 0x40, static_cast<std::uint16_t>(0x2003)); // MOVS r0, #3
    write_value(program, 0x42, static_cast<std::uint16_t>(0x2104)); // MOVS r1, #4
    write_value(program, 0x44, static_cast<std::uint16_t>(0x1842)); // ADDS r2, r0, r1
    write_value(program, 0x46, static_cast<std::uint16_t>(0x0052)); // LSLS r2, r2, #1
    write_value(program, 0x48, static_cast<std::uint16_t>(0x404A)); // EORS r2, r1

    write_value(program, 0x60, static_cast<std::uint16_t>(0x6008)); // STR r0, [r1]
    write_value(program, 0x62, static_cast<std::uint16_t>(0x680A)); // LDR r2, [r1]
    write_value(program, 0x64, static_cast<std::uint16_t>(0x7108)); // STRB r0, [r1, #4]
    write_value(program, 0x66, static_cast<std::uint16_t>(0x790B)); // LDRB r3, [r1, #4]
    write_value(program, 0x68, static_cast<std::uint16_t>(0x80C8)); // STRH r0, [r1, #6]
    write_value(program, 0x6A, static_cast<std::uint16_t>(0x88CC)); // LDRH r4, [r1, #6]

    write_value(program, 0x80, static_cast<std::uint16_t>(0x5488)); // STRB r0, [r1, r2]
    write_value(program, 0x82, static_cast<std::uint16_t>(0x568B)); // LDRSB r3, [r1, r2]

    write_value(program, 0x90, static_cast<std::uint16_t>(0xC203)); // STMIA r2!, {r0,r1}
    write_value(program, 0x92, static_cast<std::uint16_t>(0x3A08)); // SUBS r2, #8
    write_value(program, 0x94, static_cast<std::uint16_t>(0xCA18)); // LDMIA r2!, {r3,r4}

    write_value(program, 0xA0, static_cast<std::uint16_t>(0x2000)); // MOVS r0, #0
    write_value(program, 0xA2, static_cast<std::uint16_t>(0xB100)); // CBZ r0, +0
    write_value(program, 0xA4, static_cast<std::uint16_t>(0x21EE)); // skipped
    write_value(program, 0xA6, static_cast<std::uint16_t>(0x2102)); // MOVS r1, #2

    write_value(program, 0xB0, static_cast<std::uint16_t>(0xE000)); // B +0
    write_value(program, 0xB2, static_cast<std::uint16_t>(0x21EE)); // skipped
    write_value(program, 0xB4, static_cast<std::uint16_t>(0x2103)); // MOVS r1, #3

    write_value(program, 0xC0, static_cast<std::uint16_t>(0xA801)); // ADD r0, sp, #4
    write_value(program, 0xD0, static_cast<std::uint16_t>(0x4480)); // ADD r8, r0
    write_value(program, 0xD2, static_cast<std::uint16_t>(0x4641)); // MOV r1, r8

    const auto memory_size_64 = static_cast<std::uint64_t>(memory.host_page_size());
    if (memory_size_64 < 0x400 || memory_size_64 > std::numeric_limits<std::uint32_t>::max()) {
        error = "The host page size is invalid for the Thumb compiler baseline diagnostic.";
        return false;
    }
    const auto memory_size = static_cast<std::uint32_t>(memory_size_64);
    if (!memory.map_segment(test_address, program, memory_size, 7, error)) {
        return false;
    }

    HLEDispatcher dispatcher;
    ArmInterpreter interpreter(memory, dispatcher);
    const auto initial_sp = test_address + memory_size;
    const auto data_address = test_address + 0x200u;
    const auto run_at = [&](std::uint32_t offset, std::size_t count) {
        interpreter.reset((test_address + offset) | 1u, initial_sp);
        return interpreter.run(count);
    };

    const auto stack_adjust = run_at(0x00, 1);
    const auto stack_adjust_state = interpreter.state();
    const auto arithmetic = run_at(0x20, 6);
    const auto arithmetic_state = interpreter.state();
    const auto alu = run_at(0x40, 5);
    const auto alu_state = interpreter.state();

    interpreter.reset((test_address + 0x60u) | 1u, initial_sp);
    interpreter.state().registers[0] = 0xA1B2C3D4u;
    interpreter.state().registers[1] = data_address;
    const auto memory_immediate = interpreter.run(6);
    const auto memory_immediate_state = interpreter.state();

    interpreter.reset((test_address + 0x80u) | 1u, initial_sp);
    interpreter.state().registers[0] = 0xFFFFFF80u;
    interpreter.state().registers[1] = data_address;
    interpreter.state().registers[2] = 8;
    const auto memory_register = interpreter.run(2);
    const auto memory_register_state = interpreter.state();

    interpreter.reset((test_address + 0x90u) | 1u, initial_sp);
    interpreter.state().registers[0] = 0x11111111u;
    interpreter.state().registers[1] = 0x22222222u;
    interpreter.state().registers[2] = data_address + 0x20u;
    const auto multiple = interpreter.run(3);
    const auto multiple_state = interpreter.state();

    const auto compare_branch = run_at(0xA0, 3);
    const auto compare_branch_state = interpreter.state();
    const auto branch = run_at(0xB0, 2);
    const auto branch_state = interpreter.state();
    const auto stack_address = run_at(0xC0, 1);
    const auto stack_address_state = interpreter.state();

    interpreter.reset((test_address + 0xD0u) | 1u, initial_sp);
    interpreter.state().registers[0] = 7;
    interpreter.state().registers[8] = 5;
    const auto high_register = interpreter.run(2);
    const auto high_register_state = interpreter.state();

    const auto stopped_at_limit = [](const ArmExecutionResult &result) {
        return result.reason == ArmStopReason::instruction_limit;
    };
    const bool valid = stopped_at_limit(stack_adjust) && stack_adjust_state.registers[13] == initial_sp - 8u && stopped_at_limit(arithmetic) && arithmetic_state.registers[0] == 7 && arithmetic_state.registers[1] == 1 && stopped_at_limit(alu) && alu_state.registers[2] == 10 && stopped_at_limit(memory_immediate) && memory_immediate_state.registers[2] == 0xA1B2C3D4u && memory_immediate_state.registers[3] == 0xD4u && memory_immediate_state.registers[4] == 0xC3D4u && stopped_at_limit(memory_register) && memory_register_state.registers[3] == 0xFFFFFF80u && stopped_at_limit(multiple) && multiple_state.registers[2] == data_address + 0x28u && multiple_state.registers[3] == 0x11111111u && multiple_state.registers[4] == 0x22222222u && stopped_at_limit(compare_branch) && compare_branch_state.registers[1] == 2 && stopped_at_limit(branch) && branch_state.registers[1] == 3 && stopped_at_limit(stack_address) && stack_address_state.registers[0] == initial_sp + 4u && stopped_at_limit(high_register) && high_register_state.registers[8] == 12 && high_register_state.registers[1] == 12;

    std::string unmap_error;
    const bool unmapped = memory.unmap_all_segments(unmap_error);
    if (!valid) {
        error = "The Thumb compiler baseline diagnostic produced unexpected CPU state.";
        return false;
    }
    if (!unmapped) {
        error = unmap_error;
        return false;
    }
    return true;
}

bool run_thumb2_compiler_batch_test(GuestMemory &memory, std::string &error) {
    constexpr std::uint32_t test_address = 0x60000;
    constexpr std::uint32_t first_value = 0x11111111u;
    constexpr std::uint32_t second_value = 0x22222222u;
    std::array<std::uint8_t, 0x100> program{};

    write_value(program, 0x00, static_cast<std::uint16_t>(0xF247)); // MOVW r2, #0x7690
    write_value(program, 0x02, static_cast<std::uint16_t>(0x6290));
    write_value(program, 0x04, static_cast<std::uint16_t>(0xF2C8)); // MOVT r2, #0x812c
    write_value(program, 0x06, static_cast<std::uint16_t>(0x122C));
    write_value(program, 0x08, static_cast<std::uint16_t>(0xE9CD)); // STRD r1, r0, [sp]
    write_value(program, 0x0A, static_cast<std::uint16_t>(0x1000));
    write_value(program, 0x0C, static_cast<std::uint16_t>(0xE9DD)); // LDRD r3, r4, [sp]
    write_value(program, 0x0E, static_cast<std::uint16_t>(0x3400));
    write_value(program, 0x10, static_cast<std::uint16_t>(0xE9ED)); // STRD r0, r1, [sp, #8]!
    write_value(program, 0x12, static_cast<std::uint16_t>(0x0102));
    write_value(program, 0x14, static_cast<std::uint16_t>(0xE8FD)); // LDRD r2, r3, [sp], #8
    write_value(program, 0x16, static_cast<std::uint16_t>(0x2302));

    const auto memory_size_64 = static_cast<std::uint64_t>(memory.host_page_size());
    if (memory_size_64 < 0x400 || memory_size_64 > std::numeric_limits<std::uint32_t>::max()) {
        error = "The host page size is invalid for the Thumb-2 compiler batch diagnostic.";
        return false;
    }
    const auto memory_size = static_cast<std::uint32_t>(memory_size_64);
    if (!memory.map_segment(test_address, program, memory_size, 7, error)) {
        return false;
    }

    HLEDispatcher dispatcher;
    ArmInterpreter interpreter(memory, dispatcher);
    const auto data_address = test_address + 0x200u;
    interpreter.reset(test_address | 1u, data_address);
    interpreter.state().registers[0] = second_value;
    interpreter.state().registers[1] = first_value;
    const auto captured_sequence = interpreter.run(3);
    const auto captured_state = interpreter.state();

    std::array<std::uint32_t, 2> captured_words{};
    std::array<std::uint8_t, sizeof(captured_words)> captured_bytes{};
    const bool captured_read = memory.read(data_address, captured_bytes, error);
    if (captured_read) {
        std::memcpy(captured_words.data(), captured_bytes.data(), captured_bytes.size());
    }

    interpreter.reset((test_address + 0x0Cu) | 1u, data_address);
    const auto offset_load = interpreter.run(1);
    const auto offset_load_state = interpreter.state();

    interpreter.reset((test_address + 0x10u) | 1u, data_address);
    interpreter.state().registers[0] = first_value;
    interpreter.state().registers[1] = second_value;
    const auto preindexed_store = interpreter.run(1);
    const auto preindexed_store_state = interpreter.state();

    interpreter.reset((test_address + 0x14u) | 1u, data_address + 8u);
    const auto postindexed_load = interpreter.run(1);
    const auto postindexed_load_state = interpreter.state();

    const auto stopped_at_limit = [](const ArmExecutionResult &result) {
        return result.reason == ArmStopReason::instruction_limit;
    };
    const bool valid = stopped_at_limit(captured_sequence) && captured_sequence.instructions_executed == 3 && captured_state.registers[2] == 0x812C7690u && captured_read && captured_words[0] == first_value && captured_words[1] == second_value && stopped_at_limit(offset_load) && offset_load_state.registers[3] == first_value && offset_load_state.registers[4] == second_value && stopped_at_limit(preindexed_store) && preindexed_store_state.registers[13] == data_address + 8u && stopped_at_limit(postindexed_load) && postindexed_load_state.registers[2] == first_value && postindexed_load_state.registers[3] == second_value && postindexed_load_state.registers[13] == data_address + 16u;

    std::string unmap_error;
    const bool unmapped = memory.unmap_all_segments(unmap_error);
    if (!valid) {
        error = "The Thumb-2 compiler batch diagnostic produced unexpected CPU or memory state.";
        return false;
    }
    if (!unmapped) {
        error = unmap_error;
        return false;
    }
    return true;
}

bool run_thumb2_runtime_family_test(GuestMemory &memory, std::string &error) {
    constexpr std::uint32_t test_address = 0x70000;
    std::array<std::uint8_t, 0x100> program{};

    write_value(program, 0x00, static_cast<std::uint16_t>(0xF05F)); // MOVS.W r11, #0
    write_value(program, 0x02, static_cast<std::uint16_t>(0x0B00));
    write_value(program, 0x04, static_cast<std::uint16_t>(0xF8DD)); // LDR.W r10, [sp]
    write_value(program, 0x06, static_cast<std::uint16_t>(0xA000));

    write_value(program, 0x10, static_cast<std::uint16_t>(0xF881)); // STRB.W r0, [r1, #1]
    write_value(program, 0x12, static_cast<std::uint16_t>(0x0001));
    write_value(program, 0x14, static_cast<std::uint16_t>(0xF991)); // LDRSB.W r2, [r1, #1]
    write_value(program, 0x16, static_cast<std::uint16_t>(0x2001));
    write_value(program, 0x18, static_cast<std::uint16_t>(0xF8A1)); // STRH.W r3, [r1, #2]
    write_value(program, 0x1A, static_cast<std::uint16_t>(0x3002));
    write_value(program, 0x1C, static_cast<std::uint16_t>(0xF9B1)); // LDRSH.W r4, [r1, #2]
    write_value(program, 0x1E, static_cast<std::uint16_t>(0x4002));
    write_value(program, 0x20, static_cast<std::uint16_t>(0xF8C1)); // STR.W r5, [r1, #4]
    write_value(program, 0x22, static_cast<std::uint16_t>(0x5004));
    write_value(program, 0x24, static_cast<std::uint16_t>(0xF8D1)); // LDR.W r6, [r1, #4]
    write_value(program, 0x26, static_cast<std::uint16_t>(0x6004));

    write_value(program, 0x30, static_cast<std::uint16_t>(0xF841)); // STR.W r0, [r1, #8]!
    write_value(program, 0x32, static_cast<std::uint16_t>(0x0F08));
    write_value(program, 0x34, static_cast<std::uint16_t>(0xF851)); // LDR.W r2, [r1], #8
    write_value(program, 0x36, static_cast<std::uint16_t>(0x2B08));
    write_value(program, 0x38, static_cast<std::uint16_t>(0xF811)); // LDRB.W r3, [r1, #-1]!
    write_value(program, 0x3A, static_cast<std::uint16_t>(0x3D01));

    write_value(program, 0x40, static_cast<std::uint16_t>(0xF04F)); // invalid replicated zero
    write_value(program, 0x42, static_cast<std::uint16_t>(0x3100));
    write_value(program, 0x44, static_cast<std::uint16_t>(0xF851)); // invalid base/target writeback
    write_value(program, 0x46, static_cast<std::uint16_t>(0x1F04));
    write_value(program, 0x48, static_cast<std::uint16_t>(0xF8D1)); // overflowing LDR.W address
    write_value(program, 0x4A, static_cast<std::uint16_t>(0x0001));
    write_value(program, 0x4C, static_cast<std::uint16_t>(0xF851)); // unsupported LDRT.W r0, [r1, #4]
    write_value(program, 0x4E, static_cast<std::uint16_t>(0x0E04));

    write_value(program, 0x50, static_cast<std::uint16_t>(0xF04F)); // MOV.W r0, #0xabababab
    write_value(program, 0x52, static_cast<std::uint16_t>(0x30AB));
    write_value(program, 0x54, static_cast<std::uint16_t>(0xF020)); // BIC.W r1, r0, #0x00ff00ff
    write_value(program, 0x56, static_cast<std::uint16_t>(0x11FF));
    write_value(program, 0x58, static_cast<std::uint16_t>(0xF111)); // ADDS.W r2, r1, #1
    write_value(program, 0x5A, static_cast<std::uint16_t>(0x0201));
    write_value(program, 0x5C, static_cast<std::uint16_t>(0xF172)); // SBCS.W r3, r2, #1
    write_value(program, 0x5E, static_cast<std::uint16_t>(0x0301));
    write_value(program, 0x60, static_cast<std::uint16_t>(0xF1B3)); // CMP.W r3, #0
    write_value(program, 0x62, static_cast<std::uint16_t>(0x0F00));
    write_value(program, 0x64, static_cast<std::uint16_t>(0xF06F)); // MVN.W r4, #0
    write_value(program, 0x66, static_cast<std::uint16_t>(0x0400));
    write_value(program, 0x68, static_cast<std::uint16_t>(0xF064)); // ORN.W r5, r4, #1
    write_value(program, 0x6A, static_cast<std::uint16_t>(0x0501));
    write_value(program, 0x6C, static_cast<std::uint16_t>(0xF1C0)); // RSB.W r6, r0, #0
    write_value(program, 0x6E, static_cast<std::uint16_t>(0x0600));
    write_value(program, 0x70, static_cast<std::uint16_t>(0xF05F)); // MOVS.W r7, #0x80000000
    write_value(program, 0x72, static_cast<std::uint16_t>(0x4700));

    const auto memory_size_64 = static_cast<std::uint64_t>(memory.host_page_size());
    if (memory_size_64 < 0x400 || memory_size_64 > std::numeric_limits<std::uint32_t>::max()) {
        error = "The host page size is invalid for the Thumb-2 runtime-family diagnostic.";
        return false;
    }
    const auto memory_size = static_cast<std::uint32_t>(memory_size_64);
    if (!memory.map_segment(test_address, program, memory_size, 7, error)) {
        return false;
    }

    HLEDispatcher dispatcher;
    ArmInterpreter interpreter(memory, dispatcher);
    const auto data_address = test_address + 0x300u;
    constexpr std::uint32_t captured_stack_word = 0xAABBCCDDu;
    std::array<std::uint8_t, sizeof(captured_stack_word)> captured_stack_bytes{};
    std::memcpy(captured_stack_bytes.data(), &captured_stack_word,
        sizeof(captured_stack_word));
    if (!memory.write(data_address, captured_stack_bytes, error)) {
        return false;
    }

    interpreter.reset(test_address | 1u, data_address);
    interpreter.state().cpsr = 1u << 29;
    const auto captured = interpreter.run(2);
    const auto captured_state = interpreter.state();

    interpreter.reset((test_address + 0x10u) | 1u, data_address);
    interpreter.state().registers[0] = 0xFFFFFF80u;
    interpreter.state().registers[1] = data_address;
    interpreter.state().registers[3] = 0xFFFF8001u;
    interpreter.state().registers[5] = 0x11223344u;
    const auto offset_memory = interpreter.run(6);
    const auto offset_memory_state = interpreter.state();

    interpreter.reset((test_address + 0x30u) | 1u, data_address);
    interpreter.state().registers[0] = 0x55667788u;
    interpreter.state().registers[1] = data_address;
    const auto indexed_memory = interpreter.run(3);
    const auto indexed_memory_state = interpreter.state();

    interpreter.reset((test_address + 0x40u) | 1u, data_address);
    const auto invalid_immediate = interpreter.run(1);
    interpreter.reset((test_address + 0x44u) | 1u, data_address);
    interpreter.state().registers[1] = data_address;
    const auto invalid_writeback = interpreter.run(1);
    interpreter.reset((test_address + 0x48u) | 1u, data_address);
    interpreter.state().registers[1] = std::numeric_limits<std::uint32_t>::max();
    const auto overflow = interpreter.run(1);
    interpreter.reset((test_address + 0x4Cu) | 1u, data_address);
    interpreter.state().registers[1] = data_address;
    const auto unprivileged_load = interpreter.run(1);

    interpreter.reset((test_address + 0x50u) | 1u, data_address);
    const auto modified_alu = interpreter.run(9);
    const auto modified_alu_state = interpreter.state();

    const auto stopped_at_limit = [](const ArmExecutionResult &result) {
        return result.reason == ArmStopReason::instruction_limit;
    };
    const bool valid = stopped_at_limit(captured) && captured.instructions_executed == 2 && captured_state.registers[11] == 0 && captured_state.registers[10] == captured_stack_word && (captured_state.cpsr & (1u << 30)) != 0 && (captured_state.cpsr & (1u << 29)) != 0 && stopped_at_limit(offset_memory) && offset_memory_state.registers[2] == 0xFFFFFF80u && offset_memory_state.registers[4] == 0xFFFF8001u && offset_memory_state.registers[6] == 0x11223344u && stopped_at_limit(indexed_memory) && indexed_memory_state.registers[2] == 0x55667788u && indexed_memory_state.registers[1] == data_address + 15u && invalid_immediate.reason == ArmStopReason::unsupported_instruction && invalid_writeback.reason == ArmStopReason::unsupported_instruction && overflow.reason == ArmStopReason::memory_fault && unprivileged_load.reason == ArmStopReason::unsupported_instruction && stopped_at_limit(modified_alu) && modified_alu_state.registers[0] == 0xABABABABu && modified_alu_state.registers[1] == 0xAB00AB00u && modified_alu_state.registers[2] == 0xAB00AB01u && modified_alu_state.registers[3] == 0xAB00AAFFu && modified_alu_state.registers[4] == 0xFFFFFFFFu && modified_alu_state.registers[5] == 0xFFFFFFFFu && modified_alu_state.registers[6] == 0x54545455u && modified_alu_state.registers[7] == 0x80000000u && (modified_alu_state.cpsr & (1u << 31)) != 0 && (modified_alu_state.cpsr & (1u << 29)) != 0;

    std::string unmap_error;
    const bool unmapped = memory.unmap_all_segments(unmap_error);
    if (!valid) {
        error = "The Thumb-2 runtime-family diagnostic produced unexpected CPU, flags, memory, or fault state.";
        return false;
    }
    if (!unmapped) {
        error = unmap_error;
        return false;
    }
    return true;
}

bool run_thumb2_register_family_test(GuestMemory &memory, std::string &error) {
    constexpr std::uint32_t test_address = 0x80000;
    std::array<std::uint8_t, 0x100> program{};

    write_value(program, 0x00, static_cast<std::uint16_t>(0xEBAA)); // SUB.W r2, r10, r2
    write_value(program, 0x02, static_cast<std::uint16_t>(0x0202));
    write_value(program, 0x04, static_cast<std::uint16_t>(0xEA10)); // ANDS.W r3, r0, r1, LSL #2
    write_value(program, 0x06, static_cast<std::uint16_t>(0x0381));
    write_value(program, 0x08, static_cast<std::uint16_t>(0xEA5F)); // RORS.W r4, r1, #8
    write_value(program, 0x0A, static_cast<std::uint16_t>(0x2431));
    write_value(program, 0x0C, static_cast<std::uint16_t>(0xEA7F)); // MVNS.W r5, r0
    write_value(program, 0x0E, static_cast<std::uint16_t>(0x0500));
    write_value(program, 0x10, static_cast<std::uint16_t>(0xEA10)); // TST.W r0, r1, LSL #1
    write_value(program, 0x12, static_cast<std::uint16_t>(0x0F41));
    write_value(program, 0x14, static_cast<std::uint16_t>(0xEB50)); // ADCS.W r6, r0, r1
    write_value(program, 0x16, static_cast<std::uint16_t>(0x0601));
    write_value(program, 0x18, static_cast<std::uint16_t>(0xEB70)); // SBCS.W r7, r0, r1
    write_value(program, 0x1A, static_cast<std::uint16_t>(0x0701));
    write_value(program, 0x1C, static_cast<std::uint16_t>(0xEBD0)); // RSBS.W r8, r0, r1
    write_value(program, 0x1E, static_cast<std::uint16_t>(0x0801));
    write_value(program, 0x20, static_cast<std::uint16_t>(0xEA5F)); // RRXS.W r4, r1
    write_value(program, 0x22, static_cast<std::uint16_t>(0x0431));
    write_value(program, 0x24, static_cast<std::uint16_t>(0xEAC0)); // unsupported PKH family
    write_value(program, 0x26, static_cast<std::uint16_t>(0x0201));
    write_value(program, 0x28, static_cast<std::uint16_t>(0xEBAA)); // invalid PC shift source
    write_value(program, 0x2A, static_cast<std::uint16_t>(0x020F));

    write_value(program, 0x40, static_cast<std::uint16_t>(0xF841)); // STR.W r0, [r1, r2, LSL #2]
    write_value(program, 0x42, static_cast<std::uint16_t>(0x0022));
    write_value(program, 0x44, static_cast<std::uint16_t>(0xF851)); // LDR.W r3, [r1, r2, LSL #2]
    write_value(program, 0x46, static_cast<std::uint16_t>(0x3022));
    write_value(program, 0x48, static_cast<std::uint16_t>(0xF801)); // STRB.W r4, [r1, r2]
    write_value(program, 0x4A, static_cast<std::uint16_t>(0x4002));
    write_value(program, 0x4C, static_cast<std::uint16_t>(0xF911)); // LDRSB.W r5, [r1, r2]
    write_value(program, 0x4E, static_cast<std::uint16_t>(0x5002));
    write_value(program, 0x50, static_cast<std::uint16_t>(0xF821)); // STRH.W r6, [r1, r2]
    write_value(program, 0x52, static_cast<std::uint16_t>(0x6002));
    write_value(program, 0x54, static_cast<std::uint16_t>(0xF931)); // LDRSH.W r7, [r1, r2]
    write_value(program, 0x56, static_cast<std::uint16_t>(0x7002));
    write_value(program, 0x58, static_cast<std::uint16_t>(0xF851)); // invalid PC offset register
    write_value(program, 0x5A, static_cast<std::uint16_t>(0x300F));
    write_value(program, 0x5C, static_cast<std::uint16_t>(0xF851)); // overflowing scaled offset
    write_value(program, 0x5E, static_cast<std::uint16_t>(0x3022));
    write_value(program, 0x60, static_cast<std::uint16_t>(0xF851)); // reserved register suffix
    write_value(program, 0x62, static_cast<std::uint16_t>(0x3042));
    write_value(program, 0x64, static_cast<std::uint16_t>(0xF851)); // unmapped register-offset load
    write_value(program, 0x66, static_cast<std::uint16_t>(0x3000));

    const auto memory_size_64 = static_cast<std::uint64_t>(memory.host_page_size());
    if (memory_size_64 < 0x400 || memory_size_64 > std::numeric_limits<std::uint32_t>::max()) {
        error = "The host page size is invalid for the Thumb-2 register-family diagnostic.";
        return false;
    }
    const auto memory_size = static_cast<std::uint32_t>(memory_size_64);
    if (!memory.map_segment(test_address, program, memory_size, 7, error)) {
        return false;
    }

    HLEDispatcher dispatcher;
    ArmInterpreter interpreter(memory, dispatcher);
    const auto data_address = test_address + 0x300u;
    const auto run_at = [&](std::uint32_t offset) {
        interpreter.reset((test_address + offset) | 1u, data_address);
    };

    run_at(0x00);
    interpreter.state().registers[10] = 100;
    interpreter.state().registers[2] = 40;
    interpreter.state().cpsr = 0xF0000000u;
    const auto captured = interpreter.run(1);
    const auto captured_state = interpreter.state();

    run_at(0x04);
    interpreter.state().registers[0] = 0xFFFFFFFFu;
    interpreter.state().registers[1] = 0x40000001u;
    interpreter.state().cpsr = 1u << 28;
    const auto logical = interpreter.run(1);
    const auto logical_state = interpreter.state();

    run_at(0x08);
    interpreter.state().registers[1] = 0x80000001u;
    interpreter.state().cpsr = flag_carry;
    const auto rotate = interpreter.run(1);
    const auto rotate_state = interpreter.state();

    run_at(0x0C);
    interpreter.state().registers[0] = 0;
    interpreter.state().cpsr = flag_carry | flag_overflow;
    const auto invert = interpreter.run(1);
    const auto invert_state = interpreter.state();

    run_at(0x10);
    interpreter.state().registers[0] = 8;
    interpreter.state().registers[1] = 4;
    const auto test = interpreter.run(1);
    const auto test_state = interpreter.state();

    run_at(0x14);
    interpreter.state().registers[0] = std::numeric_limits<std::uint32_t>::max();
    interpreter.state().registers[1] = 0;
    interpreter.state().cpsr = flag_carry;
    const auto add = interpreter.run(1);
    const auto add_state = interpreter.state();

    run_at(0x18);
    interpreter.state().registers[0] = 0;
    interpreter.state().registers[1] = 0;
    const auto subtract_carry = interpreter.run(1);
    const auto subtract_carry_state = interpreter.state();

    run_at(0x1C);
    interpreter.state().registers[0] = 5;
    interpreter.state().registers[1] = 9;
    const auto reverse_subtract = interpreter.run(1);
    const auto reverse_subtract_state = interpreter.state();

    run_at(0x20);
    interpreter.state().registers[1] = 2;
    interpreter.state().cpsr = flag_carry;
    const auto rotate_extend = interpreter.run(1);
    const auto rotate_extend_state = interpreter.state();

    run_at(0x24);
    const auto unsupported_pack = interpreter.run(1);
    run_at(0x28);
    const auto invalid_shift_register = interpreter.run(1);

    run_at(0x40);
    interpreter.state().registers[0] = 0xAABBCCDDu;
    interpreter.state().registers[1] = data_address;
    interpreter.state().registers[2] = 0x40000001u;
    const auto word_memory = interpreter.run(2);
    const auto word_memory_state = interpreter.state();

    run_at(0x48);
    interpreter.state().registers[1] = data_address + 0x20u;
    interpreter.state().registers[2] = 3;
    interpreter.state().registers[4] = 0xFFFFFF80u;
    const auto byte_memory = interpreter.run(2);
    const auto byte_memory_state = interpreter.state();

    run_at(0x50);
    interpreter.state().registers[1] = data_address + 0x40u;
    interpreter.state().registers[2] = 2;
    interpreter.state().registers[6] = 0xFFFF8001u;
    const auto halfword_memory = interpreter.run(2);
    const auto halfword_memory_state = interpreter.state();

    run_at(0x58);
    interpreter.state().registers[1] = data_address;
    const auto invalid_offset_register = interpreter.run(1);
    run_at(0x5C);
    interpreter.state().registers[1] = 0xFFFFFFFCu;
    interpreter.state().registers[2] = 1;
    const auto offset_overflow = interpreter.run(1);
    run_at(0x60);
    const auto reserved_memory_suffix = interpreter.run(1);
    run_at(0x64);
    interpreter.state().registers[1] = test_address + memory_size;
    const auto unmapped_memory = interpreter.run(1);

    const auto stopped_at_limit = [](const ArmExecutionResult &result) {
        return result.reason == ArmStopReason::instruction_limit;
    };
    const bool alu_valid = stopped_at_limit(captured) && captured_state.registers[2] == 60 && captured_state.cpsr == 0xF0000000u && stopped_at_limit(logical) && logical_state.registers[3] == 4 && (logical_state.cpsr & flag_carry) != 0 && (logical_state.cpsr & flag_overflow) != 0 && stopped_at_limit(rotate) && rotate_state.registers[4] == 0x01800000u && (rotate_state.cpsr & flag_carry) == 0 && stopped_at_limit(invert) && invert_state.registers[5] == 0xFFFFFFFFu && (invert_state.cpsr & flag_negative) != 0 && (invert_state.cpsr & flag_carry) != 0 && (invert_state.cpsr & flag_overflow) != 0 && stopped_at_limit(test) && (test_state.cpsr & flag_zero) == 0 && stopped_at_limit(add) && add_state.registers[6] == 0 && (add_state.cpsr & flag_zero) != 0 && (add_state.cpsr & flag_carry) != 0 && stopped_at_limit(subtract_carry) && subtract_carry_state.registers[7] == 0xFFFFFFFFu && (subtract_carry_state.cpsr & flag_negative) != 0 && (subtract_carry_state.cpsr & flag_carry) == 0 && stopped_at_limit(reverse_subtract) && reverse_subtract_state.registers[8] == 4 && (reverse_subtract_state.cpsr & flag_carry) != 0 && stopped_at_limit(rotate_extend) && rotate_extend_state.registers[4] == 0x80000001u && (rotate_extend_state.cpsr & flag_carry) == 0 && unsupported_pack.reason == ArmStopReason::unsupported_instruction && invalid_shift_register.reason == ArmStopReason::unsupported_instruction;
    const bool memory_valid = stopped_at_limit(word_memory) && word_memory_state.registers[3] == 0xAABBCCDDu && stopped_at_limit(byte_memory) && byte_memory_state.registers[5] == 0xFFFFFF80u && stopped_at_limit(halfword_memory) && halfword_memory_state.registers[7] == 0xFFFF8001u && invalid_offset_register.reason == ArmStopReason::unsupported_instruction && offset_overflow.reason == ArmStopReason::memory_fault && reserved_memory_suffix.reason == ArmStopReason::unsupported_instruction && unmapped_memory.reason == ArmStopReason::memory_fault;

    std::string unmap_error;
    const bool unmapped = memory.unmap_all_segments(unmap_error);
    if (!alu_valid || !memory_valid) {
        error = "The Thumb-2 register-family diagnostic produced unexpected CPU, flags, memory, or fault state: reserved=" + std::to_string(static_cast<int>(reserved_memory_suffix.reason)) + ", unmapped=" + std::to_string(static_cast<int>(unmapped_memory.reason)) + " (" + unmapped_memory.detail + ").";
        return false;
    }
    if (!unmapped) {
        error = unmap_error;
        return false;
    }
    return true;
}

bool run_thumb2_multiple_transfer_test(GuestMemory &memory, std::string &error) {
    constexpr std::uint32_t test_address = 0x90000;
    std::array<std::uint8_t, 0x100> program{};

    write_value(program, 0x00, static_cast<std::uint16_t>(0xE8BD)); // POP.W {r4-r8, pc}
    write_value(program, 0x02, static_cast<std::uint16_t>(0x81F0));
    write_value(program, 0x10, static_cast<std::uint16_t>(0xE8A1)); // STMIA.W r1!, {r0,r2,r8}
    write_value(program, 0x12, static_cast<std::uint16_t>(0x0105));
    write_value(program, 0x14, static_cast<std::uint16_t>(0xE892)); // LDMIA.W r2, {r3,r9}
    write_value(program, 0x16, static_cast<std::uint16_t>(0x0208));
    write_value(program, 0x18, static_cast<std::uint16_t>(0xE903)); // STMDB.W r3, {r0,r4}
    write_value(program, 0x1A, static_cast<std::uint16_t>(0x0011));
    write_value(program, 0x1C, static_cast<std::uint16_t>(0xE934)); // LDMDB.W r4!, {r5,r10}
    write_value(program, 0x1E, static_cast<std::uint16_t>(0x0420));
    write_value(program, 0x20, static_cast<std::uint16_t>(0xE8B1)); // invalid one-register LDM
    write_value(program, 0x22, static_cast<std::uint16_t>(0x0002));
    write_value(program, 0x24, static_cast<std::uint16_t>(0xE8A1)); // invalid STM including PC
    write_value(program, 0x26, static_cast<std::uint16_t>(0x8001));
    write_value(program, 0x28, static_cast<std::uint16_t>(0xE8B1)); // invalid LDM including LR and PC
    write_value(program, 0x2A, static_cast<std::uint16_t>(0xC001));
    write_value(program, 0x2C, static_cast<std::uint16_t>(0xE8A1)); // invalid STM including SP
    write_value(program, 0x2E, static_cast<std::uint16_t>(0x2001));
    write_value(program, 0x30, static_cast<std::uint16_t>(0xE8A1)); // invalid writeback/base alias
    write_value(program, 0x32, static_cast<std::uint16_t>(0x0003));
    write_value(program, 0x34, static_cast<std::uint16_t>(0xE923)); // underflowing STMDB.W
    write_value(program, 0x36, static_cast<std::uint16_t>(0x0011));
    write_value(program, 0x38, static_cast<std::uint16_t>(0xE8A1)); // overflowing STMIA.W
    write_value(program, 0x3A, static_cast<std::uint16_t>(0x0005));
    write_value(program, 0x3C, static_cast<std::uint16_t>(0xE8B1)); // unmapped LDMIA.W
    write_value(program, 0x3E, static_cast<std::uint16_t>(0x0005));
    write_value(program, 0x40, static_cast<std::uint16_t>(0xE8BD)); // zero-link POP.W return
    write_value(program, 0x42, static_cast<std::uint16_t>(0x8001));

    const auto memory_size_64 = static_cast<std::uint64_t>(memory.host_page_size());
    if (memory_size_64 < 0x400 || memory_size_64 > std::numeric_limits<std::uint32_t>::max()) {
        error = "The host page size is invalid for the Thumb-2 multiple-transfer diagnostic.";
        return false;
    }
    const auto memory_size = static_cast<std::uint32_t>(memory_size_64);
    if (!memory.map_segment(test_address, program, memory_size, 7, error)) {
        return false;
    }

    HLEDispatcher dispatcher;
    ArmInterpreter interpreter(memory, dispatcher);
    const auto data_address = test_address + 0x200u;
    const auto run_at = [&](std::uint32_t offset, std::uint32_t stack_pointer) {
        interpreter.reset((test_address + offset) | 1u, stack_pointer);
        return interpreter.run(1);
    };

    const std::array<std::uint32_t, 6> pop_words{
        0x44444444u, 0x55555555u, 0x66666666u,
        0x77777777u, 0x88888888u, (test_address + 0x80u) | 1u
    };
    std::array<std::uint8_t, sizeof(pop_words)> pop_bytes{};
    std::memcpy(pop_bytes.data(), pop_words.data(), pop_bytes.size());
    const auto pop_stack = data_address + 0x80u;
    if (!memory.write(pop_stack, pop_bytes, error)) {
        return false;
    }
    const auto captured_pop = run_at(0x00, pop_stack);
    const auto captured_pop_state = interpreter.state();

    constexpr std::uint32_t first_value = 0x11111111u;
    constexpr std::uint32_t second_value = 0x22222222u;
    constexpr std::uint32_t third_value = 0x88888888u;
    interpreter.reset((test_address + 0x10u) | 1u, data_address + 0x100u);
    interpreter.state().registers[0] = first_value;
    interpreter.state().registers[1] = data_address;
    interpreter.state().registers[2] = second_value;
    interpreter.state().registers[8] = third_value;
    const auto increment_store = interpreter.run(1);
    const auto increment_store_state = interpreter.state();
    std::array<std::uint32_t, 3> stored_words{};
    std::array<std::uint8_t, sizeof(stored_words)> stored_bytes{};
    const bool stored_read = memory.read(data_address, stored_bytes, error);
    if (stored_read) {
        std::memcpy(stored_words.data(), stored_bytes.data(), stored_bytes.size());
    }

    interpreter.reset((test_address + 0x14u) | 1u, data_address + 0x100u);
    interpreter.state().registers[2] = data_address;
    const auto increment_load = interpreter.run(1);
    const auto increment_load_state = interpreter.state();

    interpreter.reset((test_address + 0x18u) | 1u, data_address + 0x100u);
    interpreter.state().registers[0] = first_value;
    interpreter.state().registers[3] = data_address + 0x48u;
    interpreter.state().registers[4] = second_value;
    const auto decrement_store = interpreter.run(1);
    const auto decrement_store_state = interpreter.state();
    std::array<std::uint32_t, 2> decrement_words{};
    std::array<std::uint8_t, sizeof(decrement_words)> decrement_bytes{};
    const bool decrement_read = memory.read(
        data_address + 0x40u, decrement_bytes, error);
    if (decrement_read) {
        std::memcpy(decrement_words.data(), decrement_bytes.data(), decrement_bytes.size());
    }

    interpreter.reset((test_address + 0x1Cu) | 1u, data_address + 0x100u);
    interpreter.state().registers[4] = data_address + 0x48u;
    const auto decrement_load = interpreter.run(1);
    const auto decrement_load_state = interpreter.state();

    const auto invalid_count = run_at(0x20, data_address + 0x100u);
    const auto invalid_store_pc = run_at(0x24, data_address + 0x100u);
    const auto invalid_load_lr_pc = run_at(0x28, data_address + 0x100u);
    const auto invalid_sp = run_at(0x2C, data_address + 0x100u);
    const auto invalid_base_alias = run_at(0x30, data_address + 0x100u);
    interpreter.reset((test_address + 0x34u) | 1u, data_address + 0x100u);
    interpreter.state().registers[3] = 4;
    const auto underflow = interpreter.run(1);
    interpreter.reset((test_address + 0x38u) | 1u, data_address + 0x100u);
    interpreter.state().registers[1] = 0xFFFFFFFCu;
    const auto overflow = interpreter.run(1);
    interpreter.reset((test_address + 0x3Cu) | 1u, data_address + 0x100u);
    interpreter.state().registers[1] = test_address + memory_size;
    const auto unmapped = interpreter.run(1);

    const std::array<std::uint32_t, 2> zero_return_words{ 0xAAAAAAAAu, 0u };
    std::array<std::uint8_t, sizeof(zero_return_words)> zero_return_bytes{};
    std::memcpy(zero_return_bytes.data(), zero_return_words.data(),
        zero_return_bytes.size());
    const auto zero_return_stack = data_address + 0xC0u;
    if (!memory.write(zero_return_stack, zero_return_bytes, error)) {
        return false;
    }
    const auto zero_return = run_at(0x40, zero_return_stack);
    const auto zero_return_state = interpreter.state();

    const auto stopped_at_limit = [](const ArmExecutionResult &result) {
        return result.reason == ArmStopReason::instruction_limit;
    };
    const bool transfers_valid = stopped_at_limit(captured_pop)
        && captured_pop_state.registers[4] == pop_words[0]
        && captured_pop_state.registers[5] == pop_words[1]
        && captured_pop_state.registers[6] == pop_words[2]
        && captured_pop_state.registers[7] == pop_words[3]
        && captured_pop_state.registers[8] == pop_words[4]
        && captured_pop_state.registers[13] == pop_stack + sizeof(pop_words)
        && captured_pop_state.registers[15] == test_address + 0x80u
        && stopped_at_limit(increment_store) && stored_read
        && increment_store_state.registers[1] == data_address + sizeof(stored_words)
        && stored_words[0] == first_value && stored_words[1] == second_value
        && stored_words[2] == third_value && stopped_at_limit(increment_load)
        && increment_load_state.registers[2] == data_address
        && increment_load_state.registers[3] == first_value
        && increment_load_state.registers[9] == second_value
        && stopped_at_limit(decrement_store) && decrement_read
        && decrement_store_state.registers[3] == data_address + 0x48u
        && decrement_words[0] == first_value && decrement_words[1] == second_value
        && stopped_at_limit(decrement_load)
        && decrement_load_state.registers[4] == data_address + 0x40u
        && decrement_load_state.registers[5] == first_value
        && decrement_load_state.registers[10] == second_value;
    const bool boundaries_valid = invalid_count.reason == ArmStopReason::unsupported_instruction
        && invalid_store_pc.reason == ArmStopReason::unsupported_instruction
        && invalid_load_lr_pc.reason == ArmStopReason::unsupported_instruction
        && invalid_sp.reason == ArmStopReason::unsupported_instruction
        && invalid_base_alias.reason == ArmStopReason::unsupported_instruction
        && underflow.reason == ArmStopReason::memory_fault
        && overflow.reason == ArmStopReason::memory_fault
        && unmapped.reason == ArmStopReason::memory_fault
        && zero_return.reason == ArmStopReason::halted
        && zero_return_state.registers[0] == zero_return_words[0]
        && zero_return_state.registers[13] == zero_return_stack + sizeof(zero_return_words);

    std::string unmap_error;
    const bool unmap_succeeded = memory.unmap_all_segments(unmap_error);
    if (!transfers_valid || !boundaries_valid) {
        std::ostringstream diagnostic;
        diagnostic << "The Thumb-2 multiple-transfer diagnostic produced unexpected state: "
                   << "pop_reason=" << static_cast<int>(captured_pop.reason)
                   << ", pop_instruction=" << captured_pop.last_instruction
                   << ", pop_sp=" << captured_pop_state.registers[13]
                   << ", pop_pc=" << captured_pop_state.registers[15]
                   << ", pop_regs=" << captured_pop_state.registers[4]
                   << "/" << captured_pop_state.registers[5]
                   << "/" << captured_pop_state.registers[6]
                   << "/" << captured_pop_state.registers[7]
                   << "/" << captured_pop_state.registers[8]
                   << ", stm_reason=" << static_cast<int>(increment_store.reason)
                   << ", stm_base=" << increment_store_state.registers[1]
                   << ", stm_read=" << stored_read
                   << ", stm_words=" << stored_words[0]
                   << "/" << stored_words[1] << "/" << stored_words[2]
                   << ", ldm_reason=" << static_cast<int>(increment_load.reason)
                   << ", ldm_r2=" << increment_load_state.registers[2]
                   << ", ldm_r3=" << increment_load_state.registers[3]
                   << ", ldm_r9=" << increment_load_state.registers[9]
                   << ", stmdb_reason=" << static_cast<int>(decrement_store.reason)
                   << ", stmdb_base=" << decrement_store_state.registers[3]
                   << ", stmdb_words=" << decrement_words[0]
                   << "/" << decrement_words[1]
                   << ", ldmdb_reason=" << static_cast<int>(decrement_load.reason)
                   << ", ldmdb_base=" << decrement_load_state.registers[4]
                   << ", ldmdb_regs=" << decrement_load_state.registers[5]
                   << "/" << decrement_load_state.registers[10]
                   << ", invalid=" << static_cast<int>(invalid_count.reason)
                   << "/" << static_cast<int>(invalid_store_pc.reason)
                   << "/" << static_cast<int>(invalid_load_lr_pc.reason)
                   << "/" << static_cast<int>(invalid_sp.reason)
                   << "/" << static_cast<int>(invalid_base_alias.reason)
                   << ", faults=" << static_cast<int>(underflow.reason)
                   << "/" << static_cast<int>(overflow.reason)
                   << "/" << static_cast<int>(unmapped.reason)
                   << ", zero=" << static_cast<int>(zero_return.reason)
                   << ", zero_r0=" << zero_return_state.registers[0]
                   << ", zero_sp=" << zero_return_state.registers[13] << ".";
        error = diagnostic.str();
        return false;
    }
    if (!unmap_succeeded) {
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

void scan_installed_titles() {
    core_status.installed_titles.clear();
    if (!host_storage.ready)
        return;
    std::error_code error;
    const auto app_root = host_storage.root / "ux0/app";
    for (std::filesystem::directory_iterator iterator(app_root, error), end;
        iterator != end && !error; iterator.increment(error)) {
        if (!iterator->is_directory(error) || error)
            continue;
        const auto metadata = parse_vita_app_metadata(iterator->path() / "sce_sys/param.sfo");
        if (!metadata.parsed)
            continue;
        core_status.installed_titles.push_back({ .title_id = metadata.title_id,
            .title = metadata.title,
            .app_version = metadata.app_version,
            .patch_installed = std::filesystem::is_directory(
                host_storage.root / "ux0/patch" / metadata.title_id, error),
            .base_eboot_present = std::filesystem::is_regular_file(
                iterator->path() / "eboot.bin", error),
            .patch_eboot_present = std::filesystem::is_regular_file(
                host_storage.root / "ux0/patch" / metadata.title_id / "eboot.bin", error),
            .app_path = iterator->path().string() });
        error.clear();
    }
    if (error)
        append_storage_error("Installed title scan: " + error.message());
    std::ranges::sort(core_status.installed_titles, {}, &InstalledTitle::title_id);
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
    core_status.selected_title_id.clear();
    core_status.selected_executable_loaded = false;
    core_status.selected_boot_available = false;
    core_status.selected_boot_attempted = false;
    core_status.title_preparation_status.clear();
    core_status.title_boot_status.clear();
    prepared_executable = {};
    scan_installed_titles();
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
        if (loaded_filename.empty() && guest_memory && core_status.loader_pipeline_ready && probe.structurally_valid && probe.kind == "Vita ELF") {
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
        core_status.imported_artifacts.push_back({ .filename = entry.path().filename().string(),
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
                ? std::string{}
                : archive.applications.front().install_target,
            .structurally_valid = probe.structurally_valid,
            .load_segment_count = probe.load_segments.size(),
            .load_attempted = load.attempted,
            .loaded = load.loaded,
            .module_info_valid = load.module_info_valid,
            .relocations_applied = load.relocations_applied,
            .module_tables_parsed = load.module_tables_parsed,
            .import_stubs_bound = load.import_stubs_bound,
            .module_start_valid = load.module_start_valid,
            .module_start_from_export = load.module_start_from_export,
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
            .detail = std::move(detail) });
    }

    std::ostringstream summary;
    summary << "Core slice: ARM encoder + Vita NID database\n"
            << "Self-tests: " << (core_status.self_tests_passed ? "passed" : "FAILED") << "\n"
            << "Upstream app metadata: " << (core_status.upstream_metadata_ready ? "passed (Vita3K packages/SFO parser linked)" : "FAILED") << "\n"
            << "Upstream app archive: " << (core_status.upstream_archive_ready ? "passed (miniz + Vita3K package inspector linked)" : "FAILED") << "\n"
            << "Package installer: " << (core_status.package_installer_ready ? "ready (transactional app + patch commit)" : "FAILED") << "\n"
            << "Guest memory: " << (core_status.guest_memory_ready ? "ready" : "FAILED");
    if (core_status.guest_memory_ready) {
        summary << " (" << (core_status.guest_memory_size >> 30) << " GiB reserved, "
                << core_status.host_page_size << "-byte pages)";
    }
    summary << "\nBatch segment map: " << (core_status.segment_mapping_ready ? "passed (shared-page permission merge)" : "FAILED");
    summary << "\nRelocation/tables: " << (core_status.loader_pipeline_ready ? "passed (verified patch + 1 export/1 import library)" : "FAILED");
    summary << "\nCompiled import stubs: " << (core_status.import_binding_ready ? "passed (" + std::to_string(core_status.thread_test_bound_stub_count) + " SVC trampolines)" : "FAILED");
    summary << "\nARM/Thumb execution/HLE: " << (core_status.arm_execution_ready ? "passed (compiler/runtime families + " + std::to_string(core_status.arm_test_instruction_count) + " ARM instructions + " + std::to_string(core_status.hle_test_dispatch_count) + " bound NID call)" : "FAILED");
    summary << "\nThumb/ARM entry/thread: " << (core_status.guest_thread_ready ? "passed (" + std::to_string(core_status.thread_test_instruction_count) + " instructions + " + std::to_string(core_status.thread_test_hle_dispatch_count) + " kernel HLE calls + exit " + std::to_string(core_status.thread_test_exit_status) + ")" : "FAILED");
    summary << "\nStorage: " << (core_status.storage_ready ? "ready" : "FAILED")
            << "\nInstalled titles: " << core_status.installed_titles.size();
    for (const auto &title : core_status.installed_titles) {
        summary << "\n  + " << title.title_id << " - " << title.title;
        if (title.patch_installed)
            summary << " (patch installed)";
    }
    if (!core_status.package_install_status.empty())
        summary << "\nLast install: " << core_status.package_install_status;
    summary << "\n"
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
    summary << "\n\nNext: prepare an installed title using its lifecycle module_start export, then use Attempt Boot once to capture the first real CPU or HLE boundary. The bounded interpreter cannot run general Vita games yet.";
    if (!host_storage.error.empty()) {
        summary << "\nStorage error: " << host_storage.error;
    }
    core_status.summary = summary.str();
    update_renderer_status();
    update_input_status();
}

void update_title_preparation_summary(const TitlePreparationResult &result) {
    const auto marker = core_status.summary.find("\nSelected title:");
    const auto next = core_status.summary.find("\n\nNext:");
    if (marker != std::string::npos) {
        core_status.summary.erase(marker,
            next == std::string::npos ? std::string::npos : next - marker);
    }

    std::ostringstream lines;
    lines << "\nSelected title: " << result.title_id;
    if (!result.title.empty())
        lines << " - " << result.title;
    if (!result.source.empty())
        lines << "\nBoot source: " << result.source << "/eboot.bin";
    lines << "\nExecutable preparation: " << result.detail;
    const auto insertion = core_status.summary.find("\n\nNext:");
    core_status.summary.insert(insertion == std::string::npos
            ? core_status.summary.size()
            : insertion,
        lines.str());
}

void update_title_boot_summary(const TitleBootResult &result, std::size_t instruction_limit) {
    const auto marker = core_status.summary.find("\nControlled boot attempt:");
    const auto next = core_status.summary.find("\n\nNext:");
    if (marker != std::string::npos) {
        core_status.summary.erase(marker,
            next == std::string::npos ? std::string::npos : next - marker);
    }
    std::ostringstream line;
    line << "\nControlled boot attempt: interpreter-only, " << instruction_limit
         << "-instruction ceiling; " << result.detail;
    const auto insertion = core_status.summary.find("\n\nNext:");
    core_status.summary.insert(insertion == std::string::npos
            ? core_status.summary.size()
            : insertion,
        line.str());
}

} // namespace

CoreStatus initialize_core(const std::filesystem::path &documents_root) {
    std::lock_guard lock(core_mutex);
    host_display = HostDisplay{};
    host_input = HostInput{};
    prepared_executable = {};
    core_status.self_tests_passed = run_upstream_self_tests();
    core_status.upstream_metadata_ready = run_upstream_metadata_test();
    core_status.upstream_archive_ready = run_upstream_archive_test();
    guest_memory = std::make_unique<GuestMemory>();
    std::string memory_error;
    constexpr std::uint64_t vita_address_space_size = 1ULL << 32;
    const bool reserved = guest_memory->reserve(vita_address_space_size, memory_error);
    const bool protection_test_passed = reserved && guest_memory->run_commit_protection_test(memory_error);
    const bool segment_mapping_passed = protection_test_passed && run_segment_mapping_test(*guest_memory, memory_error);
    GuestThreadRunResult thread_test;
    ImportBindingResult binding_test;
    const bool loader_pipeline_passed = segment_mapping_passed && run_loader_pipeline_test(*guest_memory, binding_test, thread_test, memory_error);
    std::string execution_error;
    const bool arm_execution_passed = loader_pipeline_passed && run_arm_execution_test(*guest_memory, core_status.arm_test_instruction_count, core_status.hle_test_dispatch_count, execution_error) && run_inline_hle_nid_diagnostic_test(*guest_memory, execution_error) && run_thumb2_wide_push_test(*guest_memory, execution_error) && run_thumb_compiler_baseline_test(*guest_memory, execution_error) && run_thumb2_compiler_batch_test(*guest_memory, execution_error) && run_thumb2_runtime_family_test(*guest_memory, execution_error) && run_thumb2_register_family_test(*guest_memory, execution_error) && run_thumb2_multiple_transfer_test(*guest_memory, execution_error);
    core_status.guest_memory_ready = reserved && protection_test_passed;
    core_status.segment_mapping_ready = segment_mapping_passed;
    core_status.loader_pipeline_ready = loader_pipeline_passed;
    core_status.import_binding_ready = loader_pipeline_passed && binding_test.success && binding_test.bound_function_count == 2;
    core_status.arm_execution_ready = arm_execution_passed;
    core_status.guest_thread_ready = loader_pipeline_passed && thread_test.exited;
    core_status.thread_test_instruction_count = thread_test.instruction_count;
    core_status.thread_test_hle_dispatch_count = thread_test.hle_dispatch_count;
    core_status.thread_test_exit_status = thread_test.exit_status;
    core_status.thread_test_bound_stub_count = binding_test.bound_function_count;
    core_status.guest_memory_size = reserved ? guest_memory->size() : 0;
    core_status.host_page_size = reserved ? guest_memory->host_page_size() : 0;
    host_storage = initialize_host_storage(documents_root);
    core_status.package_installer_ready = core_status.upstream_archive_ready && host_storage.ready;
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

GameInstallResult install_game_archive(const std::filesystem::path &archive_path) {
    std::lock_guard lock(core_mutex);
    const auto installed = packages::install_archive_transactionally(
        archive_path, host_storage.root);
    core_status.package_install_status = installed.detail;
    update_status_from_storage();
    return {
        .attempted = installed.attempted,
        .success = installed.success,
        .application_count = installed.application_count,
        .file_count = installed.file_count,
        .bytes_written = installed.bytes_written,
        .installed_targets = installed.installed_targets,
        .detail = installed.detail
    };
}

TitlePreparationResult prepare_installed_title(std::string title_id, bool prefer_patch) {
    std::lock_guard lock(core_mutex);
    TitlePreparationResult result{
        .attempted = true,
        .title_id = std::move(title_id)
    };
    prepared_executable = {};
    core_status.selected_boot_available = false;
    core_status.selected_boot_attempted = false;
    core_status.title_boot_status.clear();

    const auto selected = std::ranges::find(core_status.installed_titles,
        result.title_id, &InstalledTitle::title_id);
    if (selected == core_status.installed_titles.end()) {
        result.detail = "The selected title is no longer installed. Rescan the library.";
        core_status.selected_title_id = result.title_id;
        core_status.selected_executable_loaded = false;
        core_status.title_preparation_status = result.detail;
        update_title_preparation_summary(result);
        return result;
    }
    result.selected = true;
    result.title = selected->title;

    if (guest_memory && guest_memory->mapped_segment_count() != 0) {
        std::string unmap_error;
        if (!guest_memory->unmap_all_segments(unmap_error)) {
            result.detail = "The previously prepared executable could not be unloaded: " + unmap_error;
            core_status.selected_title_id = result.title_id;
            core_status.selected_executable_loaded = false;
            core_status.title_preparation_status = result.detail;
            update_title_preparation_summary(result);
            return result;
        }
    }

    const auto base = host_storage.root / "ux0/app" / result.title_id / "eboot.bin";
    const auto patch = host_storage.root / "ux0/patch" / result.title_id / "eboot.bin";
    std::error_code error;
    const bool base_exists = std::filesystem::is_regular_file(base, error);
    error.clear();
    const bool patch_exists = std::filesystem::is_regular_file(patch, error);
    error.clear();
    std::filesystem::path executable;
    if (prefer_patch && patch_exists) {
        executable = patch;
        result.patch_selected = true;
        result.source = "patch";
    } else if (base_exists) {
        executable = base;
        result.source = "base app";
    } else if (patch_exists) {
        executable = patch;
        result.patch_selected = true;
        result.source = "patch fallback";
    } else {
        result.detail = "No base-app or patch eboot.bin exists for this title.";
    }

    if (!executable.empty()) {
        result.executable_path = executable.string();
        const auto probe = probe_artifact(executable);
        result.probe_valid = probe.structurally_valid;
        result.self_segments_plain = probe.self_segments_plain;
        result.kind = probe.kind;
        result.load_segment_count = probe.load_segments.size();
        result.encrypted_segment_count = probe.encrypted_segment_count;
        result.compressed_segment_count = probe.compressed_segment_count;
        if (!probe.structurally_valid) {
            result.detail = "Probe stopped: " + probe.detail;
        } else if (probe.kind == "Vita SELF" && !probe.self_segments_plain) {
            std::ostringstream detail;
            detail << "SELF recognized with " << probe.load_segments.size()
                   << " load segments, but " << probe.encrypted_segment_count
                   << " segment" << (probe.encrypted_segment_count == 1 ? " is" : "s are")
                   << " encrypted. Decryption must be integrated before mapping or execution.";
            result.detail = detail.str();
        } else if (!guest_memory || !core_status.loader_pipeline_ready) {
            result.detail = "The guest-memory loader pipeline is unavailable.";
        } else {
            const auto load = load_plain_elf(executable, probe, *guest_memory);
            result.loaded = load.loaded;
            result.module_name = load.module_name;
            result.imported_nid_count = load.imported_nid_count;
            result.bound_import_stub_count = load.bound_import_stub_count;
            result.module_start_address = load.module_start_address;
            result.module_start_from_export = load.module_start_from_export;
            result.detail = load.detail;
            if (load.loaded && load.module_start_valid && load.temporary_stack_pointer != 0) {
                prepared_executable = {
                    .ready = true,
                    .title_id = result.title_id,
                    .module_name = load.module_name,
                    .module_start_address = load.module_start_address,
                    .temporary_stack_pointer = load.temporary_stack_pointer,
                    .imported_nids = load.imported_nids
                };
            }
        }
    }

    core_status.selected_title_id = result.title_id;
    core_status.selected_executable_loaded = result.loaded;
    core_status.selected_boot_available = prepared_executable.ready;
    core_status.title_preparation_status = result.detail;
    update_title_preparation_summary(result);
    return result;
}

TitleBootResult attempt_prepared_title_boot(std::size_t instruction_limit) {
    std::lock_guard lock(core_mutex);
    TitleBootResult result{
        .attempted = true,
        .title_id = prepared_executable.title_id
    };
    if (instruction_limit == 0 || instruction_limit > maximum_controlled_boot_instructions) {
        result.detail = "The controlled boot budget must be between 1 and 256 instructions.";
    } else if (!prepared_executable.ready || !guest_memory) {
        result.detail = "No prepared executable is available. Select the title again first.";
    } else {
        const auto thread = run_guest_module_start(*guest_memory,
            prepared_executable.module_start_address,
            prepared_executable.temporary_stack_pointer,
            prepared_executable.imported_nids,
            prepared_executable.module_name,
            instruction_limit);
        result.started = thread.started;
        result.exited = thread.exited;
        result.returned = thread.returned;
        result.instruction_count = thread.instruction_count;
        result.hle_dispatch_count = thread.hle_dispatch_count;
        result.last_hle_nid = thread.last_hle_nid;
        result.last_guest_pc = thread.last_guest_pc;
        result.libc_dso_handle_main = thread.libc_dso_handle_main;
        result.libc_atexit_registration_count = thread.libc_atexit_registration_count;
        result.libc_finalize_call_count = thread.libc_finalize_call_count;
        result.libc_guard_acquire_count = thread.libc_guard_acquire_count;
        result.libc_guard_release_count = thread.libc_guard_release_count;
        result.libc_guard_abort_count = thread.libc_guard_abort_count;
        result.libc_guard_initialization_count = thread.libc_guard_initialization_count;
        result.libc_guard_recursive_acquire_count = thread.libc_guard_recursive_acquire_count;
        result.libc_heap_allocation_count = thread.libc_heap_allocation_count;
        result.libc_heap_free_count = thread.libc_heap_free_count;
        result.libc_heap_realloc_count = thread.libc_heap_realloc_count;
        result.libc_heap_failure_count = thread.libc_heap_failure_count;
        result.libc_heap_live_bytes = thread.libc_heap_live_bytes;
        result.libc_heap_peak_bytes = thread.libc_heap_peak_bytes;
        result.last_libc_atexit_object = thread.last_libc_atexit_object;
        result.last_libc_atexit_destructor = thread.last_libc_atexit_destructor;
        result.last_libc_atexit_dso = thread.last_libc_atexit_dso;
        result.last_libc_finalize_dso = thread.last_libc_finalize_dso;
        result.last_libc_guard_address = thread.last_libc_guard_address;
        result.last_libc_guard_word = thread.last_libc_guard_word;
        result.last_libc_guard_result = thread.last_libc_guard_result;
        result.last_libc_heap_address = thread.last_libc_heap_address;
        result.last_libc_heap_size = thread.last_libc_heap_size;
        result.last_libc_heap_alignment = thread.last_libc_heap_alignment;
        result.exit_status = thread.exit_status;
        result.return_value = thread.return_value;
        result.detail = thread.detail;
        prepared_executable.ready = false;
    }
    core_status.selected_boot_available = prepared_executable.ready;
    core_status.selected_boot_attempted = result.started;
    core_status.title_boot_status = result.detail;
    update_title_boot_summary(result, instruction_limit);
    return result;
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
