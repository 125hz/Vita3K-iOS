#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>

namespace vita3k::ios {

class GuestMemory;

struct GuestThreadRunResult {
    bool attempted = false;
    bool started = false;
    bool exited = false;
    bool returned = false;
    std::int32_t thread_id = 0;
    std::int32_t exit_status = 0;
    std::uint32_t observed_thread_id = 0;
    std::uint64_t instruction_count = 0;
    std::size_t hle_dispatch_count = 0;
    std::uint32_t last_hle_nid = 0;
    std::uint32_t last_guest_pc = 0;
    std::size_t unique_pc_count = 0;
    std::uint32_t hottest_pc = 0;
    std::uint64_t hottest_pc_hits = 0;
    std::uint64_t non_forward_pc_count = 0;
    std::uint32_t libc_dso_handle_main = 0;
    std::size_t libc_atexit_registration_count = 0;
    std::size_t libc_finalize_call_count = 0;
    std::size_t libc_guard_acquire_count = 0;
    std::size_t libc_guard_release_count = 0;
    std::size_t libc_guard_abort_count = 0;
    std::size_t libc_guard_initialization_count = 0;
    std::size_t libc_guard_recursive_acquire_count = 0;
    std::size_t libc_heap_allocation_count = 0;
    std::size_t libc_heap_free_count = 0;
    std::size_t libc_heap_realloc_count = 0;
    std::size_t libc_heap_failure_count = 0;
    std::size_t cxx_new_call_count = 0;
    std::size_t cxx_new_array_call_count = 0;
    std::size_t cxx_delete_call_count = 0;
    std::size_t cxx_delete_array_call_count = 0;
    std::size_t cxx_nothrow_failure_count = 0;
    std::size_t cxx_placement_delete_call_count = 0;
    std::uint64_t libc_heap_live_bytes = 0;
    std::uint64_t libc_heap_peak_bytes = 0;
    std::uint32_t last_libc_atexit_object = 0;
    std::uint32_t last_libc_atexit_destructor = 0;
    std::uint32_t last_libc_atexit_dso = 0;
    std::uint32_t last_libc_finalize_dso = 0;
    std::uint32_t last_libc_guard_address = 0;
    std::uint32_t last_libc_guard_word = 0;
    std::int32_t last_libc_guard_result = 0;
    std::uint32_t last_libc_heap_address = 0;
    std::uint32_t last_libc_heap_size = 0;
    std::uint32_t last_libc_heap_alignment = 0;
    std::uint32_t return_value = 0;
    std::string detail;
};

GuestThreadRunResult run_guest_module_start(GuestMemory &memory,
    std::uint32_t entry_point,
    std::uint32_t stack_pointer,
    std::span<const std::uint32_t> imported_nids,
    std::string thread_name,
    std::size_t instruction_limit = 65536);

} // namespace vita3k::ios
