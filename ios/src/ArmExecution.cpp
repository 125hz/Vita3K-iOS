#include <vita3k_ios/ArmExecution.h>

#include <vita3k_ios/GuestMemory.h>

#include <algorithm>
#include <array>
#include <bit>
#include <cstring>
#include <iomanip>
#include <limits>
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

bool HLEDispatcher::has_binding(std::uint32_t nid) const {
    return std::ranges::find(bindings_, nid, &Binding::nid) != bindings_.end();
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
    state_.thumb = (entry_point & 1u) != 0;
    state_.registers[register_pc] = entry_point & (state_.thumb ? ~1u : ~3u);
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
        return step_thumb();
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

    // MOV Rd, Rm without a shift.
    if ((instruction & 0x0FFF0FF0u) == 0x01A00000u) {
        const auto destination = (instruction >> 12) & 0xFu;
        const auto source = instruction & 0xFu;
        const auto value = state_.registers[source];
        if (destination == register_pc) {
            state_.thumb = (value & 1u) != 0;
            state_.registers[register_pc] = value & (state_.thumb ? ~1u : ~3u);
        } else {
            state_.registers[destination] = value;
        }
        return {
            .reason = ArmStopReason::instruction_limit,
            .instructions_executed = state_.instruction_count,
            .final_pc = state_.registers[register_pc],
            .last_instruction = instruction
        };
    }

    // B/BL immediate. LR receives the next instruction for BL.
    if ((instruction & 0x0E000000u) == 0x0A000000u) {
        const auto signed_offset = static_cast<std::int32_t>(instruction << 8) >> 6;
        const auto target = static_cast<std::int64_t>(pc) + 8 + signed_offset;
        if (target < 0 || target > std::numeric_limits<std::uint32_t>::max()) {
            return {
                .reason = ArmStopReason::unsupported_instruction,
                .instructions_executed = state_.instruction_count,
                .final_pc = pc,
                .last_instruction = instruction,
                .detail = "ARM branch target leaves guest address space."
            };
        }
        if ((instruction & (1u << 24)) != 0) {
            state_.registers[register_lr] = pc + 4;
        }
        state_.registers[register_pc] = static_cast<std::uint32_t>(target) & ~3u;
        return {
            .reason = ArmStopReason::instruction_limit,
            .instructions_executed = state_.instruction_count,
            .final_pc = state_.registers[register_pc],
            .last_instruction = instruction
        };
    }

    // LDR/STR word with a pre-indexed immediate and no writeback.
    if ((instruction & 0x0F600000u) == 0x05000000u) {
        const bool load = (instruction & (1u << 20)) != 0;
        const bool add_offset = (instruction & (1u << 23)) != 0;
        const auto base_register = (instruction >> 16) & 0xFu;
        const auto data_register = (instruction >> 12) & 0xFu;
        const auto offset = instruction & 0xFFFu;
        const auto address64 = static_cast<std::int64_t>(state_.registers[base_register]) +
            (add_offset ? static_cast<std::int64_t>(offset) : -static_cast<std::int64_t>(offset));
        if (address64 < 0 || address64 > std::numeric_limits<std::uint32_t>::max()) {
            return {
                .reason = ArmStopReason::memory_fault,
                .instructions_executed = state_.instruction_count,
                .final_pc = pc,
                .last_instruction = instruction,
                .detail = "ARM load/store address leaves guest address space."
            };
        }
        const auto address = static_cast<std::uint32_t>(address64);
        std::array<std::uint8_t, sizeof(std::uint32_t)> word{};
        std::string data_error;
        if (load) {
            if (!memory_.read(address, word, data_error)) {
                return {
                    .reason = ArmStopReason::memory_fault,
                    .instructions_executed = state_.instruction_count,
                    .final_pc = pc,
                    .last_instruction = instruction,
                    .detail = "ARM LDR failed: " + data_error
                };
            }
            std::uint32_t value = 0;
            std::memcpy(&value, word.data(), sizeof(value));
            state_.registers[data_register] = value;
        } else {
            const auto value = state_.registers[data_register];
            std::memcpy(word.data(), &value, sizeof(value));
            if (!memory_.write(address, word, data_error)) {
                return {
                    .reason = ArmStopReason::memory_fault,
                    .instructions_executed = state_.instruction_count,
                    .final_pc = pc,
                    .last_instruction = instruction,
                    .detail = "ARM STR failed: " + data_error
                };
            }
        }
        return {
            .reason = ArmStopReason::instruction_limit,
            .instructions_executed = state_.instruction_count,
            .final_pc = state_.registers[register_pc],
            .last_instruction = instruction
        };
    }

    // PUSH alias: STMDB sp!, register-list.
    if ((instruction & 0x0FFF0000u) == 0x092D0000u) {
        const auto register_list = static_cast<std::uint16_t>(instruction);
        const auto register_count = std::popcount(register_list);
        if (register_count == 0) {
            return {
                .reason = ArmStopReason::unsupported_instruction,
                .instructions_executed = state_.instruction_count,
                .final_pc = pc,
                .last_instruction = instruction,
                .detail = "ARM PUSH has an empty register list."
            };
        }
        const auto byte_count = static_cast<std::uint32_t>(register_count * sizeof(std::uint32_t));
        const auto old_sp = state_.registers[register_sp];
        if (old_sp < byte_count) {
            return {
                .reason = ArmStopReason::memory_fault,
                .instructions_executed = state_.instruction_count,
                .final_pc = pc,
                .last_instruction = instruction,
                .detail = "ARM PUSH underflowed guest address space."
            };
        }
        const auto new_sp = old_sp - byte_count;
        std::uint32_t cursor = new_sp;
        for (std::size_t index = 0; index < state_.registers.size(); ++index) {
            if ((register_list & (1u << index)) == 0) {
                continue;
            }
            std::array<std::uint8_t, sizeof(std::uint32_t)> word{};
            std::memcpy(word.data(), &state_.registers[index], sizeof(std::uint32_t));
            std::string stack_error;
            if (!memory_.write(cursor, word, stack_error)) {
                return {
                    .reason = ArmStopReason::memory_fault,
                    .instructions_executed = state_.instruction_count,
                    .final_pc = pc,
                    .last_instruction = instruction,
                    .detail = "ARM PUSH failed: " + stack_error
                };
            }
            cursor += sizeof(std::uint32_t);
        }
        state_.registers[register_sp] = new_sp;
        return {
            .reason = ArmStopReason::instruction_limit,
            .instructions_executed = state_.instruction_count,
            .final_pc = state_.registers[register_pc],
            .last_instruction = instruction
        };
    }

    // POP alias without CPSR restore: LDMIA sp!, register-list.
    if ((instruction & 0x0FFF0000u) == 0x08BD0000u) {
        const auto register_list = static_cast<std::uint16_t>(instruction);
        const auto register_count = std::popcount(register_list);
        if (register_count == 0) {
            return {
                .reason = ArmStopReason::unsupported_instruction,
                .instructions_executed = state_.instruction_count,
                .final_pc = pc,
                .last_instruction = instruction,
                .detail = "ARM POP has an empty register list."
            };
        }
        auto cursor = state_.registers[register_sp];
        for (std::size_t index = 0; index < state_.registers.size(); ++index) {
            if ((register_list & (1u << index)) == 0) {
                continue;
            }
            std::array<std::uint8_t, sizeof(std::uint32_t)> word{};
            std::string stack_error;
            if (!memory_.read(cursor, word, stack_error)) {
                return {
                    .reason = ArmStopReason::memory_fault,
                    .instructions_executed = state_.instruction_count,
                    .final_pc = pc,
                    .last_instruction = instruction,
                    .detail = "ARM POP failed: " + stack_error
                };
            }
            std::memcpy(&state_.registers[index], word.data(), sizeof(std::uint32_t));
            cursor += sizeof(std::uint32_t);
        }
        state_.registers[register_sp] +=
            static_cast<std::uint32_t>(register_count * sizeof(std::uint32_t));
        return {
            .reason = ArmStopReason::instruction_limit,
            .instructions_executed = state_.instruction_count,
            .final_pc = state_.registers[register_pc],
            .last_instruction = instruction
        };
    }

    // SVC #imm. Vita3K's ARM trampolines place the imported NID in r12.
    if ((instruction & 0x0F000000u) == 0x0F000000u) {
        auto nid = state_.registers[register_hle_nid];
        std::array<std::uint8_t, sizeof(std::uint32_t)> inline_nid_bytes{};
        std::string inline_error;
        if (state_.registers[register_pc] <=
                std::numeric_limits<std::uint32_t>::max() - sizeof(std::uint32_t) &&
            memory_.read(state_.registers[register_pc] + sizeof(std::uint32_t),
                inline_nid_bytes, inline_error)) {
            std::uint32_t inline_nid = 0;
            std::memcpy(&inline_nid, inline_nid_bytes.data(), sizeof(inline_nid));
            if (hle_dispatcher_.has_binding(inline_nid)) {
                nid = inline_nid;
            }
        }
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
        if (state_.stop_requested) {
            return {
                .reason = ArmStopReason::halted,
                .instructions_executed = state_.instruction_count,
                .final_pc = state_.registers[register_pc],
                .last_instruction = instruction,
                .last_hle_nid = nid,
                .detail = "HLE call " + dispatched.name + " requested a clean thread stop."
            };
        }
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

ArmExecutionResult ArmInterpreter::step_thumb() {
    const auto pc = state_.registers[register_pc];
    std::array<std::uint8_t, sizeof(std::uint16_t)> bytes{};
    std::string memory_error;
    if (!memory_.read(pc, bytes, memory_error)) {
        return {
            .reason = ArmStopReason::memory_fault,
            .instructions_executed = state_.instruction_count,
            .final_pc = pc,
            .detail = "Thumb instruction fetch failed at " + hexadecimal(pc) + ": " +
                memory_error
        };
    }

    std::uint16_t instruction = 0;
    std::memcpy(&instruction, bytes.data(), sizeof(instruction));
    state_.registers[register_pc] = pc + 2;
    ++state_.instruction_count;

    const auto continued = [&]() {
        return ArmExecutionResult{
            .reason = ArmStopReason::instruction_limit,
            .instructions_executed = state_.instruction_count,
            .final_pc = state_.registers[register_pc],
            .last_instruction = instruction
        };
    };

    // PUSH {R0-R7, LR}. This covers the compact compiler prologue used by the
    // Milestone 9 fixture without claiming the wider Thumb-2 encodings.
    if ((instruction & 0xFE00u) == 0xB400u) {
        const auto register_list = static_cast<std::uint16_t>(
            (instruction & 0x00FFu) | ((instruction & 0x0100u) != 0 ? (1u << register_lr) : 0));
        const auto register_count = std::popcount(register_list);
        if (register_count == 0) {
            return {
                .reason = ArmStopReason::unsupported_instruction,
                .instructions_executed = state_.instruction_count,
                .final_pc = pc,
                .last_instruction = instruction,
                .detail = "Thumb PUSH has an empty register list."
            };
        }
        const auto byte_count = static_cast<std::uint32_t>(register_count * sizeof(std::uint32_t));
        const auto old_sp = state_.registers[register_sp];
        if (old_sp < byte_count) {
            return {
                .reason = ArmStopReason::memory_fault,
                .instructions_executed = state_.instruction_count,
                .final_pc = pc,
                .last_instruction = instruction,
                .detail = "Thumb PUSH underflowed guest address space."
            };
        }
        const auto new_sp = old_sp - byte_count;
        auto cursor = new_sp;
        for (std::size_t index = 0; index < state_.registers.size(); ++index) {
            if ((register_list & (1u << index)) == 0) {
                continue;
            }
            std::array<std::uint8_t, sizeof(std::uint32_t)> word{};
            std::memcpy(word.data(), &state_.registers[index], sizeof(std::uint32_t));
            std::string stack_error;
            if (!memory_.write(cursor, word, stack_error)) {
                return {
                    .reason = ArmStopReason::memory_fault,
                    .instructions_executed = state_.instruction_count,
                    .final_pc = pc,
                    .last_instruction = instruction,
                    .detail = "Thumb PUSH failed: " + stack_error
                };
            }
            cursor += sizeof(std::uint32_t);
        }
        state_.registers[register_sp] = new_sp;
        return continued();
    }

    // LDR Rt, [PC, #imm8*4]. Thumb uses Align(PC+4, 4) as the literal base.
    if ((instruction & 0xF800u) == 0x4800u) {
        const auto destination = (instruction >> 8) & 0x7u;
        const auto address = ((pc + 4u) & ~3u) + ((instruction & 0xFFu) << 2);
        std::array<std::uint8_t, sizeof(std::uint32_t)> word{};
        std::string data_error;
        if (!memory_.read(address, word, data_error)) {
            return {
                .reason = ArmStopReason::memory_fault,
                .instructions_executed = state_.instruction_count,
                .final_pc = pc,
                .last_instruction = instruction,
                .detail = "Thumb literal LDR failed: " + data_error
            };
        }
        std::memcpy(&state_.registers[destination], word.data(), sizeof(std::uint32_t));
        return continued();
    }

    // BLX Rm. LR records a Thumb return address and the low target bit selects
    // the next instruction set. The fixture uses this to call ARM import stubs.
    if ((instruction & 0xFF87u) == 0x4780u) {
        const auto source = (instruction >> 3) & 0xFu;
        const auto target = state_.registers[source];
        state_.registers[register_lr] = (pc + 2u) | 1u;
        state_.thumb = (target & 1u) != 0;
        state_.registers[register_pc] = target & (state_.thumb ? ~1u : ~3u);
        return continued();
    }

    // MOV (register), including the high-register encoding but excluding PC.
    if ((instruction & 0xFF00u) == 0x4600u) {
        const auto destination = (instruction & 0x7u) | ((instruction >> 4) & 0x8u);
        const auto source = (instruction >> 3) & 0xFu;
        if (destination == register_pc) {
            return {
                .reason = ArmStopReason::unsupported_instruction,
                .instructions_executed = state_.instruction_count,
                .final_pc = pc,
                .last_instruction = instruction,
                .detail = "Thumb MOV to PC is outside the Milestone 9 subset."
            };
        }
        state_.registers[destination] = state_.registers[source];
        return continued();
    }

    // STR/LDR Rt, [SP, #imm8*4].
    if ((instruction & 0xF000u) == 0x9000u) {
        const bool load = (instruction & 0x0800u) != 0;
        const auto data_register = (instruction >> 8) & 0x7u;
        const auto address = state_.registers[register_sp] + ((instruction & 0xFFu) << 2);
        std::array<std::uint8_t, sizeof(std::uint32_t)> word{};
        std::string data_error;
        if (load) {
            if (!memory_.read(address, word, data_error)) {
                return {
                    .reason = ArmStopReason::memory_fault,
                    .instructions_executed = state_.instruction_count,
                    .final_pc = pc,
                    .last_instruction = instruction,
                    .detail = "Thumb SP-relative LDR failed: " + data_error
                };
            }
            std::memcpy(&state_.registers[data_register], word.data(), sizeof(std::uint32_t));
        } else {
            std::memcpy(word.data(), &state_.registers[data_register], sizeof(std::uint32_t));
            if (!memory_.write(address, word, data_error)) {
                return {
                    .reason = ArmStopReason::memory_fault,
                    .instructions_executed = state_.instruction_count,
                    .final_pc = pc,
                    .last_instruction = instruction,
                    .detail = "Thumb SP-relative STR failed: " + data_error
                };
            }
        }
        return continued();
    }

    // MOVS Rd, #imm8. Flag updates are intentionally not consumed yet.
    if ((instruction & 0xF800u) == 0x2000u) {
        const auto destination = (instruction >> 8) & 0x7u;
        state_.registers[destination] = instruction & 0xFFu;
        return continued();
    }

    // UXTB Rd, Rm.
    if ((instruction & 0xFFC0u) == 0xB2C0u) {
        const auto source = (instruction >> 3) & 0x7u;
        const auto destination = instruction & 0x7u;
        state_.registers[destination] = state_.registers[source] & 0xFFu;
        return continued();
    }

    // POP {R0-R7, PC}. A zero PC is the diagnostic module-return sentinel.
    if ((instruction & 0xFE00u) == 0xBC00u) {
        const auto register_list = static_cast<std::uint16_t>(
            (instruction & 0x00FFu) | ((instruction & 0x0100u) != 0 ? (1u << register_pc) : 0));
        const auto register_count = std::popcount(register_list);
        if (register_count == 0) {
            return {
                .reason = ArmStopReason::unsupported_instruction,
                .instructions_executed = state_.instruction_count,
                .final_pc = pc,
                .last_instruction = instruction,
                .detail = "Thumb POP has an empty register list."
            };
        }
        auto cursor = state_.registers[register_sp];
        for (std::size_t index = 0; index < state_.registers.size(); ++index) {
            if ((register_list & (1u << index)) == 0) {
                continue;
            }
            std::array<std::uint8_t, sizeof(std::uint32_t)> word{};
            std::string stack_error;
            if (!memory_.read(cursor, word, stack_error)) {
                return {
                    .reason = ArmStopReason::memory_fault,
                    .instructions_executed = state_.instruction_count,
                    .final_pc = pc,
                    .last_instruction = instruction,
                    .detail = "Thumb POP failed: " + stack_error
                };
            }
            std::memcpy(&state_.registers[index], word.data(), sizeof(std::uint32_t));
            cursor += sizeof(std::uint32_t);
        }
        state_.registers[register_sp] +=
            static_cast<std::uint32_t>(register_count * sizeof(std::uint32_t));
        if ((register_list & (1u << register_pc)) != 0) {
            const auto target = state_.registers[register_pc];
            if (target == 0) {
                return {
                    .reason = ArmStopReason::halted,
                    .instructions_executed = state_.instruction_count,
                    .final_pc = 0,
                    .last_instruction = instruction,
                    .detail = "Guest Thumb routine returned through the zero-link sentinel."
                };
            }
            state_.thumb = (target & 1u) != 0;
            state_.registers[register_pc] = target & (state_.thumb ? ~1u : ~3u);
        }
        return continued();
    }

    if (instruction == 0xBF00u) {
        return continued();
    }

    // Thumb-2 BL/BLX immediate. This is the first 32-bit compiler-generated
    // instruction accepted by the real VitaSDK fixture.
    if ((instruction & 0xF800u) == 0xF000u) {
        std::array<std::uint8_t, sizeof(std::uint16_t)> lower_bytes{};
        std::string lower_error;
        if (!memory_.read(pc + 2u, lower_bytes, lower_error)) {
            return {
                .reason = ArmStopReason::memory_fault,
                .instructions_executed = state_.instruction_count,
                .final_pc = pc,
                .last_instruction = instruction,
                .detail = "Thumb-2 branch suffix fetch failed: " + lower_error
            };
        }
        std::uint16_t lower = 0;
        std::memcpy(&lower, lower_bytes.data(), sizeof(lower));
        if ((lower & 0xC000u) == 0xC000u) {
            const auto sign = (instruction >> 10) & 1u;
            const auto j1 = (lower >> 13) & 1u;
            const auto j2 = (lower >> 11) & 1u;
            const auto i1 = (~(j1 ^ sign)) & 1u;
            const auto i2 = (~(j2 ^ sign)) & 1u;
            const bool link_to_thumb = (lower & 0x1000u) != 0;
            std::uint32_t encoded_offset = (sign << 24) | (i1 << 23) | (i2 << 22) |
                ((instruction & 0x03FFu) << 12);
            if (link_to_thumb) {
                encoded_offset |= (lower & 0x07FFu) << 1;
            } else {
                encoded_offset |= ((lower >> 1) & 0x03FFu) << 2;
            }
            const auto offset = static_cast<std::int32_t>(encoded_offset << 7) >> 7;
            const auto base = link_to_thumb ? pc + 4u : (pc + 4u) & ~3u;
            const auto target64 = static_cast<std::int64_t>(base) + offset;
            if (target64 < 0 || target64 > std::numeric_limits<std::uint32_t>::max()) {
                return {
                    .reason = ArmStopReason::unsupported_instruction,
                    .instructions_executed = state_.instruction_count,
                    .final_pc = pc,
                    .last_instruction = static_cast<std::uint32_t>(instruction) |
                        (static_cast<std::uint32_t>(lower) << 16),
                    .detail = "Thumb-2 branch target leaves guest address space."
                };
            }
            state_.registers[register_lr] = (pc + 4u) | 1u;
            state_.thumb = link_to_thumb;
            state_.registers[register_pc] = static_cast<std::uint32_t>(target64) &
                (state_.thumb ? ~1u : ~3u);
            return {
                .reason = ArmStopReason::instruction_limit,
                .instructions_executed = state_.instruction_count,
                .final_pc = state_.registers[register_pc],
                .last_instruction = static_cast<std::uint32_t>(instruction) |
                    (static_cast<std::uint32_t>(lower) << 16)
            };
        }
    }

    // A leading 11101/11110/11111 halfword begins a 32-bit Thumb-2 encoding.
    if ((instruction & 0xF800u) >= 0xE800u) {
        return {
            .reason = ArmStopReason::unsupported_thumb,
            .instructions_executed = state_.instruction_count,
            .final_pc = pc,
            .last_instruction = instruction,
            .detail = "Unsupported 32-bit Thumb-2 instruction prefix " +
                hexadecimal(instruction) + " at " + hexadecimal(pc) + "."
        };
    }

    return {
        .reason = ArmStopReason::unsupported_instruction,
        .instructions_executed = state_.instruction_count,
        .final_pc = pc,
        .last_instruction = instruction,
        .detail = "Unsupported 16-bit Thumb instruction " + hexadecimal(instruction) +
            " at " + hexadecimal(pc) + "."
    };
}

} // namespace vita3k::ios
