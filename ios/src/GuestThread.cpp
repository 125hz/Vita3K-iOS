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
constexpr std::uint32_t nid_sce_touch_set_sampling_state = 0x1B9C5D14;
constexpr std::uint32_t nid_sce_touch_get_sampling_state = 0x26531526;
constexpr std::uint32_t nid_sce_touch_get_panel_info = 0x10A2CA25;
constexpr std::uint32_t nid_sce_touch_get_pixel_density = 0xF0704CF3;
constexpr std::uint32_t nid_sce_touch_peek = 0xFF082DF0;
constexpr std::uint32_t nid_sce_touch_peek2 = 0x3AD3D0A1;
constexpr std::uint32_t nid_sce_touch_peek_region = 0x04440622;
constexpr std::uint32_t nid_sce_touch_read = 0x169A1D58;
constexpr std::uint32_t nid_sce_touch_read2 = 0x39401BEA;
constexpr std::uint32_t nid_sce_touch_enable_touch_force = 0xB18370C2;
constexpr std::uint32_t nid_sce_touch_disable_touch_force = 0x41194411;
constexpr std::uint32_t nid_sce_kernel_get_process_time = 0x4C4672BF;
constexpr std::uint32_t nid_sce_kernel_get_process_time_low = 0xE9F973B1;
constexpr std::uint32_t nid_sce_kernel_get_process_time_wide = 0xB110C123;
constexpr std::uint32_t nid_sce_kernel_get_system_time_wide = 0xF4EE4FA9;
constexpr std::uint32_t nid_sce_rtc_get_current_tick = 0x23F79274;
constexpr std::uint32_t nid_sce_rtc_get_current_tick_internal = 0x247EE33B;
constexpr std::uint32_t nid_sce_power_set_arm_clock = 0x74DB5AE5;
constexpr std::uint32_t nid_sce_power_set_bus_clock = 0xB8D7B3FB;
constexpr std::uint32_t nid_sce_power_set_gpu_clock = 0x717DB06C;
constexpr std::uint32_t nid_sce_power_set_gpu_xbar_clock = 0xA7739DBE;
constexpr std::uint32_t nid_sce_power_get_arm_clock = 0xABC6F88F;
constexpr std::uint32_t nid_sce_power_get_bus_clock = 0x478FE6F5;
constexpr std::uint32_t nid_sce_power_get_gpu_clock = 0x1B04A1D6;
constexpr std::uint32_t nid_sce_power_get_gpu_xbar_clock = 0x0A750DEE;
constexpr std::uint32_t nid_sce_power_get_battery_life_percent = 0x2085D15D;
constexpr std::uint32_t nid_sce_power_get_battery_life_time = 0x8EFB3FA2;
constexpr std::uint32_t nid_sce_power_is_battery_charging = 0x1E490401;
constexpr std::uint32_t nid_sce_power_is_battery_exist = 0x0AFD0D8B;
constexpr std::uint32_t nid_sce_power_is_low_battery = 0xD3075926;
constexpr std::uint32_t nid_sce_power_is_power_online = 0x87440F5E;
constexpr std::uint32_t nid_sce_display_get_refresh_rate = 0xA08CA60D;
constexpr std::uint32_t nid_sce_display_get_vcount = 0xB6FDE0BA;
constexpr std::uint32_t nid_sce_display_wait_vblank_start = 0x5795E898;
constexpr std::uint32_t nid_sce_display_wait_vblank_start_cb = 0x78B41B92;
constexpr std::uint32_t nid_sce_display_wait_vblank_start_multi = 0xDD0A13B8;
constexpr std::uint32_t nid_sce_display_wait_vblank_start_multi_cb = 0x05F27764;
constexpr std::uint32_t nid_sce_display_set_frame_buf_internal = 0xF51523CB;
constexpr std::uint32_t nid_sce_display_set_frame_buf = 0x7A410B64;
constexpr std::uint32_t nid_sce_display_get_frame_buf_internal = 0xA753B0CA;
constexpr std::uint32_t nid_sce_display_get_frame_buf = 0x42AE6BBC;
// Upstream vita3k/touch/include/touch/touch.h definitions.
constexpr std::uint32_t sce_touch_error_invalid_arg = 0x80350001;
constexpr std::uint32_t sce_touch_port_max = 2;
constexpr std::uint32_t sce_touch_sampling_state_start = 1;
constexpr std::uint32_t sce_touch_max_buffers = 64;
constexpr std::size_t sce_touch_data_size = 0x90;
constexpr std::size_t sce_touch_panel_info_size = 0x30;
// Upstream vita3k/rtc/include/rtc/rtc.h RTC_OFFSET: microseconds between
// 0001-01-01 and 1970-01-01. The diagnostic runner uses a deterministic
// virtual clock anchored at that epoch instead of host wall time.
constexpr std::uint64_t sce_rtc_epoch_offset_us = 62135596800000000ULL;
constexpr std::uint32_t sce_rtc_error_invalid_pointer = 0x80251001;
// Upstream vita3k/modules/ScePower/ScePower.cpp constants and stubbed
// deterministic handheld defaults (444/222/222/166 MHz, full offline battery).
constexpr std::uint32_t sce_power_error_invalid_value = 0x802B0000;
constexpr std::int32_t sce_power_arm_clock_mhz = 444;
constexpr std::int32_t sce_power_bus_clock_mhz = 222;
constexpr std::int32_t sce_power_gpu_clock_mhz = 222;
constexpr std::int32_t sce_power_gpu_xbar_clock_mhz = 166;
// Upstream vita3k/modules/SceDisplay/SceDisplay.h definitions.
constexpr std::uint32_t sce_display_error_invalid_value = 0x80290001;
constexpr std::uint32_t sce_display_error_invalid_addr = 0x80290002;
constexpr std::uint32_t sce_display_error_invalid_pixelformat = 0x80290003;
constexpr std::uint32_t sce_display_error_invalid_pitch = 0x80290004;
constexpr std::uint32_t sce_display_error_invalid_resolution = 0x80290005;
constexpr std::uint32_t sce_display_error_invalid_updatetiming = 0x80290006;
constexpr std::uint32_t sce_display_pixelformat_a8b8g8r8 = 0;
constexpr std::size_t sce_display_frame_buf_size = 0x18;
constexpr std::size_t sce_display_frame_buf2_size = 0x1C;
constexpr std::uint64_t sce_display_vblank_period_us = 16667;
constexpr float sce_display_refresh_rate = 59.94005f;
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

