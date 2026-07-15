#include <vita3k_ios/GuestThread.h>

#include <vita3k_ios/ArmExecution.h>
#include <vita3k_ios/GuestHeap.h>
#include <vita3k_ios/GuestMemory.h>

#include <nids/functions.h>

#include <algorithm>
#include <array>
#include <cstring>
#include <iomanip>
#include <limits>
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
constexpr std::uint32_t nid_calloc = 0xE7EC3D0B;
constexpr std::uint32_t nid_free = 0x5B9BB802;
constexpr std::uint32_t nid_malloc = 0x775A0CB2;
constexpr std::uint32_t nid_malloc_usable_size = 0x54A54EB1;
constexpr std::uint32_t nid_memalign = 0xA9363E6B;
constexpr std::uint32_t nid_realloc = 0x006B54BA;
constexpr std::uint32_t nid_delete_array = 0x91B0DC47;
constexpr std::uint32_t nid_delete_array_nothrow = 0xA7241F09;
constexpr std::uint32_t nid_delete_array_placement = 0x3688FFDA;
constexpr std::uint32_t nid_delete = 0x72293931;
constexpr std::uint32_t nid_delete_nothrow = 0x87EF85FF;
constexpr std::uint32_t nid_delete_placement = 0x1EB89099;
constexpr std::uint32_t nid_new_array = 0xE7FB2BF4;
constexpr std::uint32_t nid_new_array_nothrow = 0x31C62481;
constexpr std::uint32_t nid_new = 0xF99ED5AC;
constexpr std::uint32_t nid_new_nothrow = 0x0AE71DC3;
constexpr std::int32_t first_diagnostic_thread_id = 0x10001;
constexpr std::uint32_t diagnostic_heap_base = 0x90000000;
constexpr std::uint32_t diagnostic_heap_size = 16 * 1024 * 1024;

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
    const bool imports_calloc = contains_nid(imported_nids, nid_calloc);
    const bool imports_free = contains_nid(imported_nids, nid_free);
    const bool imports_malloc = contains_nid(imported_nids, nid_malloc);
    const bool imports_malloc_usable_size = contains_nid(imported_nids, nid_malloc_usable_size);
    const bool imports_memalign = contains_nid(imported_nids, nid_memalign);
    const bool imports_realloc = contains_nid(imported_nids, nid_realloc);
    const bool imports_delete_array = contains_nid(imported_nids, nid_delete_array);
    const bool imports_delete_array_nothrow = contains_nid(imported_nids, nid_delete_array_nothrow);
    const bool imports_delete_array_placement = contains_nid(imported_nids, nid_delete_array_placement);
    const bool imports_delete = contains_nid(imported_nids, nid_delete);
    const bool imports_delete_nothrow = contains_nid(imported_nids, nid_delete_nothrow);
    const bool imports_delete_placement = contains_nid(imported_nids, nid_delete_placement);
    const bool imports_new_array = contains_nid(imported_nids, nid_new_array);
    const bool imports_new_array_nothrow = contains_nid(imported_nids, nid_new_array_nothrow);
    const bool imports_new = contains_nid(imported_nids, nid_new);
    const bool imports_new_nothrow = contains_nid(imported_nids, nid_new_nothrow);
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
    if ((imports_calloc && std::string_view(import_name(nid_calloc)) != "calloc")
        || (imports_free && std::string_view(import_name(nid_free)) != "free")
        || (imports_malloc && std::string_view(import_name(nid_malloc)) != "malloc")
        || (imports_malloc_usable_size && std::string_view(import_name(nid_malloc_usable_size)) != "malloc_usable_size")
        || (imports_memalign && std::string_view(import_name(nid_memalign)) != "memalign")
        || (imports_realloc && std::string_view(import_name(nid_realloc)) != "realloc")) {
        result.detail = "The upstream Vita NID database did not match the libc heap bindings.";
        return result;
    }
    if ((imports_delete_array && std::string_view(import_name(nid_delete_array)) != "_ZdaPv")
        || (imports_delete_array_nothrow && std::string_view(import_name(nid_delete_array_nothrow)) != "_ZdaPvRKSt9nothrow_t")
        || (imports_delete_array_placement && std::string_view(import_name(nid_delete_array_placement)) != "_ZdaPvS_")
        || (imports_delete && std::string_view(import_name(nid_delete)) != "_ZdlPv")
        || (imports_delete_nothrow && std::string_view(import_name(nid_delete_nothrow)) != "_ZdlPvRKSt9nothrow_t")
        || (imports_delete_placement && std::string_view(import_name(nid_delete_placement)) != "_ZdlPvS_")
        || (imports_new_array && std::string_view(import_name(nid_new_array)) != "_Znaj")
        || (imports_new_array_nothrow && std::string_view(import_name(nid_new_array_nothrow)) != "_ZnajRKSt9nothrow_t")
        || (imports_new && std::string_view(import_name(nid_new)) != "_Znwj")
        || (imports_new_nothrow && std::string_view(import_name(nid_new_nothrow)) != "_ZnwjRKSt9nothrow_t")) {
        result.detail = "The upstream Vita NID database did not match the C++ allocation bindings.";
        return result;
    }

    HLEDispatcher dispatcher;
    bool exit_requested = false;
    std::vector<LibcAtexitRegistration> atexit_registrations;
    std::vector<std::uint32_t> guards_in_progress;
    std::string guard_error;
    GuestHeap heap;
    std::string heap_error;
    const bool imports_heap = imports_calloc || imports_free || imports_malloc
        || imports_malloc_usable_size || imports_memalign || imports_realloc
        || imports_delete_array || imports_delete_array_nothrow
        || imports_delete || imports_delete_nothrow
        || imports_new_array || imports_new_array_nothrow
        || imports_new || imports_new_nothrow;
    if (imports_heap && !heap.initialize(memory,
            diagnostic_heap_base, diagnostic_heap_size, heap_error)) {
        result.detail = "The bounded libc heap could not be initialized: " + heap_error;
        return result;
    }
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
    const auto sync_heap_stats = [&] {
        result.libc_heap_allocation_count = heap.stats().allocation_count;
        result.libc_heap_free_count = heap.stats().free_count;
        result.libc_heap_realloc_count = heap.stats().realloc_count;
        result.libc_heap_live_bytes = heap.stats().live_bytes;
        result.libc_heap_peak_bytes = heap.stats().peak_bytes;
    };
    const auto fail_heap = [&](ArmCpuState &state, std::string operation,
                               std::string error) -> std::int32_t {
        ++result.libc_heap_failure_count;
        heap_error = std::move(operation) + " failed: " + std::move(error);
        state.stop_requested = true;
        state.stop_code = -1;
        sync_heap_stats();
        return 0;
    };
    bool calloc_bound = true;
    if (imports_calloc) {
        calloc_bound = dispatcher.bind(nid_calloc,
            "calloc", [&](ArmCpuState &state) -> std::int32_t {
                ++result.hle_dispatch_count;
                const auto count = state.registers[0];
                const auto element_size = state.registers[1];
                const auto total = static_cast<std::uint64_t>(count) * element_size;
                result.last_libc_heap_size = total <= std::numeric_limits<std::uint32_t>::max()
                    ? static_cast<std::uint32_t>(total) : 0;
                result.last_libc_heap_alignment = 16;
                if (total > std::numeric_limits<std::uint32_t>::max()) {
                    return fail_heap(state, "calloc", "the element count and size overflow 32 bits");
                }
                std::string error;
                const auto address = heap.allocate(static_cast<std::uint32_t>(total), 16, true, error);
                if (address == 0) {
                    return fail_heap(state, "calloc", std::move(error));
                }
                result.last_libc_heap_address = address;
                sync_heap_stats();
                return static_cast<std::int32_t>(address);
            });
    }
    bool malloc_bound = true;
    if (imports_malloc) {
        malloc_bound = dispatcher.bind(nid_malloc,
            "malloc", [&](ArmCpuState &state) -> std::int32_t {
                ++result.hle_dispatch_count;
                result.last_libc_heap_size = state.registers[0];
                result.last_libc_heap_alignment = 16;
                std::string error;
                const auto address = heap.allocate(state.registers[0], 16, false, error);
                if (address == 0) {
                    return fail_heap(state, "malloc", std::move(error));
                }
                result.last_libc_heap_address = address;
                sync_heap_stats();
                return static_cast<std::int32_t>(address);
            });
    }
    bool memalign_bound = true;
    if (imports_memalign) {
        memalign_bound = dispatcher.bind(nid_memalign,
            "memalign", [&](ArmCpuState &state) -> std::int32_t {
                ++result.hle_dispatch_count;
                const auto alignment = state.registers[0];
                const auto size = state.registers[1];
                result.last_libc_heap_size = size;
                result.last_libc_heap_alignment = alignment;
                std::string error;
                const auto address = heap.allocate(size, alignment, false, error);
                if (address == 0) {
                    return fail_heap(state, "memalign", std::move(error));
                }
                result.last_libc_heap_address = address;
                sync_heap_stats();
                return static_cast<std::int32_t>(address);
            });
    }
    bool free_bound = true;
    if (imports_free) {
        free_bound = dispatcher.bind(nid_free,
            "free", [&](ArmCpuState &state) -> std::int32_t {
                ++result.hle_dispatch_count;
                result.last_libc_heap_address = state.registers[0];
                result.last_libc_heap_size = 0;
                result.last_libc_heap_alignment = 0;
                std::string error;
                if (!heap.free(state.registers[0], error)) {
                    return fail_heap(state, "free", std::move(error));
                }
                sync_heap_stats();
                return 0;
            });
    }
    bool realloc_bound = true;
    if (imports_realloc) {
        realloc_bound = dispatcher.bind(nid_realloc,
            "realloc", [&](ArmCpuState &state) -> std::int32_t {
                ++result.hle_dispatch_count;
                result.last_libc_heap_size = state.registers[1];
                result.last_libc_heap_alignment = 16;
                std::string error;
                const auto address = heap.reallocate(state.registers[0], state.registers[1], error);
                if (address == 0 && (state.registers[1] != 0 || !error.empty())) {
                    return fail_heap(state, "realloc", std::move(error));
                }
                result.last_libc_heap_address = address;
                sync_heap_stats();
                return static_cast<std::int32_t>(address);
            });
    }
    bool malloc_usable_size_bound = true;
    if (imports_malloc_usable_size) {
        malloc_usable_size_bound = dispatcher.bind(nid_malloc_usable_size,
            "malloc_usable_size", [&](ArmCpuState &state) -> std::int32_t {
                ++result.hle_dispatch_count;
                result.last_libc_heap_address = state.registers[0];
                result.last_libc_heap_alignment = 0;
                std::uint32_t size = 0;
                std::string error;
                if (!heap.usable_size(state.registers[0], size, error)) {
                    return fail_heap(state, "malloc_usable_size", std::move(error));
                }
                result.last_libc_heap_size = size;
                sync_heap_stats();
                return static_cast<std::int32_t>(size);
            });
    }
    const auto bind_cpp_new = [&](bool imported, std::uint32_t nid,
                                  std::string_view name, bool array,
                                  bool nothrow) {
        if (!imported) {
            return true;
        }
        return dispatcher.bind(nid, std::string(name),
            [&, name, array, nothrow](ArmCpuState &state) -> std::int32_t {
                ++result.hle_dispatch_count;
                if (array) {
                    ++result.cxx_new_array_call_count;
                } else {
                    ++result.cxx_new_call_count;
                }
                result.last_libc_heap_size = state.registers[0];
                result.last_libc_heap_alignment = 16;
                std::string error;
                const auto address = heap.allocate(state.registers[0], 16, false, error);
                if (address == 0) {
                    result.last_libc_heap_address = 0;
                    if (nothrow) {
                        ++result.libc_heap_failure_count;
                        ++result.cxx_nothrow_failure_count;
                        sync_heap_stats();
                        return 0;
                    }
                    return fail_heap(state, std::string(name), std::move(error)
                        + "; guest C++ allocation-failure unwinding is not implemented");
                }
                result.last_libc_heap_address = address;
                sync_heap_stats();
                return static_cast<std::int32_t>(address);
            });
    };
    const auto bind_cpp_delete = [&](bool imported, std::uint32_t nid,
                                     std::string_view name, bool array,
                                     bool placement) {
        if (!imported) {
            return true;
        }
        return dispatcher.bind(nid, std::string(name),
            [&, name, array, placement](ArmCpuState &state) -> std::int32_t {
                ++result.hle_dispatch_count;
                if (array) {
                    ++result.cxx_delete_array_call_count;
                } else {
                    ++result.cxx_delete_call_count;
                }
                result.last_libc_heap_address = state.registers[0];
                result.last_libc_heap_size = 0;
                result.last_libc_heap_alignment = 0;
                if (placement) {
                    ++result.cxx_placement_delete_call_count;
                    return 0;
                }
                std::string error;
                if (!heap.free(state.registers[0], error)) {
                    return fail_heap(state, std::string(name), std::move(error));
                }
                sync_heap_stats();
                return 0;
            });
    };
    const bool new_bound = bind_cpp_new(
        imports_new, nid_new, "_Znwj", false, false);
    const bool new_nothrow_bound = bind_cpp_new(
        imports_new_nothrow, nid_new_nothrow, "_ZnwjRKSt9nothrow_t", false, true);
    const bool new_array_bound = bind_cpp_new(
        imports_new_array, nid_new_array, "_Znaj", true, false);
    const bool new_array_nothrow_bound = bind_cpp_new(
        imports_new_array_nothrow, nid_new_array_nothrow,
        "_ZnajRKSt9nothrow_t", true, true);
    const bool delete_bound = bind_cpp_delete(
        imports_delete, nid_delete, "_ZdlPv", false, false);
    const bool delete_nothrow_bound = bind_cpp_delete(
        imports_delete_nothrow, nid_delete_nothrow,
        "_ZdlPvRKSt9nothrow_t", false, false);
    const bool delete_placement_bound = bind_cpp_delete(
        imports_delete_placement, nid_delete_placement, "_ZdlPvS_", false, true);
    const bool delete_array_bound = bind_cpp_delete(
        imports_delete_array, nid_delete_array, "_ZdaPv", true, false);
    const bool delete_array_nothrow_bound = bind_cpp_delete(
        imports_delete_array_nothrow, nid_delete_array_nothrow,
        "_ZdaPvRKSt9nothrow_t", true, false);
    const bool delete_array_placement_bound = bind_cpp_delete(
        imports_delete_array_placement, nid_delete_array_placement,
        "_ZdaPvS_", true, true);
    const auto expected_binding_count = 1u
        + static_cast<std::size_t>(imports_exit_thread)
        + static_cast<std::size_t>(imports_cxa_set_dso_handle_main)
        + static_cast<std::size_t>(imports_aeabi_atexit)
        + static_cast<std::size_t>(imports_cxa_atexit)
        + static_cast<std::size_t>(imports_cxa_finalize)
        + static_cast<std::size_t>(imports_cxa_guard_abort)
        + static_cast<std::size_t>(imports_cxa_guard_acquire)
        + static_cast<std::size_t>(imports_cxa_guard_release)
        + static_cast<std::size_t>(imports_calloc)
        + static_cast<std::size_t>(imports_free)
        + static_cast<std::size_t>(imports_malloc)
        + static_cast<std::size_t>(imports_malloc_usable_size)
        + static_cast<std::size_t>(imports_memalign)
        + static_cast<std::size_t>(imports_realloc)
        + static_cast<std::size_t>(imports_delete_array)
        + static_cast<std::size_t>(imports_delete_array_nothrow)
        + static_cast<std::size_t>(imports_delete_array_placement)
        + static_cast<std::size_t>(imports_delete)
        + static_cast<std::size_t>(imports_delete_nothrow)
        + static_cast<std::size_t>(imports_delete_placement)
        + static_cast<std::size_t>(imports_new_array)
        + static_cast<std::size_t>(imports_new_array_nothrow)
        + static_cast<std::size_t>(imports_new)
        + static_cast<std::size_t>(imports_new_nothrow);
    if (!get_id_bound || !exit_bound || !dso_handle_bound || !aeabi_atexit_bound
        || !cxa_atexit_bound || !cxa_finalize_bound || !cxa_guard_abort_bound
        || !cxa_guard_acquire_bound || !cxa_guard_release_bound
        || !calloc_bound || !free_bound || !malloc_bound || !malloc_usable_size_bound
        || !memalign_bound || !realloc_bound
        || !new_bound || !new_nothrow_bound || !new_array_bound || !new_array_nothrow_bound
        || !delete_bound || !delete_nothrow_bound || !delete_placement_bound
        || !delete_array_bound || !delete_array_nothrow_bound
        || !delete_array_placement_bound
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
    result.unique_pc_count = execution.unique_pc_count;
    result.hottest_pc = execution.hottest_pc;
    result.hottest_pc_hits = execution.hottest_pc_hits;
    result.non_forward_pc_count = execution.non_forward_pc_count;
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
    if (result.libc_heap_allocation_count != 0 || result.libc_heap_free_count != 0
        || result.libc_heap_realloc_count != 0 || result.libc_heap_failure_count != 0) {
        detail << " Libc heap: allocations=" << result.libc_heap_allocation_count
               << ", frees=" << result.libc_heap_free_count
               << ", reallocs=" << result.libc_heap_realloc_count
               << ", failures=" << result.libc_heap_failure_count
               << ", live=" << result.libc_heap_live_bytes
               << ", peak=" << result.libc_heap_peak_bytes
               << "; last address=" << nid_hex(result.last_libc_heap_address)
               << ", size=" << result.last_libc_heap_size
               << ", alignment=" << result.last_libc_heap_alignment << ".";
    }
    if (!heap_error.empty()) {
        detail << " Heap boundary: " << heap_error << ".";
    }
    const auto cxx_allocation_call_count = result.cxx_new_call_count
        + result.cxx_new_array_call_count;
    const auto cxx_delete_call_count = result.cxx_delete_call_count
        + result.cxx_delete_array_call_count;
    if (cxx_allocation_call_count != 0 || cxx_delete_call_count != 0) {
        detail << " C++ allocation ABI: new=" << result.cxx_new_call_count
               << ", new[]=" << result.cxx_new_array_call_count
               << ", delete=" << result.cxx_delete_call_count
               << ", delete[]=" << result.cxx_delete_array_call_count
               << ", nothrow failures=" << result.cxx_nothrow_failure_count
               << ", placement deletes=" << result.cxx_placement_delete_call_count
               << ".";
    }
    result.detail = detail.str();
    return result;
}

} // namespace vita3k::ios
