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
constexpr std::uint32_t nid_sce_app_util_init = 0xDAFFE671;
constexpr std::uint32_t nid_sce_app_util_shutdown = 0xB220B00B;
constexpr std::uint32_t nid_sce_sysmodule_is_loaded = 0x53099B7A;
constexpr std::uint32_t nid_sce_sysmodule_load_module = 0x79A0160A;
constexpr std::uint32_t nid_sce_sysmodule_unload_module = 0x31D87805;
constexpr std::uint32_t nid_sce_np_get_service_state = 0x54060DF6;
constexpr std::uint32_t nid_sce_np_init = 0x04D9F484;
constexpr std::uint32_t nid_sce_np_term = 0x19E40AE1;
constexpr std::uint32_t nid_sce_np_trophy_init = 0x34516838;
constexpr std::uint32_t nid_sce_np_trophy_term = 0xBFE0F28F;
constexpr std::uint32_t nid_sce_np_trophy_abort_handle = 0xD55C6F4C;
constexpr std::uint32_t nid_sce_np_trophy_create_context = 0xC49FD33F;
constexpr std::uint32_t nid_sce_np_trophy_create_handle = 0x4EBC6977;
constexpr std::uint32_t nid_sce_np_trophy_destroy_context = 0x56F5CBA5;
constexpr std::uint32_t nid_sce_np_trophy_destroy_handle = 0xFF142071;
constexpr std::uint32_t nid_sce_ctrl_get_controller_port_info = 0x324F1B66;
constexpr std::uint32_t nid_sce_ctrl_get_sampling_mode = 0xEC752AAF;
constexpr std::uint32_t nid_sce_ctrl_get_sampling_mode_ext = 0xBD27F830;
constexpr std::uint32_t nid_sce_ctrl_get_wireless_controller_info = 0xB8844141;
constexpr std::uint32_t nid_sce_ctrl_is_multi_controller_supported = 0x1FFFD965;
constexpr std::uint32_t nid_sce_ctrl_peek_buffer_negative = 0x104ED1A7;
constexpr std::uint32_t nid_sce_ctrl_peek_buffer_negative2 = 0x81A89660;
constexpr std::uint32_t nid_sce_ctrl_peek_buffer_positive = 0xA9C3CED6;
constexpr std::uint32_t nid_sce_ctrl_peek_buffer_positive2 = 0x15F81E8C;
constexpr std::uint32_t nid_sce_ctrl_peek_buffer_positive_ext = 0xA59454D3;
constexpr std::uint32_t nid_sce_ctrl_peek_buffer_positive_ext2 = 0x860BF292;
constexpr std::uint32_t nid_sce_ctrl_read_buffer_negative = 0x15F96FB0;
constexpr std::uint32_t nid_sce_ctrl_read_buffer_negative2 = 0x27A0C5FB;
constexpr std::uint32_t nid_sce_ctrl_read_buffer_positive = 0x67E7AB83;
constexpr std::uint32_t nid_sce_ctrl_read_buffer_positive2 = 0xC4226A3E;
constexpr std::uint32_t nid_sce_ctrl_read_buffer_positive_ext = 0xE2D99296;
constexpr std::uint32_t nid_sce_ctrl_read_buffer_positive_ext2 = 0xA7178860;
constexpr std::uint32_t nid_sce_ctrl_set_sampling_mode = 0xA497B150;
constexpr std::uint32_t nid_sce_ctrl_set_sampling_mode_ext = 0xB1DC4378;
constexpr std::uint32_t sce_app_util_error_parameter = 0x80100600;
constexpr std::uint32_t sce_app_util_error_not_initialized = 0x80100601;
constexpr std::uint32_t sce_app_util_error_busy = 0x80100603;
constexpr std::size_t sce_app_util_init_param_size = 0x40;
constexpr std::size_t sce_app_util_boot_param_size = 0x28;
constexpr std::uint32_t sce_sysmodule_count = 0x56;
constexpr std::uint32_t sce_sysmodule_error_invalid_value = 0x805A1000;
constexpr std::uint32_t sce_sysmodule_error_unloaded = 0x805A1001;
constexpr std::uint32_t sce_np_error_already_initialized = 0x80550001;
constexpr std::uint32_t sce_np_error_invalid_argument = 0x80550003;
constexpr std::uint32_t sce_np_trophy_error_not_initialized = 0x80551601;
constexpr std::uint32_t sce_np_trophy_error_already_initialized = 0x80551602;
constexpr std::uint32_t sce_np_trophy_error_invalid_argument = 0x80551604;
constexpr std::uint32_t sce_ctrl_error_invalid_arg = 0x80340001;
constexpr std::uint32_t sce_ctrl_error_no_device = 0x80340020;
constexpr std::uint32_t sce_ctrl_mode_analog_wide = 2;
constexpr std::size_t sce_ctrl_data_size = 32;
constexpr std::uint32_t sce_ctrl_max_samples = 64;
constexpr std::uint32_t sce_np_trophy_error_invalid_context = 0x80551609;
constexpr std::uint32_t sce_np_trophy_error_invalid_np_comm_id = 0x8055160A;
constexpr std::uint32_t sce_np_trophy_error_context_already_exists = 0x80551616;
constexpr std::uint32_t sce_np_service_state_signed_out = 1;
constexpr std::size_t sce_np_communication_config_size = 12;
constexpr std::size_t sce_np_communication_id_size = 12;
constexpr std::int32_t first_diagnostic_thread_id = 0x10001;
constexpr std::uint32_t diagnostic_heap_base = 0x90000000;
constexpr std::uint32_t diagnostic_heap_size = 16 * 1024 * 1024;

