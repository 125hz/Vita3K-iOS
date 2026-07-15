#include <vita3k_ios/GuestThread.h>

#include <vita3k_ios/ArmExecution.h>
#include <vita3k_ios/GuestMemory.h>

#include <nids/functions.h>

#include <algorithm>
#include <array>
#include <cstring>
#include <iomanip>
#include <sstream>
#include <string_view>
#include <utility>
#include <vector>

namespace vita3k::ios {
namespace {

constexpr std::uint32_t nid_sce_kernel_get_thread_id = 0x0FB972F9;
constexpr std::uint32_t nid_sce_kernel_exit_thread = 0x0C8A38E1;
constexpr std::uint32_t nid_cxa_set_dso_handle_main = 0xBFE02B3A;
constexpr std::uint32_t nid_aeabi_atexit = 0xEDC939E1;
constexpr std::uint32_t nid_cxa_atexit = 0x33B83B70;
constexpr std::uint32_t nid_cxa_finalize = 0xB538BF48;
constexpr std::uint32_t nid_cxa_guard_abort = 0xD18E461D;
constexpr std::uint32_t nid_cxa_guard_acquire = 0xD0310E31;
constexpr std::uint32_t nid_cxa_guard_release = 0x4ED1056F;
constexpr std::int32_t first_diagnostic_thread_id = 0x10001;

struct LibcAtexitRegistration {
    std::uint32_t object = 0;
    std::uint32_t destructor = 0;
    std::uint32_t dso = 0;
};

bool contains_nid(std::span<const std::uint32_t> nids, std::uint32_t expected) {
    return std::ranges::find(nids, expected) != nids.end();
}

std::string nid_hex(std::uint32_t nid) {
    std::ostringstream stream;
    stream << "0x" << std::hex << std::uppercase << std::setfill('0')
           << std::setw(8) << nid;
    return stream.str();
}

bool read_guest_u32(GuestMemory &memory, std::uint32_t address,
    std::uint32_t &value, std::string &error) {
    std::array<std::uint8_t, sizeof(value)> bytes{};
    if (!memory.read(address, bytes, error)) {
        return false;
    }
    std::memcpy(&value, bytes.data(), sizeof(value));
    return true;
}

bool write_guest_u32(GuestMemory &memory, std::uint32_t address,
    std::uint32_t value, std::string &error) {
    std::array<std::uint8_t, sizeof(value)> bytes{};
    std::memcpy(bytes.data(), &value, sizeof(value));
    return memory.write(address, bytes, error);
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
        result.detail = "The module does not import sceKernelGetThreadId (" + nid_hex(nid_sce_kernel_get_thread_id) + ").";
        return result;
    }
    const bool imports_exit_thread = contains_nid(imported_nids, nid_sce_kernel_exit_thread);
    const bool imports_cxa_set_dso_handle_main = contains_nid(imported_nids, nid_cxa_set_dso_handle_main);
    const bool imports_aeabi_atexit = contains_nid(imported_nids, nid_aeabi_atexit);
    const bool imports_cxa_atexit = contains_nid(imported_nids, nid_cxa_atexit);
    const bool imports_cxa_finalize = contains_nid(imported_nids, nid_cxa_finalize);
    const bool imports_cxa_guard_abort = contains_nid(imported_nids, nid_cxa_guard_abort);
    const bool imports_cxa_guard_acquire = contains_nid(imported_nids, nid_cxa_guard_acquire);
    const bool imports_cxa_guard_release = contains_nid(imported_nids, nid_cxa_guard_release);
    if (std::string_view(import_name(nid_sce_kernel_get_thread_id)) != "sceKernelGetThreadId" || (imports_exit_thread && std::string_view(import_name(nid_sce_kernel_exit_thread)) != "sceKernelExitThread")) {
        result.detail = "The upstream Vita NID database did not match the kernel thread bindings.";
        return result;
    }
    if (imports_cxa_set_dso_handle_main && std::string_view(import_name(nid_cxa_set_dso_handle_main)) != "__cxa_set_dso_handle_main") {
        result.detail = "The upstream Vita NID database did not match the libc runtime binding.";
        return result;
    }
    if ((imports_aeabi_atexit && std::string_view(import_name(nid_aeabi_atexit)) != "__aeabi_atexit")
        || (imports_cxa_atexit && std::string_view(import_name(nid_cxa_atexit)) != "__cxa_atexit")
        || (imports_cxa_finalize && std::string_view(import_name(nid_cxa_finalize)) != "__cxa_finalize")) {
        result.detail = "The upstream Vita NID database did not match the libc termination bindings.";
        return result;
    }
    if ((imports_cxa_guard_abort && std::string_view(import_name(nid_cxa_guard_abort)) != "__cxa_guard_abort")
        || (imports_cxa_guard_acquire && std::string_view(import_name(nid_cxa_guard_acquire)) != "__cxa_guard_acquire")
        || (imports_cxa_guard_release && std::string_view(import_name(nid_cxa_guard_release)) != "__cxa_guard_release")) {
        result.detail = "The upstream Vita NID database did not match the C++ guard bindings.";
        return result;
    }