constexpr std::array<CtrlImport, 11> touch_imports{{
    { nid_sce_touch_set_sampling_state, "sceTouchSetSamplingState" },
    { nid_sce_touch_get_sampling_state, "sceTouchGetSamplingState" },
    { nid_sce_touch_get_panel_info, "sceTouchGetPanelInfo" },
    { nid_sce_touch_get_pixel_density, "sceTouchGetPixelDensity" },
    { nid_sce_touch_peek, "sceTouchPeek" },
    { nid_sce_touch_peek2, "sceTouchPeek2" },
    { nid_sce_touch_peek_region, "sceTouchPeekRegion" },
    { nid_sce_touch_read, "sceTouchRead" },
    { nid_sce_touch_read2, "sceTouchRead2" },
    { nid_sce_touch_enable_touch_force, "sceTouchEnableTouchForce" },
    { nid_sce_touch_disable_touch_force, "sceTouchDisableTouchForce" },
}};

constexpr std::array<CtrlImport, 6> time_imports{{
    { nid_sce_kernel_get_process_time, "sceKernelGetProcessTime" },
    { nid_sce_kernel_get_process_time_low, "sceKernelGetProcessTimeLow" },
    { nid_sce_kernel_get_process_time_wide, "sceKernelGetProcessTimeWide" },
    { nid_sce_kernel_get_system_time_wide, "sceKernelGetSystemTimeWide" },
    { nid_sce_rtc_get_current_tick, "sceRtcGetCurrentTick" },
    { nid_sce_rtc_get_current_tick_internal, "_sceRtcGetCurrentTick" },
}};

constexpr std::array<CtrlImport, 14> power_imports{{
    { nid_sce_power_set_arm_clock, "scePowerSetArmClockFrequency" },
    { nid_sce_power_set_bus_clock, "scePowerSetBusClockFrequency" },
    { nid_sce_power_set_gpu_clock, "scePowerSetGpuClockFrequency" },
    { nid_sce_power_set_gpu_xbar_clock, "scePowerSetGpuXbarClockFrequency" },
    { nid_sce_power_get_arm_clock, "scePowerGetArmClockFrequency" },
    { nid_sce_power_get_bus_clock, "scePowerGetBusClockFrequency" },
    { nid_sce_power_get_gpu_clock, "scePowerGetGpuClockFrequency" },
    { nid_sce_power_get_gpu_xbar_clock, "scePowerGetGpuXbarClockFrequency" },
    { nid_sce_power_get_battery_life_percent, "scePowerGetBatteryLifePercent" },
    { nid_sce_power_get_battery_life_time, "scePowerGetBatteryLifeTime" },
    { nid_sce_power_is_battery_charging, "scePowerIsBatteryCharging" },
    { nid_sce_power_is_battery_exist, "scePowerIsBatteryExist" },
    { nid_sce_power_is_low_battery, "scePowerIsLowBattery" },
    { nid_sce_power_is_power_online, "scePowerIsPowerOnline" },
}};