struct LibcAtexitRegistration {
    std::uint32_t object = 0;
    std::uint32_t destructor = 0;
    std::uint32_t dso = 0;
};

struct CtrlImport {
    std::uint32_t nid;
    std::string_view name;
};

constexpr std::array<CtrlImport, 19> ctrl_imports{{
    { nid_sce_ctrl_get_controller_port_info, "sceCtrlGetControllerPortInfo" },
    { nid_sce_ctrl_get_sampling_mode, "sceCtrlGetSamplingMode" },
    { nid_sce_ctrl_get_sampling_mode_ext, "sceCtrlGetSamplingModeExt" },
    { nid_sce_ctrl_get_wireless_controller_info, "sceCtrlGetWirelessControllerInfo" },
    { nid_sce_ctrl_is_multi_controller_supported, "sceCtrlIsMultiControllerSupported" },
    { nid_sce_ctrl_peek_buffer_negative, "sceCtrlPeekBufferNegative" },
    { nid_sce_ctrl_peek_buffer_negative2, "sceCtrlPeekBufferNegative2" },
    { nid_sce_ctrl_peek_buffer_positive, "sceCtrlPeekBufferPositive" },
    { nid_sce_ctrl_peek_buffer_positive2, "sceCtrlPeekBufferPositive2" },
    { nid_sce_ctrl_peek_buffer_positive_ext, "sceCtrlPeekBufferPositiveExt" },
    { nid_sce_ctrl_peek_buffer_positive_ext2, "sceCtrlPeekBufferPositiveExt2" },
    { nid_sce_ctrl_read_buffer_negative, "sceCtrlReadBufferNegative" },
    { nid_sce_ctrl_read_buffer_negative2, "sceCtrlReadBufferNegative2" },
    { nid_sce_ctrl_read_buffer_positive, "sceCtrlReadBufferPositive" },
    { nid_sce_ctrl_read_buffer_positive2, "sceCtrlReadBufferPositive2" },
    { nid_sce_ctrl_read_buffer_positive_ext, "sceCtrlReadBufferPositiveExt" },
    { nid_sce_ctrl_read_buffer_positive_ext2, "sceCtrlReadBufferPositiveExt2" },
    { nid_sce_ctrl_set_sampling_mode, "sceCtrlSetSamplingMode" },
    { nid_sce_ctrl_set_sampling_mode_ext, "sceCtrlSetSamplingModeExt" },
}};

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
    const bool imports_sce_app_util_init = contains_nid(imported_nids, nid_sce_app_util_init);
    const bool imports_sce_app_util_shutdown = contains_nid(imported_nids, nid_sce_app_util_shutdown);
    const bool imports_sce_sysmodule_is_loaded = contains_nid(
        imported_nids, nid_sce_sysmodule_is_loaded);
    const bool imports_sce_sysmodule_load_module = contains_nid(
        imported_nids, nid_sce_sysmodule_load_module);
    const bool imports_sce_sysmodule_unload_module = contains_nid(
        imported_nids, nid_sce_sysmodule_unload_module);
    const bool imports_sce_np_get_service_state = contains_nid(
        imported_nids, nid_sce_np_get_service_state);
    const bool imports_sce_np_init = contains_nid(imported_nids, nid_sce_np_init);
    const bool imports_sce_np_term = contains_nid(imported_nids, nid_sce_np_term);
    const bool imports_sce_np_trophy_init = contains_nid(
        imported_nids, nid_sce_np_trophy_init);
    const bool imports_sce_np_trophy_term = contains_nid(
        imported_nids, nid_sce_np_trophy_term);
    const bool imports_sce_np_trophy_abort_handle = contains_nid(
        imported_nids, nid_sce_np_trophy_abort_handle);
    const bool imports_sce_np_trophy_create_context = contains_nid(
        imported_nids, nid_sce_np_trophy_create_context);
    const bool imports_sce_np_trophy_create_handle = contains_nid(
        imported_nids, nid_sce_np_trophy_create_handle);
    const bool imports_sce_np_trophy_destroy_context = contains_nid(
        imported_nids, nid_sce_np_trophy_destroy_context);
    const bool imports_sce_np_trophy_destroy_handle = contains_nid(
        imported_nids, nid_sce_np_trophy_destroy_handle);
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
    if ((imports_sce_app_util_init
            && std::string_view(import_name(nid_sce_app_util_init)) != "sceAppUtilInit")
        || (imports_sce_app_util_shutdown
            && std::string_view(import_name(nid_sce_app_util_shutdown)) != "sceAppUtilShutdown")) {
        result.detail = "The upstream Vita NID database did not match the AppUtil lifecycle bindings.";
        return result;
    }
    if ((imports_sce_sysmodule_is_loaded
            && std::string_view(import_name(nid_sce_sysmodule_is_loaded))
                != "sceSysmoduleIsLoaded")
        || (imports_sce_sysmodule_load_module
            && std::string_view(import_name(nid_sce_sysmodule_load_module))
                != "sceSysmoduleLoadModule")
        || (imports_sce_sysmodule_unload_module
            && std::string_view(import_name(nid_sce_sysmodule_unload_module))
                != "sceSysmoduleUnloadModule")) {
        result.detail = "The upstream Vita NID database did not match the Sysmodule lifecycle bindings.";
        return result;
    }
    if ((imports_sce_np_get_service_state
            && std::string_view(import_name(nid_sce_np_get_service_state))
                != "sceNpGetServiceState")
        || (imports_sce_np_init
            && std::string_view(import_name(nid_sce_np_init)) != "sceNpInit")
        || (imports_sce_np_term
            && std::string_view(import_name(nid_sce_np_term)) != "sceNpTerm")
        || (imports_sce_np_trophy_init
            && std::string_view(import_name(nid_sce_np_trophy_init))
                != "sceNpTrophyInit")
        || (imports_sce_np_trophy_term
            && std::string_view(import_name(nid_sce_np_trophy_term))
                != "sceNpTrophyTerm")
        || (imports_sce_np_trophy_abort_handle
            && std::string_view(import_name(nid_sce_np_trophy_abort_handle))
                != "sceNpTrophyAbortHandle")
        || (imports_sce_np_trophy_create_context
            && std::string_view(import_name(nid_sce_np_trophy_create_context))
                != "sceNpTrophyCreateContext")
        || (imports_sce_np_trophy_create_handle
            && std::string_view(import_name(nid_sce_np_trophy_create_handle))
                != "sceNpTrophyCreateHandle")
        || (imports_sce_np_trophy_destroy_context
            && std::string_view(import_name(nid_sce_np_trophy_destroy_context))
                != "sceNpTrophyDestroyContext")
        || (imports_sce_np_trophy_destroy_handle
            && std::string_view(import_name(nid_sce_np_trophy_destroy_handle))
                != "sceNpTrophyDestroyHandle")) {
        result.detail = "The upstream Vita NID database did not match the NP lifecycle bindings.";
        return result;
    }
    std::size_t imported_ctrl_count = 0;
    for (const auto &ctrl_import : ctrl_imports) {
        if (!contains_nid(imported_nids, ctrl_import.nid)) {
            continue;
        }
        ++imported_ctrl_count;
        if (std::string_view(import_name(ctrl_import.nid)) != ctrl_import.name) {
            result.detail = "The upstream Vita NID database did not match the controller bindings.";
            return result;
        }
    }

    HLEDispatcher dispatcher;
    bool exit_requested = false;
    std::vector<LibcAtexitRegistration> atexit_registrations;
    std::vector<std::uint32_t> guards_in_progress;
    std::string guard_error;
    std::string app_util_error;
    bool app_util_initialized = false;
    std::vector<std::uint32_t> loaded_sysmodules;
    bool np_initialized = false;
    bool np_trophy_initialized = false;
    std::vector<std::int32_t> np_trophy_contexts;
    std::vector<std::int32_t> np_trophy_handles;
    std::string np_error;
    std::string ctrl_error;
    std::uint64_t ctrl_timestamp = 0;
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
    const auto set_app_util_result = [&](std::uint32_t value) -> std::int32_t {
        result.last_app_util_result = static_cast<std::int32_t>(value);
        return result.last_app_util_result;
    };
    const auto fail_app_util_memory = [&](ArmCpuState &state, std::string operation,
                                          std::string error) -> std::int32_t {
        app_util_error = std::move(operation) + " guest-memory validation failed: "
            + std::move(error);
        state.stop_requested = true;
        state.stop_code = -1;
        return set_app_util_result(sce_app_util_error_parameter);
    };
    bool app_util_init_bound = true;
    if (imports_sce_app_util_init) {
        app_util_init_bound = dispatcher.bind(nid_sce_app_util_init,
            "sceAppUtilInit", [&](ArmCpuState &state) -> std::int32_t {
                ++result.hle_dispatch_count;
                ++result.app_util_init_call_count;
                result.last_app_util_init_param = state.registers[0];
                result.last_app_util_boot_param = state.registers[1];
                if (app_util_initialized) {
                    return set_app_util_result(sce_app_util_error_busy);
                }
                if (state.registers[0] == 0 || state.registers[1] == 0) {
                    return set_app_util_result(sce_app_util_error_parameter);
                }
                std::array<std::uint8_t, sce_app_util_init_param_size> init_param{};
                std::array<std::uint8_t, sce_app_util_boot_param_size> boot_param{};
                std::string error;
                if (!memory.read(state.registers[0], init_param, error)) {
                    return fail_app_util_memory(state, "sceAppUtilInit initParam",
                        std::move(error));
                }
                if (!memory.read(state.registers[1], boot_param, error)) {
                    return fail_app_util_memory(state, "sceAppUtilInit bootParam",
                        std::move(error));
                }
                std::memcpy(&result.last_app_util_work_buffer_size,
                    init_param.data(), sizeof(result.last_app_util_work_buffer_size));
                std::memcpy(&result.last_app_util_boot_attribute,
                    boot_param.data(), sizeof(result.last_app_util_boot_attribute));
                std::memcpy(&result.last_app_util_app_version,
                    boot_param.data() + sizeof(std::uint32_t),
                    sizeof(result.last_app_util_app_version));
                const bool init_reserved_nonzero = std::ranges::any_of(
                    std::span(init_param).subspan(sizeof(std::uint32_t)),
                    [](std::uint8_t value) { return value != 0; });
                const bool boot_reserved_nonzero = std::ranges::any_of(
                    std::span(boot_param).subspan(2 * sizeof(std::uint32_t)),
                    [](std::uint8_t value) { return value != 0; });
                if (init_reserved_nonzero || boot_reserved_nonzero) {
                    return set_app_util_result(sce_app_util_error_parameter);
                }
                app_util_initialized = true;
                result.app_util_initialized = true;
                return set_app_util_result(0);
            });
    }
    bool app_util_shutdown_bound = true;
    if (imports_sce_app_util_shutdown) {
        app_util_shutdown_bound = dispatcher.bind(nid_sce_app_util_shutdown,
            "sceAppUtilShutdown", [&](ArmCpuState &) -> std::int32_t {
                ++result.hle_dispatch_count;
                ++result.app_util_shutdown_call_count;
                if (!app_util_initialized) {
                    return set_app_util_result(sce_app_util_error_not_initialized);
                }
                app_util_initialized = false;
                result.app_util_initialized = false;
                return set_app_util_result(0);
            });
    }
    const auto set_sysmodule_result = [&](std::uint32_t module_id,
                                          std::uint32_t value) -> std::int32_t {
        result.last_sysmodule_id = module_id;
        result.last_sysmodule_result = static_cast<std::int32_t>(value);
        result.loaded_sysmodule_count = loaded_sysmodules.size();
        return result.last_sysmodule_result;
    };
    const auto sysmodule_id_valid = [](std::uint32_t module_id) {
        return module_id <= sce_sysmodule_count;
    };
    bool sysmodule_load_bound = true;
    if (imports_sce_sysmodule_load_module) {
        sysmodule_load_bound = dispatcher.bind(nid_sce_sysmodule_load_module,
            "sceSysmoduleLoadModule", [&](ArmCpuState &state) -> std::int32_t {
                ++result.hle_dispatch_count;
                ++result.sysmodule_load_call_count;
                const auto module_id = state.registers[0];
                if (!sysmodule_id_valid(module_id)) {
                    return set_sysmodule_result(
                        module_id, sce_sysmodule_error_invalid_value);
                }
                if (!std::ranges::contains(loaded_sysmodules, module_id)) {
                    loaded_sysmodules.push_back(module_id);
                }
                return set_sysmodule_result(module_id, 0);
            });
    }
    bool sysmodule_is_loaded_bound = true;
    if (imports_sce_sysmodule_is_loaded) {
        sysmodule_is_loaded_bound = dispatcher.bind(nid_sce_sysmodule_is_loaded,
            "sceSysmoduleIsLoaded", [&](ArmCpuState &state) -> std::int32_t {
                ++result.hle_dispatch_count;
                ++result.sysmodule_is_loaded_call_count;
                const auto module_id = state.registers[0];
                if (!sysmodule_id_valid(module_id)) {
                    return set_sysmodule_result(
                        module_id, sce_sysmodule_error_invalid_value);
                }
                return set_sysmodule_result(module_id,
                    std::ranges::contains(loaded_sysmodules, module_id)
                        ? 0u : sce_sysmodule_error_unloaded);
            });
    }
    bool sysmodule_unload_bound = true;
    if (imports_sce_sysmodule_unload_module) {
        sysmodule_unload_bound = dispatcher.bind(nid_sce_sysmodule_unload_module,
            "sceSysmoduleUnloadModule", [&](ArmCpuState &state) -> std::int32_t {
                ++result.hle_dispatch_count;
                ++result.sysmodule_unload_call_count;
                const auto module_id = state.registers[0];
                if (!sysmodule_id_valid(module_id)) {
                    return set_sysmodule_result(
                        module_id, sce_sysmodule_error_invalid_value);
                }
                std::erase(loaded_sysmodules, module_id);
                return set_sysmodule_result(module_id, 0);
            });
    }
    const auto set_np_result = [&](std::uint32_t value) -> std::int32_t {
        result.last_np_result = static_cast<std::int32_t>(value);
        result.np_initialized = np_initialized;
        result.np_trophy_initialized = np_trophy_initialized;
        result.np_trophy_context_count = np_trophy_contexts.size();
        result.np_trophy_handle_count = np_trophy_handles.size();
        return result.last_np_result;
    };
    const auto set_np_trophy_result = [&](std::uint32_t value) -> std::int32_t {
        result.last_np_trophy_result = static_cast<std::int32_t>(value);
        result.np_initialized = np_initialized;
        result.np_trophy_initialized = np_trophy_initialized;
        result.np_trophy_context_count = np_trophy_contexts.size();
        result.np_trophy_handle_count = np_trophy_handles.size();
        return result.last_np_trophy_result;
    };
    const auto fail_np_memory = [&](ArmCpuState &state, std::string operation,
                                    std::string error) -> std::int32_t {
        np_error = std::move(operation) + " guest-memory validation failed: "
            + std::move(error);
        state.stop_requested = true;
        state.stop_code = -1;
        return set_np_result(sce_np_error_invalid_argument);
    };
    const auto fail_np_trophy_memory = [&](ArmCpuState &state,
                                           std::string operation,
                                           std::string error) -> std::int32_t {
        np_error = std::move(operation) + " guest-memory validation failed: "
            + std::move(error);
        state.stop_requested = true;
        state.stop_code = -1;
        return set_np_trophy_result(sce_np_trophy_error_invalid_argument);
    };
    bool np_init_bound = true;
    if (imports_sce_np_init) {
        np_init_bound = dispatcher.bind(nid_sce_np_init,
            "sceNpInit", [&](ArmCpuState &state) -> std::int32_t {
                ++result.hle_dispatch_count;
                ++result.np_init_call_count;
                result.last_np_communication_config = state.registers[0];
                if (np_initialized) {
                    return set_np_result(sce_np_error_already_initialized);
                }
                if (state.registers[0] != 0) {
                    std::array<std::uint8_t, sce_np_communication_config_size> config{};
                    std::string error;
                    if (!memory.read(state.registers[0], config, error)) {
                        return fail_np_memory(state, "sceNpInit communication config",
                            std::move(error));
                    }
                    std::memcpy(&result.last_np_communication_id,
                        config.data(), sizeof(result.last_np_communication_id));
                    if (result.last_np_communication_id != 0) {
                        std::array<std::uint8_t, sce_np_communication_id_size> communication_id{};
                        if (!memory.read(result.last_np_communication_id,
                                communication_id, error)) {
                            return fail_np_memory(state, "sceNpInit communication ID",
                                std::move(error));
                        }
                    }
                }
                np_initialized = true;
                return set_np_result(0);
            });
    }
    bool np_get_service_state_bound = true;
    if (imports_sce_np_get_service_state) {
        np_get_service_state_bound = dispatcher.bind(nid_sce_np_get_service_state,
            "sceNpGetServiceState", [&](ArmCpuState &state) -> std::int32_t {
                ++result.hle_dispatch_count;
                ++result.np_service_state_call_count;
                result.last_np_service_state_address = state.registers[0];
                if (state.registers[0] == 0) {
                    return set_np_result(sce_np_error_invalid_argument);
                }
                std::string error;
                if (!write_guest_u32(memory, state.registers[0],
                        sce_np_service_state_signed_out, error)) {
                    return fail_np_memory(state, "sceNpGetServiceState output",
                        std::move(error));
                }
                result.last_np_service_state = sce_np_service_state_signed_out;
                return set_np_result(0);
            });
    }
    bool np_term_bound = true;
    if (imports_sce_np_term) {
        np_term_bound = dispatcher.bind(nid_sce_np_term,
            "sceNpTerm", [&](ArmCpuState &) -> std::int32_t {
                ++result.hle_dispatch_count;
                ++result.np_term_call_count;
                np_trophy_contexts.clear();
                np_trophy_handles.clear();
                np_trophy_initialized = false;
                np_initialized = false;
                return set_np_result(0);
            });
    }
    bool np_trophy_init_bound = true;
    if (imports_sce_np_trophy_init) {
        np_trophy_init_bound = dispatcher.bind(nid_sce_np_trophy_init,
            "sceNpTrophyInit", [&](ArmCpuState &) -> std::int32_t {
                ++result.hle_dispatch_count;
                ++result.np_trophy_init_call_count;
                if (np_trophy_initialized) {
                    return set_np_trophy_result(
                        sce_np_trophy_error_already_initialized);
                }
                np_trophy_initialized = true;
                return set_np_trophy_result(0);
            });
    }
    bool np_trophy_term_bound = true;
    if (imports_sce_np_trophy_term) {
        np_trophy_term_bound = dispatcher.bind(nid_sce_np_trophy_term,
            "sceNpTrophyTerm", [&](ArmCpuState &) -> std::int32_t {
                ++result.hle_dispatch_count;
                ++result.np_trophy_term_call_count;
                if (!np_trophy_initialized) {
                    return set_np_trophy_result(
                        sce_np_trophy_error_not_initialized);
                }
                np_trophy_contexts.clear();
                np_trophy_handles.clear();
                np_trophy_initialized = false;
                return set_np_trophy_result(0);
            });
    }
    bool np_trophy_create_context_bound = true;
    if (imports_sce_np_trophy_create_context) {
        np_trophy_create_context_bound = dispatcher.bind(
            nid_sce_np_trophy_create_context, "sceNpTrophyCreateContext",
            [&](ArmCpuState &state) -> std::int32_t {
                ++result.hle_dispatch_count;
                ++result.np_trophy_context_create_call_count;
                result.last_np_trophy_context_address = state.registers[0];
                result.last_np_trophy_communication_id_address = state.registers[1];
                result.last_np_trophy_communication_signature_address = state.registers[2];
                if (!np_trophy_initialized) {
                    return set_np_trophy_result(
                        sce_np_trophy_error_not_initialized);
                }
                if (state.registers[0] == 0) {
                    return set_np_trophy_result(
                        sce_np_trophy_error_invalid_argument);
                }
                if (state.registers[1] != 0) {
                    std::array<std::uint8_t, sce_np_communication_id_size>
                        communication_id{};
                    std::string error;
                    if (!memory.read(state.registers[1], communication_id, error)) {
                        return fail_np_trophy_memory(state,
                            "sceNpTrophyCreateContext communication ID",
                            std::move(error));
                    }
                    result.last_np_trophy_communication_number = communication_id[10];
                    if (communication_id[10] > 99) {
                        return set_np_trophy_result(
                            sce_np_trophy_error_invalid_np_comm_id);
                    }
                }
                if (!np_trophy_contexts.empty()) {
                    return set_np_trophy_result(
                        sce_np_trophy_error_context_already_exists);
                }
                constexpr std::int32_t context = 1;
                std::string error;
                if (!write_guest_u32(memory, state.registers[0],
                        static_cast<std::uint32_t>(context), error)) {
                    return fail_np_trophy_memory(state,
                        "sceNpTrophyCreateContext output", std::move(error));
                }
                np_trophy_contexts.push_back(context);
                result.last_np_trophy_context = context;
                return set_np_trophy_result(0);
            });
    }
    bool np_trophy_create_handle_bound = true;
    if (imports_sce_np_trophy_create_handle) {
        np_trophy_create_handle_bound = dispatcher.bind(
            nid_sce_np_trophy_create_handle, "sceNpTrophyCreateHandle",
            [&](ArmCpuState &state) -> std::int32_t {
                ++result.hle_dispatch_count;
                ++result.np_trophy_handle_create_call_count;
                if (state.registers[0] == 0) {
                    return set_np_trophy_result(
                        sce_np_trophy_error_invalid_argument);
                }
                constexpr std::int32_t handle = 1;
                std::string error;
                if (!write_guest_u32(memory, state.registers[0],
                        static_cast<std::uint32_t>(handle), error)) {
                    return fail_np_trophy_memory(state,
                        "sceNpTrophyCreateHandle output", std::move(error));
                }
                if (!std::ranges::contains(np_trophy_handles, handle)) {
                    np_trophy_handles.push_back(handle);
                }
                result.last_np_trophy_handle = handle;
                return set_np_trophy_result(0);
            });
    }
    bool np_trophy_destroy_context_bound = true;
    if (imports_sce_np_trophy_destroy_context) {
        np_trophy_destroy_context_bound = dispatcher.bind(
            nid_sce_np_trophy_destroy_context, "sceNpTrophyDestroyContext",
            [&](ArmCpuState &state) -> std::int32_t {
                ++result.hle_dispatch_count;
                ++result.np_trophy_context_destroy_call_count;
                const auto context = static_cast<std::int32_t>(state.registers[0]);
                result.last_np_trophy_context = context;
                if (!std::ranges::contains(np_trophy_contexts, context)) {
                    return set_np_trophy_result(
                        sce_np_trophy_error_invalid_context);
                }
                std::erase(np_trophy_contexts, context);
                return set_np_trophy_result(0);
            });
    }
    bool np_trophy_destroy_handle_bound = true;
    if (imports_sce_np_trophy_destroy_handle) {
        np_trophy_destroy_handle_bound = dispatcher.bind(
            nid_sce_np_trophy_destroy_handle, "sceNpTrophyDestroyHandle",
            [&](ArmCpuState &state) -> std::int32_t {
                ++result.hle_dispatch_count;
                ++result.np_trophy_handle_destroy_call_count;
                const auto handle = static_cast<std::int32_t>(state.registers[0]);
                result.last_np_trophy_handle = handle;
                std::erase(np_trophy_handles, handle);
                return set_np_trophy_result(0);
            });
    }
    bool np_trophy_abort_handle_bound = true;
    if (imports_sce_np_trophy_abort_handle) {
        np_trophy_abort_handle_bound = dispatcher.bind(
            nid_sce_np_trophy_abort_handle, "sceNpTrophyAbortHandle",
            [&](ArmCpuState &state) -> std::int32_t {
                ++result.hle_dispatch_count;
                ++result.np_trophy_handle_abort_call_count;
                result.last_np_trophy_handle =
                    static_cast<std::int32_t>(state.registers[0]);
                return set_np_trophy_result(0);
            });
    }
    const auto set_ctrl_result = [&](std::uint32_t value) -> std::int32_t {
        result.last_ctrl_result = static_cast<std::int32_t>(value);
        return result.last_ctrl_result;
    };
    const auto fail_ctrl_memory = [&](ArmCpuState &state, std::string operation,
                                      std::string error) -> std::int32_t {
        ctrl_error = std::move(operation) + " guest-memory validation failed: "
            + std::move(error);
        state.stop_requested = true;
        state.stop_code = -1;
        return set_ctrl_result(sce_ctrl_error_invalid_arg);
    };
    bool ctrl_bindings_bound = true;
    for (const auto &ctrl_import : ctrl_imports) {
        if (!contains_nid(imported_nids, ctrl_import.nid)) {
            continue;
        }
        ctrl_bindings_bound = ctrl_bindings_bound && dispatcher.bind(
            ctrl_import.nid, std::string(ctrl_import.name),
            [&, ctrl_import](ArmCpuState &state) -> std::int32_t {
                ++result.hle_dispatch_count;
                const auto nid = ctrl_import.nid;
                if (nid == nid_sce_ctrl_set_sampling_mode
                    || nid == nid_sce_ctrl_set_sampling_mode_ext) {
                    ++result.ctrl_sampling_mode_set_call_count;
                    const auto mode = state.registers[0];
                    if (mode > sce_ctrl_mode_analog_wide) {
                        return set_ctrl_result(sce_ctrl_error_invalid_arg);
                    }
                    auto &current_mode = nid == nid_sce_ctrl_set_sampling_mode
                        ? result.ctrl_sampling_mode : result.ctrl_sampling_mode_ext;
                    const auto previous_mode = current_mode;
                    current_mode = mode;
                    return set_ctrl_result(previous_mode);
                }
                if (nid == nid_sce_ctrl_get_sampling_mode
                    || nid == nid_sce_ctrl_get_sampling_mode_ext) {
                    ++result.ctrl_sampling_mode_get_call_count;
                    if (state.registers[0] == 0) {
                        return set_ctrl_result(sce_ctrl_error_invalid_arg);
                    }
                    const auto mode = nid == nid_sce_ctrl_get_sampling_mode
                        ? result.ctrl_sampling_mode : result.ctrl_sampling_mode_ext;
                    std::string error;
                    if (!write_guest_u32(memory, state.registers[0], mode, error)) {
                        return fail_ctrl_memory(state, std::string(ctrl_import.name),
                            std::move(error));
                    }
                    return set_ctrl_result(0);
                }
                if (nid == nid_sce_ctrl_get_controller_port_info
                    || nid == nid_sce_ctrl_get_wireless_controller_info) {
                    if (state.registers[0] == 0) {
                        return set_ctrl_result(sce_ctrl_error_invalid_arg);
                    }
                    std::array<std::uint8_t, 16> info{};
                    if (nid == nid_sce_ctrl_get_controller_port_info) {
                        info[0] = 1; // SCE_CTRL_TYPE_PHY on a handheld Vita.
                    }
                    std::string error;
                    if (!memory.write(state.registers[0], info, error)) {
                        return fail_ctrl_memory(state, std::string(ctrl_import.name),
                            std::move(error));
                    }
                    return set_ctrl_result(0);
                }
                if (nid == nid_sce_ctrl_is_multi_controller_supported) {
                    return set_ctrl_result(0); // Handheld Vita mode, not PSTV.
                }

                ++result.ctrl_buffer_call_count;
                auto port = state.registers[0];
                if (port == 0) {
                    port = 1;
                }
                result.last_ctrl_port = port;
                result.last_ctrl_buffer_address = state.registers[1];
                result.last_ctrl_requested_count = state.registers[2];
                if (port > 1) {
                    return set_ctrl_result(sce_ctrl_error_no_device);
                }
                const auto count = state.registers[2];
                if (state.registers[1] == 0 || count == 0
                    || count > sce_ctrl_max_samples) {
                    return set_ctrl_result(sce_ctrl_error_invalid_arg);
                }
                const bool peek = nid == nid_sce_ctrl_peek_buffer_negative
                    || nid == nid_sce_ctrl_peek_buffer_negative2
                    || nid == nid_sce_ctrl_peek_buffer_positive
                    || nid == nid_sce_ctrl_peek_buffer_positive2
                    || nid == nid_sce_ctrl_peek_buffer_positive_ext
                    || nid == nid_sce_ctrl_peek_buffer_positive_ext2;
                const bool negative = nid == nid_sce_ctrl_peek_buffer_negative
                    || nid == nid_sce_ctrl_peek_buffer_negative2
                    || nid == nid_sce_ctrl_read_buffer_negative
                    || nid == nid_sce_ctrl_read_buffer_negative2;
                const auto returned_count = peek ? count : 1u;
                std::vector<std::uint8_t> samples(
                    static_cast<std::size_t>(returned_count) * sce_ctrl_data_size);
                ctrl_timestamp += static_cast<std::uint64_t>(returned_count) * 16667u;
                for (std::uint32_t index = 0; index < returned_count; ++index) {
                    const auto offset = static_cast<std::size_t>(index)
                        * sce_ctrl_data_size;
                    const auto timestamp = ctrl_timestamp
                        - static_cast<std::uint64_t>(index) * 16667u;
                    std::memcpy(samples.data() + offset, &timestamp,
                        sizeof(timestamp));
                    const std::uint32_t buttons = negative ? 0xFFFFFFFFu : 0u;
                    std::memcpy(samples.data() + offset + 8, &buttons,
                        sizeof(buttons));
                    std::fill_n(samples.data() + offset + 12, 4,
                        static_cast<std::uint8_t>(0x80));
                }
                std::string error;
                if (!memory.write(state.registers[1], samples, error)) {
                    return fail_ctrl_memory(state, std::string(ctrl_import.name),
                        std::move(error));
                }
                result.ctrl_sample_count += returned_count;
                return set_ctrl_result(returned_count);
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
        + static_cast<std::size_t>(imports_new_nothrow)
        + static_cast<std::size_t>(imports_sce_app_util_init)
        + static_cast<std::size_t>(imports_sce_app_util_shutdown)
        + static_cast<std::size_t>(imports_sce_sysmodule_is_loaded)
        + static_cast<std::size_t>(imports_sce_sysmodule_load_module)
        + static_cast<std::size_t>(imports_sce_sysmodule_unload_module)
        + static_cast<std::size_t>(imports_sce_np_get_service_state)
        + static_cast<std::size_t>(imports_sce_np_init)
        + static_cast<std::size_t>(imports_sce_np_term)
        + static_cast<std::size_t>(imports_sce_np_trophy_init)
        + static_cast<std::size_t>(imports_sce_np_trophy_term)
        + static_cast<std::size_t>(imports_sce_np_trophy_abort_handle)
        + static_cast<std::size_t>(imports_sce_np_trophy_create_context)
        + static_cast<std::size_t>(imports_sce_np_trophy_create_handle)
        + static_cast<std::size_t>(imports_sce_np_trophy_destroy_context)
        + static_cast<std::size_t>(imports_sce_np_trophy_destroy_handle)
        + imported_ctrl_count;
    if (!get_id_bound || !exit_bound || !dso_handle_bound || !aeabi_atexit_bound
        || !cxa_atexit_bound || !cxa_finalize_bound || !cxa_guard_abort_bound
        || !cxa_guard_acquire_bound || !cxa_guard_release_bound
        || !calloc_bound || !free_bound || !malloc_bound || !malloc_usable_size_bound
        || !memalign_bound || !realloc_bound
        || !new_bound || !new_nothrow_bound || !new_array_bound || !new_array_nothrow_bound
        || !delete_bound || !delete_nothrow_bound || !delete_placement_bound
        || !delete_array_bound || !delete_array_nothrow_bound
        || !delete_array_placement_bound
        || !app_util_init_bound || !app_util_shutdown_bound
        || !sysmodule_is_loaded_bound || !sysmodule_load_bound || !sysmodule_unload_bound
        || !np_get_service_state_bound || !np_init_bound || !np_term_bound
        || !np_trophy_init_bound || !np_trophy_term_bound
        || !np_trophy_abort_handle_bound || !np_trophy_create_context_bound
        || !np_trophy_create_handle_bound || !np_trophy_destroy_context_bound
        || !np_trophy_destroy_handle_bound
        || !ctrl_bindings_bound
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
    if (result.app_util_init_call_count != 0 || result.app_util_shutdown_call_count != 0) {
        detail << " AppUtil lifecycle: initialized="
               << (result.app_util_initialized ? "yes" : "no")
               << ", init calls=" << result.app_util_init_call_count
               << ", shutdown calls=" << result.app_util_shutdown_call_count
               << "; initParam=" << nid_hex(result.last_app_util_init_param)
               << ", workBufSize=" << result.last_app_util_work_buffer_size
               << ", bootParam=" << nid_hex(result.last_app_util_boot_param)
               << ", attr=" << nid_hex(result.last_app_util_boot_attribute)
               << ", appVersion=" << nid_hex(result.last_app_util_app_version)
               << ", last result=" << nid_hex(
                    static_cast<std::uint32_t>(result.last_app_util_result)) << ".";
    }
    if (!app_util_error.empty()) {
        detail << " AppUtil boundary: " << app_util_error << ".";
    }
    const auto sysmodule_call_count = result.sysmodule_load_call_count
        + result.sysmodule_is_loaded_call_count + result.sysmodule_unload_call_count;
    if (sysmodule_call_count != 0) {
        detail << " Sysmodule lifecycle: loaded=" << result.loaded_sysmodule_count
               << ", load calls=" << result.sysmodule_load_call_count
               << ", status calls=" << result.sysmodule_is_loaded_call_count
               << ", unload calls=" << result.sysmodule_unload_call_count
               << "; last module=" << nid_hex(result.last_sysmodule_id)
               << ", last result=" << nid_hex(
                    static_cast<std::uint32_t>(result.last_sysmodule_result)) << ".";
    }
    const auto np_call_count = result.np_init_call_count + result.np_term_call_count
        + result.np_service_state_call_count + result.np_trophy_init_call_count
        + result.np_trophy_term_call_count
        + result.np_trophy_context_create_call_count
        + result.np_trophy_context_destroy_call_count
        + result.np_trophy_handle_create_call_count
        + result.np_trophy_handle_destroy_call_count
        + result.np_trophy_handle_abort_call_count;
    if (np_call_count != 0) {
        detail << " NP lifecycle: initialized=" << (result.np_initialized ? "yes" : "no")
               << ", init calls=" << result.np_init_call_count
               << ", service-state calls=" << result.np_service_state_call_count
               << ", term calls=" << result.np_term_call_count
               << "; commConfig=" << nid_hex(result.last_np_communication_config)
               << ", commId=" << nid_hex(result.last_np_communication_id)
               << ", serviceState=" << result.last_np_service_state
               << " at " << nid_hex(result.last_np_service_state_address)
               << ", last result=" << nid_hex(
                    static_cast<std::uint32_t>(result.last_np_result))
               << ". Trophy lifecycle: initialized="
               << (result.np_trophy_initialized ? "yes" : "no")
               << ", init calls=" << result.np_trophy_init_call_count
               << ", term calls=" << result.np_trophy_term_call_count
               << ", last result=" << nid_hex(
                    static_cast<std::uint32_t>(result.last_np_trophy_result))
               << ". Trophy objects: contexts=" << result.np_trophy_context_count
               << " (create=" << result.np_trophy_context_create_call_count
               << ", destroy=" << result.np_trophy_context_destroy_call_count
               << "), handles=" << result.np_trophy_handle_count
               << " (create=" << result.np_trophy_handle_create_call_count
               << ", destroy=" << result.np_trophy_handle_destroy_call_count
               << ", abort=" << result.np_trophy_handle_abort_call_count
               << "); last context=" << result.last_np_trophy_context
               << " at " << nid_hex(result.last_np_trophy_context_address)
               << ", commId="
               << nid_hex(result.last_np_trophy_communication_id_address)
               << " #" << result.last_np_trophy_communication_number
               << ", commSign="
               << nid_hex(result.last_np_trophy_communication_signature_address)
               << ", last handle=" << result.last_np_trophy_handle << ".";
    }
    if (!np_error.empty()) {
        detail << " NP boundary: " << np_error << ".";
    }
    const auto ctrl_call_count = result.ctrl_sampling_mode_set_call_count
        + result.ctrl_sampling_mode_get_call_count + result.ctrl_buffer_call_count;
    if (ctrl_call_count != 0) {
        detail << " Controller HLE: sampling mode=" << result.ctrl_sampling_mode
               << ", ext=" << result.ctrl_sampling_mode_ext
               << " (set=" << result.ctrl_sampling_mode_set_call_count
               << ", get=" << result.ctrl_sampling_mode_get_call_count
               << "); buffers=" << result.ctrl_buffer_call_count
               << ", samples=" << result.ctrl_sample_count
               << "; last port=" << result.last_ctrl_port
               << ", buffer=" << nid_hex(result.last_ctrl_buffer_address)
               << ", count=" << result.last_ctrl_requested_count
               << ", result=" << nid_hex(
                    static_cast<std::uint32_t>(result.last_ctrl_result)) << ".";
    }
    if (!ctrl_error.empty()) {
        detail << " Controller boundary: " << ctrl_error << ".";
    }
    result.detail = detail.str();
    return result;
}

} // namespace vita3k::ios