    HLEDispatcher dispatcher;
    bool exit_requested = false;
    std::vector<LibcAtexitRegistration> atexit_registrations;
    std::vector<std::uint32_t> guards_in_progress;
    std::string guard_error;
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
    bool dso_handle_bound = true;
    if (imports_cxa_set_dso_handle_main) {
        dso_handle_bound = dispatcher.bind(nid_cxa_set_dso_handle_main,
            "__cxa_set_dso_handle_main", [&](ArmCpuState &state) -> std::int32_t {
                ++result.hle_dispatch_count;
                result.libc_dso_handle_main = state.registers[0];
                return 0;
            });
    }
    const auto record_atexit_registration = [&](std::uint32_t object,
                                                std::uint32_t destructor,
                                                std::uint32_t dso) -> std::int32_t {
        atexit_registrations.push_back({
            .object = object,
            .destructor = destructor,
            .dso = dso
        });
        result.libc_atexit_registration_count = atexit_registrations.size();
        result.last_libc_atexit_object = object;
        result.last_libc_atexit_destructor = destructor;
        result.last_libc_atexit_dso = dso;
        return 0;
    };
    bool aeabi_atexit_bound = true;
    if (imports_aeabi_atexit) {
        aeabi_atexit_bound = dispatcher.bind(nid_aeabi_atexit,
            "__aeabi_atexit", [&](ArmCpuState &state) -> std::int32_t {
                ++result.hle_dispatch_count;
                return record_atexit_registration(
                    state.registers[0], state.registers[1], state.registers[2]);
            });
    }
    bool cxa_atexit_bound = true;
    if (imports_cxa_atexit) {
        cxa_atexit_bound = dispatcher.bind(nid_cxa_atexit,
            "__cxa_atexit", [&](ArmCpuState &state) -> std::int32_t {
                ++result.hle_dispatch_count;
                return record_atexit_registration(
                    state.registers[1], state.registers[0], state.registers[2]);
            });
    }
    bool cxa_finalize_bound = true;
    if (imports_cxa_finalize) {
        cxa_finalize_bound = dispatcher.bind(nid_cxa_finalize,
            "__cxa_finalize", [&](ArmCpuState &state) -> std::int32_t {
                ++result.hle_dispatch_count;
                ++result.libc_finalize_call_count;
                result.last_libc_finalize_dso = state.registers[0];
                // Upstream Vita3K currently returns zero here without invoking guest callbacks.
                return 0;
            });
    }
    const auto fail_guard = [&](ArmCpuState &state, std::uint32_t address,
                                std::string operation, std::string error) -> std::int32_t {
        guard_error = std::move(operation) + " failed for " + nid_hex(address) + ": "
            + std::move(error);
        state.stop_requested = true;
        state.stop_code = -1;
        result.last_libc_guard_address = address;
        result.last_libc_guard_result = 0;
        return 0;
    };
    const auto read_guard = [&](ArmCpuState &state, std::uint32_t address,
                                std::string_view operation, std::uint32_t &word) -> bool {
        if (address == 0 || (address & 3u) != 0) {
            fail_guard(state, address, std::string(operation),
                "the Arm guard word is null or not 4-byte aligned");
            return false;
        }
        std::string error;
        if (!read_guest_u32(memory, address, word, error)) {
            fail_guard(state, address, std::string(operation), std::move(error));
            return false;
        }
        result.last_libc_guard_address = address;
        result.last_libc_guard_word = word;
        return true;
    };
    bool cxa_guard_acquire_bound = true;
    if (imports_cxa_guard_acquire) {
        cxa_guard_acquire_bound = dispatcher.bind(nid_cxa_guard_acquire,
            "__cxa_guard_acquire", [&](ArmCpuState &state) -> std::int32_t {
                ++result.hle_dispatch_count;
                ++result.libc_guard_acquire_count;
                const auto address = state.registers[0];
                std::uint32_t word = 0;
                if (!read_guard(state, address, "__cxa_guard_acquire", word)) {
                    return 0;
                }
                if ((word & 1u) != 0) {
                    result.last_libc_guard_result = 0;
                    return 0;
                }
                if (std::ranges::find(guards_in_progress, address) != guards_in_progress.end()) {
                    ++result.libc_guard_recursive_acquire_count;
                    return fail_guard(state, address, "__cxa_guard_acquire",
                        "recursive initialization was detected on the single diagnostic thread");
                }
                guards_in_progress.push_back(address);
                ++result.libc_guard_initialization_count;
                result.last_libc_guard_result = 1;
                return 1;
            });
    }
    bool cxa_guard_release_bound = true;
    if (imports_cxa_guard_release) {
        cxa_guard_release_bound = dispatcher.bind(nid_cxa_guard_release,
            "__cxa_guard_release", [&](ArmCpuState &state) -> std::int32_t {
                ++result.hle_dispatch_count;
                ++result.libc_guard_release_count;
                const auto address = state.registers[0];
                std::uint32_t word = 0;
                if (!read_guard(state, address, "__cxa_guard_release", word)) {
                    return 0;
                }
                const auto initialized_word = word | 1u;
                std::string error;
                if (!write_guest_u32(memory, address, initialized_word, error)) {
                    return fail_guard(state, address, "__cxa_guard_release", std::move(error));
                }
                std::erase(guards_in_progress, address);
                result.last_libc_guard_word = initialized_word;
                result.last_libc_guard_result = 0;
                return 0;
            });
    }
    bool cxa_guard_abort_bound = true;
    if (imports_cxa_guard_abort) {
        cxa_guard_abort_bound = dispatcher.bind(nid_cxa_guard_abort,
            "__cxa_guard_abort", [&](ArmCpuState &state) -> std::int32_t {
                ++result.hle_dispatch_count;
                ++result.libc_guard_abort_count;
                const auto address = state.registers[0];
                std::uint32_t word = 0;
                if (!read_guard(state, address, "__cxa_guard_abort", word)) {
                    return 0;
                }
                std::erase(guards_in_progress, address);
                result.last_libc_guard_result = 0;
                return 0;
            });
    }
    const auto expected_binding_count = 1u
        + static_cast<std::size_t>(imports_exit_thread)
        + static_cast<std::size_t>(imports_cxa_set_dso_handle_main)
        + static_cast<std::size_t>(imports_aeabi_atexit)
        + static_cast<std::size_t>(imports_cxa_atexit)
        + static_cast<std::size_t>(imports_cxa_finalize)
        + static_cast<std::size_t>(imports_cxa_guard_abort)
        + static_cast<std::size_t>(imports_cxa_guard_acquire)
        + static_cast<std::size_t>(imports_cxa_guard_release);
    if (!get_id_bound || !exit_bound || !dso_handle_bound || !aeabi_atexit_bound
        || !cxa_atexit_bound || !cxa_finalize_bound || !cxa_guard_abort_bound
        || !cxa_guard_acquire_bound || !cxa_guard_release_bound
        || dispatcher.binding_count() != expected_binding_count) {
        result.detail = "The minimal kernel/runtime HLE bindings could not be registered.";
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
    result.last_hle_nid = execution.last_hle_nid;
    result.last_guest_pc = execution.final_pc;
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
        detail << "module_start returned " << result.return_value << " through the zero-link sentinel.";
    } else {
        detail << execution.detail;
        if (execution.reason == ArmStopReason::unbound_hle) {
            detail << " Upstream NID name: " << import_name(execution.last_hle_nid)
                   << "; module import inventory: "
                   << (contains_nid(imported_nids, execution.last_hle_nid) ? "present" : "missing")
                   << ".";
        }
    }
    if (result.libc_dso_handle_main != 0) {
        detail << " Runtime DSO handle=" << nid_hex(result.libc_dso_handle_main) << ".";
    }
    if (result.libc_atexit_registration_count != 0) {
        detail << " Libc termination registrations=" << result.libc_atexit_registration_count
               << "; last object=" << nid_hex(result.last_libc_atexit_object)
               << ", destructor=" << nid_hex(result.last_libc_atexit_destructor)
               << ", DSO=" << nid_hex(result.last_libc_atexit_dso) << ".";
    }
    if (result.libc_finalize_call_count != 0) {
        detail << " Libc finalize calls=" << result.libc_finalize_call_count
               << "; last DSO=" << nid_hex(result.last_libc_finalize_dso) << ".";
    }
    const auto guard_call_count = result.libc_guard_acquire_count
        + result.libc_guard_release_count + result.libc_guard_abort_count;
    if (guard_call_count != 0) {
        detail << " C++ guards: acquire=" << result.libc_guard_acquire_count
               << ", release=" << result.libc_guard_release_count
               << ", abort=" << result.libc_guard_abort_count
               << ", initializers=" << result.libc_guard_initialization_count
               << ", recursive=" << result.libc_guard_recursive_acquire_count
               << "; last address=" << nid_hex(result.last_libc_guard_address)
               << ", word=" << nid_hex(result.last_libc_guard_word)
               << ", result=" << result.last_libc_guard_result << ".";
    }
    if (!guard_error.empty()) {
        detail << " Guard boundary: " << guard_error << ".";
    }
    result.detail = detail.str();
    return result;
}

} // namespace vita3k::ios