constexpr std::array<CtrlImport, 10> display_imports{{
    { nid_sce_display_get_refresh_rate, "sceDisplayGetRefreshRate" },
    { nid_sce_display_get_vcount, "sceDisplayGetVcount" },
    { nid_sce_display_wait_vblank_start, "sceDisplayWaitVblankStart" },
    { nid_sce_display_wait_vblank_start_cb, "sceDisplayWaitVblankStartCB" },
    { nid_sce_display_wait_vblank_start_multi, "sceDisplayWaitVblankStartMulti" },
    { nid_sce_display_wait_vblank_start_multi_cb, "sceDisplayWaitVblankStartMultiCB" },
    { nid_sce_display_set_frame_buf_internal, "_sceDisplaySetFrameBuf" },
    { nid_sce_display_set_frame_buf, "sceDisplaySetFrameBuf" },
    { nid_sce_display_get_frame_buf_internal, "_sceDisplayGetFrameBuf" },
    { nid_sce_display_get_frame_buf, "sceDisplayGetFrameBuf" },
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

bool write_guest_u64(GuestMemory &memory, std::uint32_t address,
    std::uint64_t value, std::string &error) {
    std::array<std::uint8_t, sizeof(value)> bytes{};
    std::memcpy(bytes.data(), &value, sizeof(value));
    return memory.write(address, bytes, error);
}

bool write_guest_f32(GuestMemory &memory, std::uint32_t address,
    float value, std::string &error) {
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
    const auto count_verified_family = [&](std::span<const CtrlImport> family,
                                           std::string_view family_name,
                                           std::size_t &imported_count) {
        imported_count = 0;
        for (const auto &entry : family) {
            if (!contains_nid(imported_nids, entry.nid)) {
                continue;
            }
            ++imported_count;
            if (std::string_view(import_name(entry.nid)) != entry.name) {
                result.detail = "The upstream Vita NID database did not match the "
                    + std::string(family_name) + " bindings.";
                return false;
            }
        }
        return true;
    };
    std::size_t imported_touch_count = 0;
    std::size_t imported_time_count = 0;
    std::size_t imported_power_count = 0;
    std::size_t imported_display_count = 0;
    if (!count_verified_family(touch_imports, "touch", imported_touch_count)
        || !count_verified_family(time_imports, "time", imported_time_count)
        || !count_verified_family(power_imports, "power", imported_power_count)
        || !count_verified_family(display_imports, "display", imported_display_count)) {
        return result;
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
    std::string touch_error;
    std::string time_error;
    std::string display_error;
    std::array<std::uint32_t, sce_touch_port_max> touch_sampling_state{};
    std::array<bool, sce_touch_port_max> touch_force_enabled{};
    // One deterministic virtual microsecond clock shared by the time, RTC,
    // display-vblank, and touch-read families. It starts at zero and advances
    // only through guest-visible events, so every run is reproducible.
    std::uint64_t virtual_time_us = 0;
    std::uint64_t display_vblank_count = 0;
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
    const auto sample_virtual_time = [&]() -> std::uint64_t {
        const auto now = virtual_time_us;
        ++virtual_time_us;
        return now;
    };
    const auto advance_virtual_vblank = [&](std::uint64_t vblanks) {
        display_vblank_count += vblanks;
        virtual_time_us += vblanks * sce_display_vblank_period_us;
        result.display_vblank_count = display_vblank_count;
    };
    const auto set_touch_result = [&](std::uint32_t value) -> std::int32_t {
        result.last_touch_result = static_cast<std::int32_t>(value);
        result.touch_front_sampling_state = touch_sampling_state[0];
        result.touch_back_sampling_state = touch_sampling_state[1];
        return result.last_touch_result;
    };
    const auto fail_touch_memory = [&](ArmCpuState &state, std::string operation,
                                       std::string error) -> std::int32_t {
        touch_error = std::move(operation) + " guest-memory validation failed: "
            + std::move(error);
        state.stop_requested = true;
        state.stop_code = -1;
        return set_touch_result(sce_touch_error_invalid_arg);
    };
    bool touch_bindings_bound = true;
    for (const auto &touch_import : touch_imports) {
        if (!contains_nid(imported_nids, touch_import.nid)) {
            continue;
        }
        touch_bindings_bound = touch_bindings_bound && dispatcher.bind(
            touch_import.nid, std::string(touch_import.name),
            [&, touch_import](ArmCpuState &state) -> std::int32_t {
                ++result.hle_dispatch_count;
                const auto nid = touch_import.nid;
                if (nid == nid_sce_touch_set_sampling_state) {
                    ++result.touch_sampling_state_set_call_count;
                    const auto port = state.registers[0];
                    const auto sampling_state = state.registers[1];
                    result.last_touch_port = port;
                    if (port >= sce_touch_port_max) {
                        return set_touch_result(sce_touch_error_invalid_arg);
                    }
                    if (sampling_state > sce_touch_sampling_state_start) {
                        return set_touch_result(sce_touch_error_invalid_arg);
                    }
                    touch_sampling_state[port] = sampling_state;
                    return set_touch_result(0);
                }
                if (nid == nid_sce_touch_get_sampling_state) {
                    ++result.touch_sampling_state_get_call_count;
                    const auto port = state.registers[0];
                    result.last_touch_port = port;
                    if (port >= sce_touch_port_max || state.registers[1] == 0) {
                        return set_touch_result(sce_touch_error_invalid_arg);
                    }
                    std::string error;
                    if (!write_guest_u32(memory, state.registers[1],
                            touch_sampling_state[port], error)) {
                        return fail_touch_memory(state,
                            std::string(touch_import.name), std::move(error));
                    }
                    return set_touch_result(0);
                }
                if (nid == nid_sce_touch_get_panel_info) {
                    ++result.touch_panel_info_call_count;
                    const auto port = state.registers[0];
                    result.last_touch_port = port;
                    if (port >= sce_touch_port_max || state.registers[1] == 0) {
                        return set_touch_result(sce_touch_error_invalid_arg);
                    }
                    // Upstream vita3k SceTouch panel geometry: 1920x1088 active
                    // and display area, force 1..128, rear active Y 108..889.
                    std::array<std::uint8_t, sce_touch_panel_info_size> info{};
                    const auto write_i16 = [&info](std::size_t offset,
                                               std::int16_t value) {
                        std::memcpy(info.data() + offset, &value, sizeof(value));
                    };
                    write_i16(0, 0); // minAaX
                    write_i16(2, port == 1 ? 108 : 0); // minAaY
                    write_i16(4, 1919); // maxAaX
                    write_i16(6, port == 1 ? 889 : 1087); // maxAaY
                    write_i16(8, 0); // minDispX
                    write_i16(10, 0); // minDispY
                    write_i16(12, 1919); // maxDispX
                    write_i16(14, 1087); // maxDispY
                    info[16] = 1; // minForce
                    info[17] = 128; // maxForce
                    std::string error;
                    if (!memory.write(state.registers[1], info, error)) {
                        return fail_touch_memory(state,
                            std::string(touch_import.name), std::move(error));
                    }
                    return set_touch_result(0);
                }
                if (nid == nid_sce_touch_get_pixel_density) {
                    ++result.touch_pixel_density_call_count;
                    if (state.registers[0] == 0 || state.registers[1] == 0) {
                        return set_touch_result(sce_touch_error_invalid_arg);
                    }
                    // Upstream hardcodes the 0x30 hardware profile: 22.0 DPcm.
                    std::string error;
                    if (!write_guest_f32(memory, state.registers[0], 22.0f, error)
                        || !write_guest_f32(memory, state.registers[1], 22.0f, error)) {
                        return fail_touch_memory(state,
                            std::string(touch_import.name), std::move(error));
                    }
                    return set_touch_result(0);
                }
                if (nid == nid_sce_touch_enable_touch_force
                    || nid == nid_sce_touch_disable_touch_force) {
                    ++result.touch_force_mode_call_count;
                    const auto port = state.registers[0];
                    result.last_touch_port = port;
                    if (port >= sce_touch_port_max) {
                        return set_touch_result(sce_touch_error_invalid_arg);
                    }
                    touch_force_enabled[port] =
                        nid == nid_sce_touch_enable_touch_force;
                    return set_touch_result(0);
                }

                // Peek/read family with upstream validation and neutral
                // (no-finger) samples until a host touch bridge is connected.
                ++result.touch_buffer_call_count;
                const bool peek = nid == nid_sce_touch_peek
                    || nid == nid_sce_touch_peek2
                    || nid == nid_sce_touch_peek_region;
                if (nid == nid_sce_touch_peek_region
                    && state.registers[3] > 0xFF) {
                    return set_touch_result(sce_touch_error_invalid_arg);
                }
                const auto port = state.registers[0];
                const auto buffer_address = state.registers[1];
                const auto requested_count = state.registers[2];
                result.last_touch_port = port;
                result.last_touch_buffer_address = buffer_address;
                result.last_touch_requested_count = requested_count;
                if (port >= sce_touch_port_max || buffer_address == 0
                    || requested_count > sce_touch_max_buffers) {
                    return set_touch_result(sce_touch_error_invalid_arg);
                }
                if (requested_count == 0) {
                    return set_touch_result(0);
                }
                std::uint32_t returned_count = 0;
                if (peek) {
                    // Upstream peek repeats the newest buffer `count` times
                    // while sampling runs and returns zero buffers otherwise.
                    returned_count = touch_sampling_state[port]
                            == sce_touch_sampling_state_start
                        ? requested_count : 0u;
                } else {
                    // Upstream read blocks until the next vblank and then
                    // returns the buffers produced since the previous read.
                    // The diagnostic runner advances exactly one vblank.
                    advance_virtual_vblank(1);
                    returned_count = 1;
                }
                // Upstream touch_get zero-fills every requested buffer before
                // deciding how many carry data, so mirror that exactly.
                std::vector<std::uint8_t> buffers(
                    static_cast<std::size_t>(requested_count) * sce_touch_data_size);
                for (std::uint32_t index = 0; index < returned_count; ++index) {
                    std::memcpy(buffers.data()
                            + static_cast<std::size_t>(index) * sce_touch_data_size,
                        &virtual_time_us, sizeof(virtual_time_us));
                }
                std::string error;
                if (!memory.write(buffer_address, buffers, error)) {
                    return fail_touch_memory(state,
                        std::string(touch_import.name), std::move(error));
                }
                result.touch_sample_count += returned_count;
                return set_touch_result(returned_count);
            });
    }
    const auto set_time_result = [&](std::uint32_t value) -> std::int32_t {
        result.last_time_result = static_cast<std::int32_t>(value);
        return result.last_time_result;
    };
    const auto fail_time_memory = [&](ArmCpuState &state, std::string operation,
                                      std::string error) -> std::int32_t {
        time_error = std::move(operation) + " guest-memory validation failed: "
            + std::move(error);
        state.stop_requested = true;
        state.stop_code = -1;
        return set_time_result(sce_rtc_error_invalid_pointer);
    };
    bool time_bindings_bound = true;
    for (const auto &time_import : time_imports) {
        if (!contains_nid(imported_nids, time_import.nid)) {
            continue;
        }
        time_bindings_bound = time_bindings_bound && dispatcher.bind(
            time_import.nid, std::string(time_import.name),
            [&, time_import](ArmCpuState &state) -> std::int32_t {
                ++result.hle_dispatch_count;
                const auto nid = time_import.nid;
                if (nid == nid_sce_rtc_get_current_tick
                    || nid == nid_sce_rtc_get_current_tick_internal) {
                    ++result.rtc_tick_call_count;
                    result.last_rtc_tick_address = state.registers[0];
                    if (state.registers[0] == 0) {
                        return set_time_result(sce_rtc_error_invalid_pointer);
                    }
                    const auto tick = sce_rtc_epoch_offset_us + sample_virtual_time();
                    result.last_time_value = tick;
                    std::string error;
                    if (!write_guest_u64(memory, state.registers[0], tick, error)) {
                        return fail_time_memory(state,
                            std::string(time_import.name), std::move(error));
                    }
                    return set_time_result(0);
                }
                ++result.time_query_call_count;
                if (nid == nid_sce_kernel_get_process_time) {
                    const auto elapsed = sample_virtual_time();
                    result.last_time_value = elapsed;
                    // Upstream tolerates a null output pointer and still
                    // returns success without writing anything.
                    if (state.registers[0] != 0) {
                        std::string error;
                        if (!write_guest_u64(memory, state.registers[0],
                                elapsed, error)) {
                            return fail_time_memory(state,
                                std::string(time_import.name), std::move(error));
                        }
                    }
                    return set_time_result(0);
                }
                if (nid == nid_sce_kernel_get_process_time_low) {
                    const auto elapsed = sample_virtual_time();
                    result.last_time_value = elapsed;
                    result.last_time_result = 0;
                    return static_cast<std::int32_t>(
                        static_cast<std::uint32_t>(elapsed));
                }
                // sceKernelGetProcessTimeWide / sceKernelGetSystemTimeWide
                // return a 64-bit microsecond value in r0/r1 per the AAPCS.
                const auto elapsed = sample_virtual_time();
                result.last_time_value = elapsed;
                result.last_time_result = 0;
                state.registers[1] = static_cast<std::uint32_t>(elapsed >> 32);
                return static_cast<std::int32_t>(
                    static_cast<std::uint32_t>(elapsed));
            });
    }
    const auto set_power_result = [&](std::uint32_t value) -> std::int32_t {
        result.last_power_result = static_cast<std::int32_t>(value);
        return result.last_power_result;
    };
    bool power_bindings_bound = true;
    for (const auto &power_import : power_imports) {
        if (!contains_nid(imported_nids, power_import.nid)) {
            continue;
        }
        power_bindings_bound = power_bindings_bound && dispatcher.bind(
            power_import.nid, std::string(power_import.name),
            [&, power_import](ArmCpuState &state) -> std::int32_t {
                ++result.hle_dispatch_count;
                const auto nid = power_import.nid;
                if (nid == nid_sce_power_set_arm_clock
                    || nid == nid_sce_power_set_bus_clock
                    || nid == nid_sce_power_set_gpu_clock
                    || nid == nid_sce_power_set_gpu_xbar_clock) {
                    ++result.power_clock_set_call_count;
                    const auto frequency = static_cast<std::int32_t>(state.registers[0]);
                    if (nid == nid_sce_power_set_arm_clock) {
                        result.last_power_requested_arm_clock = frequency;
                    } else if (nid == nid_sce_power_set_bus_clock) {
                        result.last_power_requested_bus_clock = frequency;
                    } else if (nid == nid_sce_power_set_gpu_clock) {
                        result.last_power_requested_gpu_clock = frequency;
                    } else {
                        result.last_power_requested_gpu_xbar_clock = frequency;
                    }
                    if (frequency < 0) {
                        return set_power_result(sce_power_error_invalid_value);
                    }
                    // Upstream accepts the request without changing the
                    // reported fixed clock profile.
                    return set_power_result(0);
                }
                if (nid == nid_sce_power_get_arm_clock
                    || nid == nid_sce_power_get_bus_clock
                    || nid == nid_sce_power_get_gpu_clock
                    || nid == nid_sce_power_get_gpu_xbar_clock) {
                    ++result.power_clock_get_call_count;
                    result.last_power_result = 0;
                    if (nid == nid_sce_power_get_arm_clock) {
                        return sce_power_arm_clock_mhz;
                    }
                    if (nid == nid_sce_power_get_bus_clock) {
                        return sce_power_bus_clock_mhz;
                    }
                    if (nid == nid_sce_power_get_gpu_clock) {
                        return sce_power_gpu_clock_mhz;
                    }
                    return sce_power_gpu_xbar_clock_mhz;
                }
                // Deterministic offline handheld battery profile: a full,
                // present, non-charging battery with no external power.
                ++result.power_status_call_count;
                result.last_power_result = 0;
                if (nid == nid_sce_power_get_battery_life_percent) {
                    return 100;
                }
                if (nid == nid_sce_power_get_battery_life_time) {
                    return std::numeric_limits<std::int32_t>::max();
                }
                if (nid == nid_sce_power_is_battery_exist) {
                    return 1;
                }
                return 0;
            });
    }
    const auto set_display_result = [&](std::uint32_t value) -> std::int32_t {
        result.last_display_result = static_cast<std::int32_t>(value);
        return result.last_display_result;
    };
    const auto fail_display_memory = [&](ArmCpuState &state, std::string operation,
                                         std::string error) -> std::int32_t {
        display_error = std::move(operation) + " guest-memory validation failed: "
            + std::move(error);
        state.stop_requested = true;
        state.stop_code = -1;
        return set_display_result(sce_display_error_invalid_addr);
    };
    bool display_bindings_bound = true;
    for (const auto &display_import : display_imports) {
        if (!contains_nid(imported_nids, display_import.nid)) {
            continue;
        }
        display_bindings_bound = display_bindings_bound && dispatcher.bind(
            display_import.nid, std::string(display_import.name),
            [&, display_import](ArmCpuState &state) -> std::int32_t {
                ++result.hle_dispatch_count;
                const auto nid = display_import.nid;
                if (nid == nid_sce_display_get_refresh_rate) {
                    ++result.display_refresh_rate_call_count;
                    std::string error;
                    if (!write_guest_f32(memory, state.registers[0],
                            sce_display_refresh_rate, error)) {
                        return fail_display_memory(state,
                            std::string(display_import.name), std::move(error));
                    }
                    return set_display_result(0);
                }
                if (nid == nid_sce_display_get_vcount) {
                    ++result.display_get_vcount_call_count;
                    result.last_display_result = 0;
                    return static_cast<std::int32_t>(display_vblank_count & 0xFFFFu);
                }
                if (nid == nid_sce_display_wait_vblank_start
                    || nid == nid_sce_display_wait_vblank_start_cb
                    || nid == nid_sce_display_wait_vblank_start_multi
                    || nid == nid_sce_display_wait_vblank_start_multi_cb) {
                    ++result.display_wait_vblank_call_count;
                    const bool multi = nid == nid_sce_display_wait_vblank_start_multi
                        || nid == nid_sce_display_wait_vblank_start_multi_cb;
                    // No guest callbacks can exist yet: callback creation is
                    // still an unbound import, so the CB variants cannot skip
                    // any registered work here.
                    advance_virtual_vblank(multi ? state.registers[0] : 1u);
                    return set_display_result(0);
                }
                if (nid == nid_sce_display_set_frame_buf_internal
                    || nid == nid_sce_display_set_frame_buf) {
                    ++result.display_set_frame_buf_call_count;
                    const auto frame_buf_address = state.registers[0];
                    const auto sync = state.registers[1];
                    result.last_display_frame_buf_address = frame_buf_address;
                    result.last_display_frame_buf_sync = sync;
                    if (frame_buf_address == 0) {
                        // Upstream treats a null framebuffer as "leave the
                        // current buffer alone" and succeeds.
                        return set_display_result(0);
                    }
                    std::array<std::uint8_t, sce_display_frame_buf_size> raw{};
                    std::string error;
                    if (!memory.read(frame_buf_address, raw, error)) {
                        return fail_display_memory(state,
                            std::string(display_import.name), std::move(error));
                    }
                    std::uint32_t size = 0;
                    std::uint32_t base = 0;
                    std::uint32_t pitch = 0;
                    std::uint32_t pixelformat = 0;
                    std::uint32_t width = 0;
                    std::uint32_t height = 0;
                    std::memcpy(&size, raw.data(), sizeof(size));
                    std::memcpy(&base, raw.data() + 4, sizeof(base));
                    std::memcpy(&pitch, raw.data() + 8, sizeof(pitch));
                    std::memcpy(&pixelformat, raw.data() + 12, sizeof(pixelformat));
                    std::memcpy(&width, raw.data() + 16, sizeof(width));
                    std::memcpy(&height, raw.data() + 20, sizeof(height));
                    if (size != sce_display_frame_buf_size
                        && size != sce_display_frame_buf2_size) {
                        return set_display_result(sce_display_error_invalid_value);
                    }
                    if (base == 0) {
                        return set_display_result(sce_display_error_invalid_addr);
                    }
                    if (pitch < width) {
                        return set_display_result(sce_display_error_invalid_pitch);
                    }
                    if (pixelformat != sce_display_pixelformat_a8b8g8r8) {
                        return set_display_result(
                            sce_display_error_invalid_pixelformat);
                    }
                    if (sync > 1) {
                        return set_display_result(
                            sce_display_error_invalid_updatetiming);
                    }
                    if (width < 480 || height < 272 || pitch < 480) {
                        return set_display_result(
                            sce_display_error_invalid_resolution);
                    }
                    result.last_display_frame_buf_base = base;
                    result.last_display_frame_buf_pitch = pitch;
                    result.last_display_frame_buf_pixel_format = pixelformat;
                    result.last_display_frame_buf_width = width;
                    result.last_display_frame_buf_height = height;
                    result.display_frame_buf_set = true;
                    return set_display_result(0);
                }
                // _sceDisplayGetFrameBuf / sceDisplayGetFrameBuf
                ++result.display_get_frame_buf_call_count;
                const auto frame_buf_address = state.registers[0];
                const auto sync = state.registers[1];
                result.last_display_frame_buf_address = frame_buf_address;
                std::string error;
                std::uint32_t size = 0;
                std::array<std::uint8_t, sizeof(size)> size_raw{};
                if (!memory.read(frame_buf_address, size_raw, error)) {
                    return fail_display_memory(state,
                        std::string(display_import.name), std::move(error));
                }
                std::memcpy(&size, size_raw.data(), sizeof(size));
                if (size != sce_display_frame_buf_size
                    && size != sce_display_frame_buf2_size) {
                    return set_display_result(sce_display_error_invalid_value);
                }
                if (sync > 1) {
                    return set_display_result(
                        sce_display_error_invalid_updatetiming);
                }
                std::array<std::uint8_t, sce_display_frame_buf_size - 4> fields{};
                std::memcpy(fields.data(), &result.last_display_frame_buf_base, 4);
                std::memcpy(fields.data() + 4, &result.last_display_frame_buf_pitch, 4);
                std::memcpy(fields.data() + 8,
                    &result.last_display_frame_buf_pixel_format, 4);
                std::memcpy(fields.data() + 12, &result.last_display_frame_buf_width, 4);
                std::memcpy(fields.data() + 16, &result.last_display_frame_buf_height, 4);
                if (!memory.write(frame_buf_address + 4, fields, error)) {
                    return fail_display_memory(state,
                        std::string(display_import.name), std::move(error));
                }
                return set_display_result(0);
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
        + imported_ctrl_count
        + imported_touch_count
        + imported_time_count
        + imported_power_count
        + imported_display_count;
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
        || !touch_bindings_bound || !time_bindings_bound
        || !power_bindings_bound || !display_bindings_bound
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
    const auto touch_call_count = result.touch_sampling_state_set_call_count
        + result.touch_sampling_state_get_call_count
        + result.touch_panel_info_call_count
        + result.touch_pixel_density_call_count
        + result.touch_force_mode_call_count
        + result.touch_buffer_call_count;
    if (touch_call_count != 0) {
        detail << " Touch HLE: front=" << result.touch_front_sampling_state
               << ", back=" << result.touch_back_sampling_state
               << " (set=" << result.touch_sampling_state_set_call_count
               << ", get=" << result.touch_sampling_state_get_call_count
               << ", panel=" << result.touch_panel_info_call_count
               << ", density=" << result.touch_pixel_density_call_count
               << ", force=" << result.touch_force_mode_call_count
               << "); buffers=" << result.touch_buffer_call_count
               << ", samples=" << result.touch_sample_count
               << "; last port=" << result.last_touch_port
               << ", buffer=" << nid_hex(result.last_touch_buffer_address)
               << ", count=" << result.last_touch_requested_count
               << ", result=" << nid_hex(
                    static_cast<std::uint32_t>(result.last_touch_result)) << ".";
    }
    if (!touch_error.empty()) {
        detail << " Touch boundary: " << touch_error << ".";
    }
    if (result.time_query_call_count != 0 || result.rtc_tick_call_count != 0) {
        detail << " Time HLE: queries=" << result.time_query_call_count
               << ", RTC ticks=" << result.rtc_tick_call_count
               << "; last value=" << result.last_time_value
               << ", last tick address=" << nid_hex(result.last_rtc_tick_address)
               << ", last result=" << nid_hex(
                    static_cast<std::uint32_t>(result.last_time_result)) << ".";
    }
    if (!time_error.empty()) {
        detail << " Time boundary: " << time_error << ".";
    }
    const auto power_call_count = result.power_clock_set_call_count
        + result.power_clock_get_call_count + result.power_status_call_count;
    if (power_call_count != 0) {
        detail << " Power HLE: clock sets=" << result.power_clock_set_call_count
               << " (ARM=" << result.last_power_requested_arm_clock
               << ", bus=" << result.last_power_requested_bus_clock
               << ", GPU=" << result.last_power_requested_gpu_clock
               << ", xbar=" << result.last_power_requested_gpu_xbar_clock
               << "), clock gets=" << result.power_clock_get_call_count
               << ", status=" << result.power_status_call_count
               << ", last result=" << nid_hex(
                    static_cast<std::uint32_t>(result.last_power_result)) << ".";
    }
    const auto display_call_count = result.display_wait_vblank_call_count
        + result.display_get_vcount_call_count
        + result.display_refresh_rate_call_count
        + result.display_set_frame_buf_call_count
        + result.display_get_frame_buf_call_count;
    if (display_call_count != 0) {
        detail << " Display HLE: vblank waits=" << result.display_wait_vblank_call_count
               << ", vblanks=" << result.display_vblank_count
               << ", vcount gets=" << result.display_get_vcount_call_count
               << ", refresh gets=" << result.display_refresh_rate_call_count
               << ", framebuf sets=" << result.display_set_frame_buf_call_count
               << ", gets=" << result.display_get_frame_buf_call_count << ";";
        if (result.display_frame_buf_set) {
            detail << " framebuf base=" << nid_hex(result.last_display_frame_buf_base)
                   << ", pitch=" << result.last_display_frame_buf_pitch
                   << ", " << result.last_display_frame_buf_width
                   << "x" << result.last_display_frame_buf_height
                   << ", format=" << nid_hex(result.last_display_frame_buf_pixel_format)
                   << ", sync=" << result.last_display_frame_buf_sync << ",";
        }
        detail << " last result=" << nid_hex(
                    static_cast<std::uint32_t>(result.last_display_result)) << ".";
    }
    if (!display_error.empty()) {
        detail << " Display boundary: " << display_error << ".";
    }
    result.detail = detail.str();
    return result;
}

} // namespace vita3k::ios
