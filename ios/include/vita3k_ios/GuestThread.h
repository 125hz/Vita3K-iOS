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
    std::int32_t thread_id = 0;
    std::int32_t exit_status = 0;
    std::uint32_t observed_thread_id = 0;
    std::uint64_t instruction_count = 0;
    std::size_t hle_dispatch_count = 0;
    std::string detail;
};

GuestThreadRunResult run_guest_module_start(GuestMemory &memory,
    std::uint32_t entry_point,
    std::uint32_t stack_pointer,
    std::span<const std::uint32_t> imported_nids,
    std::string thread_name,
    std::size_t instruction_limit = 256);

} // namespace vita3k::ios
