#include <vita3k_ios/GuestThread.h>

#include <vita3k_ios/ArmExecution.h>
#include <vita3k_ios/GuestMemory.h>

#include <nids/functions.h>

#include <algorithm>
#include <iomanip>
#include <sstream>
#include <string_view>

namespace vita3k::ios {
namespace {

constexpr std::uint32_t nid_sce_kernel_get_thread_id = 0x0FB972F9;
constexpr std::uint32_t nid_sce_kernel_exit_thread = 0x0C8A38E1;
constexpr std::int32_t first_diagnostic_thread_id = 0x10001;

bool contains_nid(std::span<const std::uint32_t> nids, std::uint32_t expected) {
    return std::ranges::find(nids, expected) != nids.end();
}

std::string nid_hex(std::uint32_t nid) {
    std::ostringstream stream;
    stream << "0x" << std::hex << std::uppercase << std::setfill('0')
           << std::setw(8) << nid;
    return stream.str();
}

} // namespace

GuestThreadRunResult run_guest_module_start(GuestMemory &memory,
    std::uint32_t entry_point,
    std::uint32_t stack_pointer,
    std::span<const std::uint32_t> imported_nids,
    std::string thread_name,
    std::size_t instruction_limit) {
    GuestThreadRunResult result{
        .attempted = true,
        .thread_id = first_diagnostic_thread_id
    };
    const bool thumb_entry = (entry_point & 1u) != 0;
    if (entry_point == 0 || (!thumb_entry && (entry_point & 3u) != 0)) {
        result.detail = "The module_start entry point is zero or not aligned for ARM/Thumb.";
        return result;
    }
    if (stack_pointer == 0 || (stack_pointer & 7u) != 0) {
        result.detail = "The temporary guest stack pointer is missing or misaligned.";
        return result;
    }
    if (!contains_nid(imported_nids, nid_sce_kernel_get_thread_id)) {
        result.detail = "The module does not import sceKernelGetThreadId (" +
            nid_hex(nid_sce_kernel_get_thread_id) + ").";
        return result;
    }
    const bool imports_exit_thread = contains_nid(imported_nids, nid_sce_kernel_exit_thread);
    if (std::string_view(import_name(nid_sce_kernel_get_thread_id)) != "sceKernelGetThreadId" ||
        (imports_exit_thread &&
            std::string_view(import_name(nid_sce_kernel_exit_thread)) != "sceKernelExitThread")) {
        result.detail = "The upstream Vita NID database did not match the kernel thread bindings.";
        return result;
    }

    HLEDispatcher dispatcher;
    bool exit_requested = false;
    result.hle_dispatch_count = 0;
    const bool get_id_bound = dispatcher.bind(nid_sce_kernel_get_thread_id,
        "sceKernelGetThreadId", [&](ArmCpuState &) -> std::int32_t {
            ++result.hle_dispatch_count;
            return result.thread_id;
        });
    bool exit_bound = true;
    if (imports_exit_thread) {
        exit_bound = dispatcher.bind(nid_sce_kernel_exit_thread,
            "sceKernelExitThread", [&](ArmCpuState &state) -> std::int32_t {
                ++result.hle_dispatch_count;
                result.exit_status = static_cast<std::int32_t>(state.registers[0]);
                exit_requested = true;
                state.stop_requested = true;
                state.stop_code = result.exit_status;
                return 0;
            });
    }
    const auto expected_binding_count = imports_exit_thread ? 2u : 1u;
    if (!get_id_bound || !exit_bound || dispatcher.binding_count() != expected_binding_count) {
        result.detail = "The minimal kernel HLE bindings could not be registered.";
        return result;
    }

    ArmInterpreter interpreter(memory, dispatcher);
    interpreter.reset(entry_point, stack_pointer);
    // Vita module_start receives arglen in r0 and argp in r1.
    interpreter.state().registers[0] = 0;
    interpreter.state().registers[1] = 0;
    result.started = true;
    const auto execution = interpreter.run(instruction_limit);
    const auto &state = interpreter.state();
    result.instruction_count = execution.instructions_executed;
    result.observed_thread_id = state.registers[2];
    result.exited = execution.halted() && exit_requested && state.stop_requested;
    result.returned = execution.halted() && !exit_requested && !state.stop_requested;
    result.return_value = state.registers[0];

    std::ostringstream detail;
    detail << "Thread " << (thread_name.empty() ? "<unnamed>" : thread_name)
           << " (UID 0x" << std::hex << std::uppercase << result.thread_id << std::dec
           << ") executed " << result.instruction_count << " instructions and "
           << result.hle_dispatch_count << " HLE call"
           << (result.hle_dispatch_count == 1 ? "" : "s") << "; ";
    if (result.exited) {
        detail << "sceKernelExitThread(" << result.exit_status << ") completed.";
    } else if (result.returned) {
        detail << "module_start returned " << result.return_value <<
            " through the zero-link sentinel.";
    } else {
        detail << execution.detail;
    }
    result.detail = detail.str();
    return result;
}

} // namespace vita3k::ios
