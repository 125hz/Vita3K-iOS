#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace vita3k::ios {

class GuestMemory;

struct ArmCpuState {
    std::array<std::uint32_t, 16> registers{};
    std::uint32_t cpsr = 0;
    bool thumb = false;
    bool stop_requested = false;
    std::int32_t stop_code = 0;
    std::uint64_t instruction_count = 0;
};

using HLEHandler = std::function<std::int32_t(ArmCpuState &)>;

struct HLEDispatchResult {
    bool handled = false;
    std::int32_t return_value = 0;
    std::string name;
};

class HLEDispatcher {
public:
    bool bind(std::uint32_t nid, std::string name, HLEHandler handler);
    [[nodiscard]] bool has_binding(std::uint32_t nid) const;
    [[nodiscard]] HLEDispatchResult dispatch(std::uint32_t nid, ArmCpuState &state) const;
    [[nodiscard]] std::size_t binding_count() const { return bindings_.size(); }

private:
    struct Binding {
        std::uint32_t nid;
        std::string name;
        HLEHandler handler;
    };

    std::vector<Binding> bindings_;
};

enum class ArmStopReason {
    halted,
    instruction_limit,
    memory_fault,
    unsupported_instruction,
    unbound_hle,
    unsupported_thumb
};

struct ArmExecutionResult {
    ArmStopReason reason = ArmStopReason::instruction_limit;
    std::uint64_t instructions_executed = 0;
    std::uint32_t final_pc = 0;
    std::uint32_t last_instruction = 0;
    std::uint32_t last_hle_nid = 0;
    std::size_t unique_pc_count = 0;
    std::uint32_t hottest_pc = 0;
    std::uint64_t hottest_pc_hits = 0;
    std::uint64_t non_forward_pc_count = 0;
    std::string detail;

    [[nodiscard]] bool halted() const { return reason == ArmStopReason::halted; }
};

class ArmInterpreter {
public:
    ArmInterpreter(GuestMemory &memory, const HLEDispatcher &hle_dispatcher);

    void reset(std::uint32_t entry_point, std::uint32_t stack_pointer,
        std::uint32_t link_register = 0);
    [[nodiscard]] ArmExecutionResult run(std::size_t instruction_limit);
    [[nodiscard]] const ArmCpuState &state() const { return state_; }
    [[nodiscard]] ArmCpuState &state() { return state_; }

private:
    [[nodiscard]] ArmExecutionResult step();
    [[nodiscard]] ArmExecutionResult step_thumb();

    GuestMemory &memory_;
    const HLEDispatcher &hle_dispatcher_;
    ArmCpuState state_;
};

} // namespace vita3k::ios
