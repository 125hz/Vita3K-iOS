#include <vita3k_ios/ArmExecution.h>

#include <vita3k_ios/GuestMemory.h>

#include <algorithm>
#include <array>
#include <bit>
#include <cstring>
#include <iomanip>
#include <sstream>
#include <utility>

namespace vita3k::ios {
namespace {

constexpr std::size_t register_sp = 13;
constexpr std::size_t register_lr = 14;
constexpr std::size_t register_pc = 15;
constexpr std::size_t register_hle_nid = 12;

std::string hexadecimal(std::uint32_t value) {
    std::ostringstream stream;
    stream << "0x" << std::hex << std::uppercase << std::setfill('0')
           << std::setw(8) << value;
    return stream.str();
}

std::uint32_t decode_arm_immediate(std::uint32_t instruction) {
    const auto immediate = instruction & 0xFFu;
    const auto rotation = ((instruction >> 8) & 0xFu) * 2;
    return std::rotr(immediate, static_cast<int>(rotation));
}

} // namespace

bool HLEDispatcher::bind(std::uint32_t nid, std::string name, HLEHandler handler) {
    if (name.empty() || !handler) {
        return false;
    }
    const auto existing = std::ranges::find(bindings_, nid, &Binding::nid);
    if (existing != bindings_.end()) {
        return false;
    }
    bindings_.push_back({
        .nid = nid,
        .name = std::move(name),
        .handler = std::move(handler)
    });
    return true;
}

HLEDispatchResult HLEDispatcher::dispatch(std::uint32_t nid, ArmCpuState &state) const {
    const auto binding = std::ranges::find(bindings_, nid, &Binding::nid);
    if (binding == bindings_.end()) {
        return {};
    }
    return {
        .handled = true,
        .return_value = binding->handler(state),
        .name = binding->name
    };
}

ArmInterpreter::ArmInterpreter(GuestMemory &memory, const HLEDispatcher &hle_dispatcher)
    : memory_(memory)
    , hle_dispatcher_(hle_dispatcher) {
}

void ArmInterpreter::reset(std::uint32_t entry_point, std::uint32_t stack_pointer,
    std::uint32_t link_register) {
    state_ = {};
    state_.registers[register_sp] = stack_pointer;
    state_.registers[register_lr] = link_register;
    state_.registers[register_pc] = entry_point & ~3u;
}

ArmExecutionResult ArmInterpreter::run(std::size_t instruction_limit) {
    if (instruction_limit == 0) {
        return {
            .reason = ArmStopReason::instruction_limit,
            .instructions_executed = state_.instruction_count,
            .final_pc = state_.registers[register_pc],
            .detail = "The ARM instruction budget was zero."
        };
    }

    for (std::size_t index = 0; index < instruction_limit; ++index) {
        auto result = step();
        if (result.reason != ArmStopReason::instruction_limit) {
            return result;
        }
    }
    return {
        .reason = ArmStopReason::instruction_limit,
        .instructions_executed = state_.instruction_count,
        .final_pc = state_.registers[register_pc],
        .detail = "The ARM instruction budget was exhausted."
    };
}

ArmExecutionResult ArmInterpreter::step() {
    const auto pc = state_.registers[register_pc];
    if (state_.thumb) {
        return {
            .reason = ArmStopReason::unsupported_thumb,
            .instructions_executed = state_.instruction_count,
            .final_pc = pc,
            .detail = "Thumb execution is not implemented in the Milestone 6 interpreter."
        };
    }

    std::array<std::uint8_t, sizeof(std::uint32_t)> bytes{};
    std::string memory_error;
    if (!memory_.read(pc, bytes, memory_error)) {
        return {
            .reason = ArmStopReason::memory_fault,
            .instructions_executed = state_.instruction_count,
            .final_pc = pc,
            .detail = "ARM instruction fetch failed at " + hexadecimal(pc) + ": " + memory_error
        };
    }

    std::uint32_t instruction = 0;
    std::memcpy(&instruction, bytes.data(), sizeof(instruction));
    const auto condition = instruction >> 28;
    if (condition != 0xEu) {
        return {
            .reason = ArmStopReason::unsupported_instruction,
            .instructions_executed = state_.instruction_count,
            .final_pc = pc,
            .last_instruction = instruction,
            .detail = "Only unconditional AL instructions are implemented; decoded " +
                hexadecimal(instruction) + " at " + hexadecimal(pc) + "."
        };
    }

    state_.registers[register_pc] = pc + 4;
    ++state_.instruction_count;

    // MOVW Rd, #imm16
    if ((instruction & 0x0FF00000u) == 0x03000000u) {
        const auto destination = (instruction >> 12) & 0xFu;
        const auto immediate = ((instruction >> 4) & 0xF000u) | (instruction & 0xFFFu);
        state_.registers[destination] = immediate;
        return {
            .reason = ArmStopReason::instruction_limit,
            .instructions_executed = state_.instruction_count,
            .final_pc = state_.registers[register_pc],
            .last_instruction = instruction
        };
    }

    // MOVT Rd, #imm16
    if ((instruction & 0x0FF00000u) == 0x03400000u) {
        const auto destination = (instruction >> 12) & 0xFu;
        const auto immediate = ((instruction >> 4) & 0xF000u) | (instruction & 0xFFFu);
        state_.registers[destination] =
            (state_.registers[destination] & 0xFFFFu) | (immediate << 16);
        return {
            .reason = ArmStopReason::instruction_limit,
            .instructions_executed = state_.instruction_count,
            .final_pc = state_.registers[register_pc],
            .last_instruction = instruction
        };
    }

    // ADD Rd, Rn, #imm12 (including ARM's rotated-immediate encoding).
    if ((instruction & 0x0FF00000u) == 0x02800000u) {
        const auto source = (instruction >> 16) & 0xFu;
        const auto destination = (instruction >> 12) & 0xFu;
        state_.registers[destination] =
            state_.registers[source] + decode_arm_immediate(instruction);
        return {
            .reason = ArmStopReason::instruction_limit,
            .instructions_executed = state_.instruction_count,
            .final_pc = state_.registers[register_pc],
            .last_instruction = instruction
        };
    }

    // SVC #imm. Vita3K's ARM trampolines place the imported NID in r12.
    if ((instruction & 0x0F000000u) == 0x0F000000u) {
        const auto nid = state_.registers[register_hle_nid];
        const auto dispatched = hle_dispatcher_.dispatch(nid, state_);
        if (!dispatched.handled) {
            return {
                .reason = ArmStopReason::unbound_hle,
                .instructions_executed = state_.instruction_count,
                .final_pc = state_.registers[register_pc],
                .last_instruction = instruction,
                .last_hle_nid = nid,
                .detail = "No diagnostic HLE binding exists for NID " + hexadecimal(nid) + "."
            };
        }
        state_.registers[0] = static_cast<std::uint32_t>(dispatched.return_value);
        return {
            .reason = ArmStopReason::instruction_limit,
            .instructions_executed = state_.instruction_count,
            .final_pc = state_.registers[register_pc],
            .last_instruction = instruction,
            .last_hle_nid = nid,
            .detail = "Dispatched " + dispatched.name + "."
        };
    }

    // BX Rn. A zero target is the deterministic return sentinel for this
    // diagnostic harness; nonzero ARM targets continue execution.
    if ((instruction & 0x0FFFFFF0u) == 0x012FFF10u) {
        const auto source = instruction & 0xFu;
        const auto target = state_.registers[source];
        if (target == 0) {
            return {
                .reason = ArmStopReason::halted,
                .instructions_executed = state_.instruction_count,
                .final_pc = state_.registers[register_pc],
                .last_instruction = instruction,
                .detail = "Guest ARM routine returned through the zero-link sentinel."
            };
        }
        state_.thumb = (target & 1u) != 0;
        state_.registers[register_pc] = target & (state_.thumb ? ~1u : ~3u);
        return {
            .reason = ArmStopReason::instruction_limit,
            .instructions_executed = state_.instruction_count,
            .final_pc = state_.registers[register_pc],
            .last_instruction = instruction
        };
    }

    return {
        .reason = ArmStopReason::unsupported_instruction,
        .instructions_executed = state_.instruction_count,
        .final_pc = pc,
        .last_instruction = instruction,
        .detail = "Unsupported ARM instruction " + hexadecimal(instruction) +
            " at " + hexadecimal(pc) + "."
    };
}

} // namespace vita3k::ios
