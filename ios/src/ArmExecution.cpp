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
constexpr std::uint32_t flag_negative = 1u << 31;
constexpr std::uint32_t flag_zero = 1u << 30;
constexpr std::uint32_t flag_carry = 1u << 29;
constexpr std::uint32_t flag_overflow = 1u << 28;

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

void set_flag(ArmCpuState &state, std::uint32_t flag, bool value) {
    if (value) {
        state.cpsr |= flag;
    } else {
        state.cpsr &= ~flag;
    }
}

bool get_flag(const ArmCpuState &state, std::uint32_t flag) {
    return (state.cpsr & flag) != 0;
}

void set_negative_zero(ArmCpuState &state, std::uint32_t value) {
    set_flag(state, flag_negative, (value & 0x80000000u) != 0);
    set_flag(state, flag_zero, value == 0);
}

struct ArithmeticResult {
    std::uint32_t value{};
    bool carry{};
    bool overflow{};
};

ArithmeticResult add_with_carry(std::uint32_t left, std::uint32_t right, bool carry_in) {
    const auto unsigned_sum = static_cast<std::uint64_t>(left) + right + static_cast<std::uint32_t>(carry_in);
    const auto value = static_cast<std::uint32_t>(unsigned_sum);
    const bool overflow = ((~(left ^ right) & (left ^ value)) & 0x80000000u) != 0;
    return {
        .value = value,
        .carry = (unsigned_sum >> 32) != 0,
        .overflow = overflow
    };
}

void set_arithmetic_flags(ArmCpuState &state, const ArithmeticResult &result) {
    set_negative_zero(state, result.value);
    set_flag(state, flag_carry, result.carry);
    set_flag(state, flag_overflow, result.overflow);
}

struct ShiftResult {
    std::uint32_t value{};
    bool carry{};
};

struct ExpandedImmediate {
    std::uint32_t value{};
    bool carry{};
    bool valid{};
};

ExpandedImmediate thumb_expand_immediate(std::uint16_t upper,
    std::uint16_t lower, bool carry_in) {
    const auto immediate12 = static_cast<std::uint32_t>(
        ((upper & 0x0400u) << 1) | ((lower & 0x7000u) >> 4) | (lower & 0x00FFu));
    if ((immediate12 & 0x0C00u) == 0) {
        const auto immediate8 = immediate12 & 0xFFu;
        switch ((immediate12 >> 8) & 0x3u) {
        case 0:
            return { immediate8, carry_in, true };
        case 1:
            return { (immediate8 << 16) | immediate8, carry_in, immediate8 != 0 };
        case 2:
            return { (immediate8 << 24) | (immediate8 << 8), carry_in, immediate8 != 0 };
        default:
            return { immediate8 * 0x01010101u, carry_in, immediate8 != 0 };
        }
    }

    const auto rotation = (immediate12 >> 7) & 0x1Fu;
    const auto unrotated = 0x80u | (immediate12 & 0x7Fu);
    const auto value = std::rotr(unrotated, static_cast<int>(rotation));
    return { value, (value & 0x80000000u) != 0, true };
}

ShiftResult logical_shift_left(std::uint32_t value, std::uint32_t amount, bool carry_in) {
    if (amount == 0) {
        return { value, carry_in };
    }
    if (amount < 32) {
        return { value << amount, ((value >> (32u - amount)) & 1u) != 0 };
    }
    return { 0, amount == 32 && (value & 1u) != 0 };
}

ShiftResult logical_shift_right(std::uint32_t value, std::uint32_t amount, bool carry_in) {
    if (amount == 0) {
        return { value, carry_in };
    }
    if (amount < 32) {
        return { value >> amount, ((value >> (amount - 1u)) & 1u) != 0 };
    }
    return { 0, amount == 32 && (value & 0x80000000u) != 0 };
}

ShiftResult arithmetic_shift_right(std::uint32_t value, std::uint32_t amount,
    bool carry_in) {
    if (amount == 0) {
        return { value, carry_in };
    }
    if (amount < 32) {
        return {
            static_cast<std::uint32_t>(static_cast<std::int32_t>(value) >> amount),
            ((value >> (amount - 1u)) & 1u) != 0
        };
    }
    const bool sign = (value & 0x80000000u) != 0;
    return { sign ? std::numeric_limits<std::uint32_t>::max() : 0u, sign };
}

ShiftResult rotate_right(std::uint32_t value, std::uint32_t amount, bool carry_in) {
    if (amount == 0) {
        return { value, carry_in };
    }
    const auto rotation = amount & 31u;
    if (rotation == 0) {
        return { value, (value & 0x80000000u) != 0 };
    }
    const auto result = std::rotr(value, static_cast<int>(rotation));
    return { result, (result & 0x80000000u) != 0 };
}

bool condition_passed(const ArmCpuState &state, std::uint32_t condition) {
    const bool negative = get_flag(state, flag_negative);
    const bool zero = get_flag(state, flag_zero);
    const bool carry = get_flag(state, flag_carry);
    const bool overflow = get_flag(state, flag_overflow);
    switch (condition) {
    case 0x0:
        return zero;
    case 0x1:
        return !zero;
    case 0x2:
        return carry;
    case 0x3:
        return !carry;
    case 0x4:
        return negative;
    case 0x5:
        return !negative;
    case 0x6:
        return overflow;
    case 0x7:
        return !overflow;
    case 0x8:
        return carry && !zero;
    case 0x9:
        return !carry || zero;
    case 0xA:
        return negative == overflow;
    case 0xB:
        return negative != overflow;
    case 0xC:
        return !zero && negative == overflow;
    case 0xD:
        return zero || negative != overflow;
    default:
        return false;
    }
}

template <typename T>
bool read_guest_value(GuestMemory &memory, std::uint32_t address, T &value,
    std::string &error) {
    std::array<std::uint8_t, sizeof(T)> bytes{};
    if (!memory.read(address, bytes, error)) {
        return false;
    }
    std::memcpy(&value, bytes.data(), sizeof(value));
    return true;
}

template <typename T>
bool write_guest_value(GuestMemory &memory, std::uint32_t address, T value,
    std::string &error) {
    std::array<std::uint8_t, sizeof(T)> bytes{};
    std::memcpy(bytes.data(), &value, sizeof(value));
    return memory.write(address, bytes, error);
}

std::string thumb_lookahead(GuestMemory &memory, std::uint32_t address,
    std::size_t halfword_count = 6) {
    std::ostringstream stream;
    stream << " Lookahead:";
    for (std::size_t index = 0; index < halfword_count; ++index) {
        const auto offset = static_cast<std::uint64_t>(index) * sizeof(std::uint16_t);
        if (static_cast<std::uint64_t>(address) + offset > std::numeric_limits<std::uint32_t>::max()) {
            break;
        }
        const auto current = address + static_cast<std::uint32_t>(offset);
        std::uint16_t value = 0;
        std::string error;
        if (!read_guest_value(memory, current, value, error)) {
            break;
        }
        stream << ' ' << hexadecimal(current) << "=0x" << std::hex << std::uppercase
               << std::setfill('0') << std::setw(4) << value;
    }
    return stream.str();
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
    bindings_.push_back({ .nid = nid,
        .name = std::move(name),
        .handler = std::move(handler) });
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

    std::uint32_t last_hle_nid = 0;
    for (std::size_t index = 0; index < instruction_limit; ++index) {
        auto result = step();
        if (result.last_hle_nid != 0) {
            last_hle_nid = result.last_hle_nid;
        } else {
            result.last_hle_nid = last_hle_nid;
        }
        if (result.reason != ArmStopReason::instruction_limit) {
            return result;
        }
    }
    return {
        .reason = ArmStopReason::instruction_limit,
        .instructions_executed = state_.instruction_count,
        .final_pc = state_.registers[register_pc],
        .last_hle_nid = last_hle_nid,
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
            .detail = "Only unconditional AL instructions are implemented; decoded " + hexadecimal(instruction) + " at " + hexadecimal(pc) + "."
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
        state_.registers[destination] = (state_.registers[destination] & 0xFFFFu) | (immediate << 16);
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
        state_.registers[destination] = state_.registers[source] + decode_arm_immediate(instruction);
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
        const auto address64 = static_cast<std::int64_t>(state_.registers[base_register]) + (add_offset ? static_cast<std::int64_t>(offset) : -static_cast<std::int64_t>(offset));
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
        state_.registers[register_sp] += static_cast<std::uint32_t>(register_count * sizeof(std::uint32_t));
        return {
            .reason = ArmStopReason::instruction_limit,
            .instructions_executed = state_.instruction_count,
            .final_pc = state_.registers[register_pc],
            .last_instruction = instruction
        };
    }

    // SVC #imm. Direct diagnostic calls place the imported NID in r12. Bound
    // Vita import stubs instead use SVC; MOV pc,lr; inline NID. Resolve that
    // canonical trampoline even when the NID has no HLE handler so the first
    // real unimplemented import is reported instead of a misleading zero r12.
    if ((instruction & 0x0F000000u) == 0x0F000000u) {
        auto nid = state_.registers[register_hle_nid];
        bool inline_trampoline = false;
        std::uint32_t return_instruction = 0;
        std::uint32_t inline_nid = 0;
        std::string inline_error;
        const auto next_pc = state_.registers[register_pc];
        if (next_pc <= std::numeric_limits<std::uint32_t>::max() - 2u * sizeof(std::uint32_t) && read_guest_value(memory_, next_pc, return_instruction, inline_error) && read_guest_value(memory_, next_pc + sizeof(std::uint32_t), inline_nid, inline_error) && return_instruction == 0xE1A0F00Eu && inline_nid != 0) {
            nid = inline_nid;
            inline_trampoline = true;
        }
        const auto dispatched = hle_dispatcher_.dispatch(nid, state_);
        if (!dispatched.handled) {
            std::ostringstream detail;
            detail << "No diagnostic HLE binding exists for NID " << hexadecimal(nid)
                   << "; SVC at " << hexadecimal(pc)
                   << (inline_trampoline ? " resolved from inline import trampoline" : " resolved from r12")
                   << "; LR=" << hexadecimal(state_.registers[register_lr])
                   << "; args r0=" << hexadecimal(state_.registers[0])
                   << " r1=" << hexadecimal(state_.registers[1])
                   << " r2=" << hexadecimal(state_.registers[2])
                   << " r3=" << hexadecimal(state_.registers[3])
                   << "; r12=" << hexadecimal(state_.registers[register_hle_nid]) << ".";
            return {
                .reason = ArmStopReason::unbound_hle,
                .instructions_executed = state_.instruction_count,
                .final_pc = state_.registers[register_pc],
                .last_instruction = instruction,
                .last_hle_nid = nid,
                .detail = detail.str()
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
        .detail = "Unsupported ARM instruction " + hexadecimal(instruction) + " at " + hexadecimal(pc) + "."
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
            .detail = "Thumb instruction fetch failed at " + hexadecimal(pc) + ": " + memory_error
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
    const auto memory_fault = [&](std::string operation, const std::string &error) {
        return ArmExecutionResult{
            .reason = ArmStopReason::memory_fault,
            .instructions_executed = state_.instruction_count,
            .final_pc = pc,
            .last_instruction = instruction,
            .detail = std::move(operation) + " failed: " + error
        };
    };

    // LSL/LSR/ASR (immediate), including the architectural shift-by-32 forms.
    const auto shift_immediate_opcode = instruction & 0xF800u;
    if (shift_immediate_opcode == 0x0000u || shift_immediate_opcode == 0x0800u || shift_immediate_opcode == 0x1000u) {
        const auto amount_field = (instruction >> 6) & 0x1Fu;
        const auto source = (instruction >> 3) & 0x7u;
        const auto destination = instruction & 0x7u;
        const bool carry_in = get_flag(state_, flag_carry);
        ShiftResult shifted;
        if (shift_immediate_opcode == 0x0000u) {
            shifted = logical_shift_left(state_.registers[source], amount_field, carry_in);
        } else if (shift_immediate_opcode == 0x0800u) {
            shifted = logical_shift_right(
                state_.registers[source], amount_field == 0 ? 32u : amount_field, carry_in);
        } else {
            shifted = arithmetic_shift_right(
                state_.registers[source], amount_field == 0 ? 32u : amount_field, carry_in);
        }
        state_.registers[destination] = shifted.value;
        set_negative_zero(state_, shifted.value);
        set_flag(state_, flag_carry, shifted.carry);
        return continued();
    }

    // ADD/SUB with either a register or imm3 operand.
    if ((instruction & 0xF800u) == 0x1800u) {
        const bool immediate = (instruction & 0x0400u) != 0;
        const bool subtract = (instruction & 0x0200u) != 0;
        const auto operand_field = (instruction >> 6) & 0x7u;
        const auto source = (instruction >> 3) & 0x7u;
        const auto destination = instruction & 0x7u;
        const auto operand = immediate ? operand_field : state_.registers[operand_field];
        const auto result = subtract
            ? add_with_carry(state_.registers[source], ~operand, true)
            : add_with_carry(state_.registers[source], operand, false);
        state_.registers[destination] = result.value;
        set_arithmetic_flags(state_, result);
        return continued();
    }

    // MOVS/CMP/ADDS/SUBS with imm8.
    const auto immediate_opcode = instruction & 0xF800u;
    if (immediate_opcode == 0x2000u || immediate_opcode == 0x2800u || immediate_opcode == 0x3000u || immediate_opcode == 0x3800u) {
        const auto destination = (instruction >> 8) & 0x7u;
        const auto immediate = static_cast<std::uint32_t>(instruction & 0xFFu);
        if (immediate_opcode == 0x2000u) {
            state_.registers[destination] = immediate;
            set_negative_zero(state_, immediate);
        } else {
            const bool subtract = immediate_opcode == 0x2800u || immediate_opcode == 0x3800u;
            const auto result = subtract
                ? add_with_carry(state_.registers[destination], ~immediate, true)
                : add_with_carry(state_.registers[destination], immediate, false);
            if (immediate_opcode != 0x2800u) {
                state_.registers[destination] = result.value;
            }
            set_arithmetic_flags(state_, result);
        }
        return continued();
    }

    // Thumb data-processing register operations. These provide the flag state
    // consumed by the conditional-branch family below.
    if ((instruction & 0xFC00u) == 0x4000u) {
        const auto operation = (instruction >> 6) & 0xFu;
        const auto source = (instruction >> 3) & 0x7u;
        const auto destination = instruction & 0x7u;
        const auto left = state_.registers[destination];
        const auto right = state_.registers[source];
        const bool carry_in = get_flag(state_, flag_carry);
        std::uint32_t value = left;
        bool write_result = true;
        bool update_carry = false;
        bool carry = carry_in;
        bool arithmetic_flags = false;
        ArithmeticResult arithmetic;
        switch (operation) {
        case 0x0: // AND
            value = left & right;
            break;
        case 0x1: // EOR
            value = left ^ right;
            break;
        case 0x2: { // LSL register
            const auto shifted = logical_shift_left(left, right & 0xFFu, carry_in);
            value = shifted.value;
            carry = shifted.carry;
            update_carry = (right & 0xFFu) != 0;
            break;
        }
        case 0x3: { // LSR register
            const auto shifted = logical_shift_right(left, right & 0xFFu, carry_in);
            value = shifted.value;
            carry = shifted.carry;
            update_carry = (right & 0xFFu) != 0;
            break;
        }
        case 0x4: { // ASR register
            const auto shifted = arithmetic_shift_right(left, right & 0xFFu, carry_in);
            value = shifted.value;
            carry = shifted.carry;
            update_carry = (right & 0xFFu) != 0;
            break;
        }
        case 0x5: // ADC
            arithmetic = add_with_carry(left, right, carry_in);
            value = arithmetic.value;
            arithmetic_flags = true;
            break;
        case 0x6: // SBC
            arithmetic = add_with_carry(left, ~right, carry_in);
            value = arithmetic.value;
            arithmetic_flags = true;
            break;
        case 0x7: { // ROR register
            const auto shifted = rotate_right(left, right & 0xFFu, carry_in);
            value = shifted.value;
            carry = shifted.carry;
            update_carry = (right & 0xFFu) != 0;
            break;
        }
        case 0x8: // TST
            value = left & right;
            write_result = false;
            break;
        case 0x9: // RSB #0
            arithmetic = add_with_carry(0, ~right, true);
            value = arithmetic.value;
            arithmetic_flags = true;
            break;
        case 0xA: // CMP
            arithmetic = add_with_carry(left, ~right, true);
            value = arithmetic.value;
            arithmetic_flags = true;
            write_result = false;
            break;
        case 0xB: // CMN
            arithmetic = add_with_carry(left, right, false);
            value = arithmetic.value;
            arithmetic_flags = true;
            write_result = false;
            break;
        case 0xC: // ORR
            value = left | right;
            break;
        case 0xD: // MUL
            value = left * right;
            break;
        case 0xE: // BIC
            value = left & ~right;
            break;
        case 0xF: // MVN
            value = ~right;
            break;
        }
        if (write_result) {
            state_.registers[destination] = value;
        }
        if (arithmetic_flags) {
            set_arithmetic_flags(state_, arithmetic);
        } else {
            set_negative_zero(state_, value);
            if (update_carry) {
                set_flag(state_, flag_carry, carry);
            }
        }
        return continued();
    }

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

    // BX/BLX Rm. BLX records a Thumb return address; the target low bit selects
    // the next instruction set for both forms.
    if ((instruction & 0xFF87u) == 0x4700u || (instruction & 0xFF87u) == 0x4780u) {
        const bool link = (instruction & 0x0080u) != 0;
        const auto source = (instruction >> 3) & 0xFu;
        const auto target = state_.registers[source];
        if (link) {
            state_.registers[register_lr] = (pc + 2u) | 1u;
        }
        state_.thumb = (target & 1u) != 0;
        state_.registers[register_pc] = target & (state_.thumb ? ~1u : ~3u);
        return continued();
    }

    // ADD/CMP/MOV high-register operations.
    if ((instruction & 0xFC00u) == 0x4400u && ((instruction >> 8) & 0x3u) != 0x3u) {
        const auto operation = (instruction >> 8) & 0x3u;
        const auto destination = (instruction & 0x7u) | ((instruction >> 4) & 0x8u);
        const auto source = (instruction >> 3) & 0xFu;
        if (operation == 0x0u) {
            const auto value = state_.registers[destination] + state_.registers[source];
            if (destination == register_pc) {
                state_.thumb = true;
                state_.registers[register_pc] = value & ~1u;
            } else {
                state_.registers[destination] = value;
            }
        } else if (operation == 0x1u) {
            const auto result = add_with_carry(
                state_.registers[destination], ~state_.registers[source], true);
            set_arithmetic_flags(state_, result);
        } else {
            const auto value = state_.registers[source];
            if (destination == register_pc) {
                state_.thumb = true;
                state_.registers[register_pc] = value & ~1u;
            } else {
                state_.registers[destination] = value;
            }
        }
        return continued();
    }

    // Register-offset word/halfword/byte loads and stores, including signed loads.
    if ((instruction & 0xF000u) == 0x5000u) {
        const auto operation = (instruction >> 9) & 0x7u;
        const auto offset_register = (instruction >> 6) & 0x7u;
        const auto base_register = (instruction >> 3) & 0x7u;
        const auto data_register = instruction & 0x7u;
        const auto address = state_.registers[base_register] + state_.registers[offset_register];
        std::string error;
        if (operation == 0x0u) {
            if (!write_guest_value(memory_, address, state_.registers[data_register], error)) {
                return memory_fault("Thumb STR (register)", error);
            }
        } else if (operation == 0x1u) {
            if (!write_guest_value(memory_, address,
                    static_cast<std::uint16_t>(state_.registers[data_register]), error)) {
                return memory_fault("Thumb STRH (register)", error);
            }
        } else if (operation == 0x2u) {
            if (!write_guest_value(memory_, address,
                    static_cast<std::uint8_t>(state_.registers[data_register]), error)) {
                return memory_fault("Thumb STRB (register)", error);
            }
        } else if (operation == 0x3u) {
            std::int8_t value = 0;
            if (!read_guest_value(memory_, address, value, error)) {
                return memory_fault("Thumb LDRSB (register)", error);
            }
            state_.registers[data_register] = static_cast<std::uint32_t>(
                static_cast<std::int32_t>(value));
        } else if (operation == 0x4u) {
            if (!read_guest_value(memory_, address, state_.registers[data_register], error)) {
                return memory_fault("Thumb LDR (register)", error);
            }
        } else if (operation == 0x5u) {
            std::uint16_t value = 0;
            if (!read_guest_value(memory_, address, value, error)) {
                return memory_fault("Thumb LDRH (register)", error);
            }
            state_.registers[data_register] = value;
        } else if (operation == 0x6u) {
            std::uint8_t value = 0;
            if (!read_guest_value(memory_, address, value, error)) {
                return memory_fault("Thumb LDRB (register)", error);
            }
            state_.registers[data_register] = value;
        } else {
            std::int16_t value = 0;
            if (!read_guest_value(memory_, address, value, error)) {
                return memory_fault("Thumb LDRSH (register)", error);
            }
            state_.registers[data_register] = static_cast<std::uint32_t>(
                static_cast<std::int32_t>(value));
        }
        return continued();
    }

    // Immediate-offset word and byte loads/stores.
    if ((instruction & 0xE000u) == 0x6000u) {
        const bool byte_access = (instruction & 0x1000u) != 0;
        const bool load = (instruction & 0x0800u) != 0;
        const auto immediate = (instruction >> 6) & 0x1Fu;
        const auto base_register = (instruction >> 3) & 0x7u;
        const auto data_register = instruction & 0x7u;
        const auto address = state_.registers[base_register] + (byte_access ? immediate : immediate << 2);
        std::string error;
        if (byte_access) {
            if (load) {
                std::uint8_t value = 0;
                if (!read_guest_value(memory_, address, value, error)) {
                    return memory_fault("Thumb LDRB (immediate)", error);
                }
                state_.registers[data_register] = value;
            } else if (!write_guest_value(memory_, address,
                           static_cast<std::uint8_t>(state_.registers[data_register]), error)) {
                return memory_fault("Thumb STRB (immediate)", error);
            }
        } else if (load) {
            if (!read_guest_value(memory_, address, state_.registers[data_register], error)) {
                return memory_fault("Thumb LDR (immediate)", error);
            }
        } else if (!write_guest_value(
                       memory_, address, state_.registers[data_register], error)) {
            return memory_fault("Thumb STR (immediate)", error);
        }
        return continued();
    }

    // Immediate-offset halfword loads/stores.
    if ((instruction & 0xF000u) == 0x8000u) {
        const bool load = (instruction & 0x0800u) != 0;
        const auto immediate = ((instruction >> 6) & 0x1Fu) << 1;
        const auto base_register = (instruction >> 3) & 0x7u;
        const auto data_register = instruction & 0x7u;
        const auto address = state_.registers[base_register] + immediate;
        std::string error;
        if (load) {
            std::uint16_t value = 0;
            if (!read_guest_value(memory_, address, value, error)) {
                return memory_fault("Thumb LDRH (immediate)", error);
            }
            state_.registers[data_register] = value;
        } else if (!write_guest_value(memory_, address,
                       static_cast<std::uint16_t>(state_.registers[data_register]), error)) {
            return memory_fault("Thumb STRH (immediate)", error);
        }
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

    // ADR and ADD Rd, SP, #imm8*4.
    if ((instruction & 0xF000u) == 0xA000u) {
        const bool use_stack = (instruction & 0x0800u) != 0;
        const auto destination = (instruction >> 8) & 0x7u;
        const auto immediate = static_cast<std::uint32_t>(instruction & 0xFFu) << 2;
        const auto base = use_stack ? state_.registers[register_sp] : ((pc + 4u) & ~3u);
        state_.registers[destination] = base + immediate;
        return continued();
    }

    // ADD/SUB SP, #imm7*4. The accepted Amagami Milestone 19 boundary is B082.
    if ((instruction & 0xFF00u) == 0xB000u) {
        const bool subtract = (instruction & 0x0080u) != 0;
        const auto immediate = static_cast<std::uint32_t>(instruction & 0x7Fu) << 2;
        state_.registers[register_sp] = subtract
            ? state_.registers[register_sp] - immediate
            : state_.registers[register_sp] + immediate;
        return continued();
    }

    // Compare-and-branch on zero/nonzero. This forward-only form does not consume flags.
    if ((instruction & 0xF500u) == 0xB100u) {
        const bool nonzero = (instruction & 0x0800u) != 0;
        const auto source = instruction & 0x7u;
        const auto immediate = ((instruction & 0x0200u) >> 3) | ((instruction & 0x00F8u) >> 2);
        const bool take_branch = (state_.registers[source] != 0) == nonzero;
        if (take_branch) {
            state_.registers[register_pc] = pc + 4u + immediate;
        }
        return continued();
    }

    // SXTH/SXTB/UXTH/UXTB.
    if ((instruction & 0xFF00u) == 0xB200u) {
        const auto operation = (instruction >> 6) & 0x3u;
        const auto source = (instruction >> 3) & 0x7u;
        const auto destination = instruction & 0x7u;
        const auto value = state_.registers[source];
        if (operation == 0x0u) {
            state_.registers[destination] = static_cast<std::uint32_t>(
                static_cast<std::int32_t>(static_cast<std::int16_t>(value)));
        } else if (operation == 0x1u) {
            state_.registers[destination] = static_cast<std::uint32_t>(
                static_cast<std::int32_t>(static_cast<std::int8_t>(value)));
        } else if (operation == 0x2u) {
            state_.registers[destination] = value & 0xFFFFu;
        } else {
            state_.registers[destination] = value & 0xFFu;
        }
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
        state_.registers[register_sp] += static_cast<std::uint32_t>(register_count * sizeof(std::uint32_t));
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

    // STMIA/LDMIA over the compact low-register list.
    if ((instruction & 0xF000u) == 0xC000u) {
        const bool load = (instruction & 0x0800u) != 0;
        const auto base_register = (instruction >> 8) & 0x7u;
        const auto register_list = static_cast<std::uint8_t>(instruction & 0xFFu);
        const auto register_count = std::popcount(register_list);
        if (register_count == 0) {
            return {
                .reason = ArmStopReason::unsupported_instruction,
                .instructions_executed = state_.instruction_count,
                .final_pc = pc,
                .last_instruction = instruction,
                .detail = "Thumb STM/LDM has an empty register list."
            };
        }
        auto cursor = state_.registers[base_register];
        for (std::size_t index = 0; index < 8; ++index) {
            if ((register_list & (1u << index)) == 0) {
                continue;
            }
            std::string error;
            if (load) {
                if (!read_guest_value(memory_, cursor, state_.registers[index], error)) {
                    return memory_fault("Thumb LDMIA", error);
                }
            } else if (!write_guest_value(memory_, cursor, state_.registers[index], error)) {
                return memory_fault("Thumb STMIA", error);
            }
            cursor += sizeof(std::uint32_t);
        }
        if (!load || (register_list & (1u << base_register)) == 0) {
            state_.registers[base_register] += static_cast<std::uint32_t>(
                register_count * sizeof(std::uint32_t));
        }
        return continued();
    }

    if (instruction == 0xBF00u) {
        return continued();
    }

    // Conditional branch. Conditions E and F are reserved for UDF/SVC here.
    if ((instruction & 0xF000u) == 0xD000u && ((instruction >> 8) & 0xFu) < 0xEu) {
        const auto condition = (instruction >> 8) & 0xFu;
        if (condition_passed(state_, condition)) {
            const auto encoded = static_cast<std::uint32_t>(instruction & 0xFFu) << 1;
            const auto offset = static_cast<std::int32_t>(encoded << 23) >> 23;
            const auto target = static_cast<std::int64_t>(pc) + 4 + offset;
            if (target < 0 || target > std::numeric_limits<std::uint32_t>::max()) {
                return {
                    .reason = ArmStopReason::unsupported_instruction,
                    .instructions_executed = state_.instruction_count,
                    .final_pc = pc,
                    .last_instruction = instruction,
                    .detail = "Thumb conditional branch target leaves guest address space."
                };
            }
            state_.registers[register_pc] = static_cast<std::uint32_t>(target) & ~1u;
        }
        return continued();
    }

    // Unconditional 16-bit branch.
    if ((instruction & 0xF800u) == 0xE000u) {
        const auto encoded = static_cast<std::uint32_t>(instruction & 0x07FFu) << 1;
        const auto offset = static_cast<std::int32_t>(encoded << 20) >> 20;
        const auto target = static_cast<std::int64_t>(pc) + 4 + offset;
        if (target < 0 || target > std::numeric_limits<std::uint32_t>::max()) {
            return {
                .reason = ArmStopReason::unsupported_instruction,
                .instructions_executed = state_.instruction_count,
                .final_pc = pc,
                .last_instruction = instruction,
                .detail = "Thumb branch target leaves guest address space."
            };
        }
        state_.registers[register_pc] = static_cast<std::uint32_t>(target) & ~1u;
        return continued();
    }

    // PUSH.W {registers} is the Thumb-2 alias of STMDB sp!, {registers}.
    // Commercial modules commonly use this form when their prologue saves
    // high registers that cannot be represented by the compact PUSH encoding.
    if (instruction == 0xE92Du) {
        std::array<std::uint8_t, sizeof(std::uint16_t)> lower_bytes{};
        std::string lower_error;
        if (!memory_.read(pc + 2u, lower_bytes, lower_error)) {
            return {
                .reason = ArmStopReason::memory_fault,
                .instructions_executed = state_.instruction_count,
                .final_pc = pc,
                .last_instruction = instruction,
                .detail = "Thumb-2 PUSH.W register-list fetch failed: " + lower_error
            };
        }
        std::uint16_t register_list = 0;
        std::memcpy(&register_list, lower_bytes.data(), sizeof(register_list));
        const auto packed_instruction = static_cast<std::uint32_t>(instruction) | (static_cast<std::uint32_t>(register_list) << 16);
        const auto displayed_instruction = (static_cast<std::uint32_t>(instruction) << 16) | register_list;
        state_.registers[register_pc] = pc + 4u;

        const auto invalid_registers = static_cast<std::uint16_t>(
            (1u << register_sp) | (1u << register_pc));
        const auto register_count = std::popcount(register_list);
        if ((register_list & invalid_registers) != 0 || register_count < 2) {
            return {
                .reason = ArmStopReason::unsupported_instruction,
                .instructions_executed = state_.instruction_count,
                .final_pc = pc,
                .last_instruction = packed_instruction,
                .detail = "Unsupported or unpredictable Thumb-2 PUSH.W " + hexadecimal(displayed_instruction) + " at " + hexadecimal(pc) + "."
            };
        }

        const auto byte_count = static_cast<std::uint32_t>(
            register_count * sizeof(std::uint32_t));
        const auto old_sp = state_.registers[register_sp];
        if (old_sp < byte_count) {
            return {
                .reason = ArmStopReason::memory_fault,
                .instructions_executed = state_.instruction_count,
                .final_pc = pc,
                .last_instruction = packed_instruction,
                .detail = "Thumb-2 PUSH.W underflowed guest address space."
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
                    .last_instruction = packed_instruction,
                    .detail = "Thumb-2 PUSH.W failed: " + stack_error
                };
            }
            cursor += sizeof(std::uint32_t);
        }
        state_.registers[register_sp] = new_sp;
        return {
            .reason = ArmStopReason::instruction_limit,
            .instructions_executed = state_.instruction_count,
            .final_pc = state_.registers[register_pc],
            .last_instruction = packed_instruction
        };
    }

    // Thumb-2 data processing with a modified immediate. Decode the complete
    // architectural immediate expansion and the compiler-facing logical and
    // arithmetic family, including MOV/MVN and the flag-only test aliases.
    if ((instruction & 0xFA00u) == 0xF000u) {
        std::uint16_t lower = 0;
        std::string lower_error;
        if (!read_guest_value(memory_, pc + 2u, lower, lower_error)) {
            return memory_fault("Thumb-2 modified-immediate suffix fetch", lower_error);
        }
        if ((lower & 0x8000u) == 0) {
            const auto packed_instruction = static_cast<std::uint32_t>(instruction) | (static_cast<std::uint32_t>(lower) << 16);
            const auto displayed_instruction = (static_cast<std::uint32_t>(instruction) << 16) | lower;
            const auto operation = (instruction >> 5) & 0xFu;
            const bool set_flags = (instruction & 0x0010u) != 0;
            const auto source = instruction & 0xFu;
            const auto destination = (lower >> 8) & 0xFu;
            const bool move_alias = source == register_pc && (operation == 0x2u || operation == 0x3u);
            const bool test_alias = destination == register_pc && set_flags && (operation == 0x0u || operation == 0x4u || operation == 0x8u || operation == 0xDu);
            const bool logical_operation = operation <= 0x4u;
            const bool arithmetic_operation = operation == 0x8u || operation == 0xAu || operation == 0xBu || operation == 0xDu || operation == 0xEu;
            const bool stack_arithmetic = (operation == 0x8u || operation == 0xDu) && !test_alias;
            const auto expanded = thumb_expand_immediate(
                instruction, lower, get_flag(state_, flag_carry));
            const bool invalid_registers = (!move_alias && source == register_pc) || (!test_alias && destination == register_pc) || ((source == register_sp || destination == register_sp) && (!stack_arithmetic || (destination == register_sp && set_flags))) || (test_alias && source == register_sp);
            state_.registers[register_pc] = pc + 4u;
            if ((!logical_operation && !arithmetic_operation) || !expanded.valid || invalid_registers) {
                return {
                    .reason = ArmStopReason::unsupported_instruction,
                    .instructions_executed = state_.instruction_count,
                    .final_pc = pc,
                    .last_instruction = packed_instruction,
                    .detail = "Unsupported or unpredictable Thumb-2 modified-immediate instruction " + hexadecimal(displayed_instruction) + " at " + hexadecimal(pc) + "."
                };
            }

            const auto left = move_alias ? 0u : state_.registers[source];
            std::uint32_t value = expanded.value;
            bool arithmetic_flags = false;
            ArithmeticResult arithmetic;
            switch (operation) {
            case 0x0: // AND / TST
                value = left & expanded.value;
                break;
            case 0x1: // BIC
                value = left & ~expanded.value;
                break;
            case 0x2: // ORR / MOV
                value = move_alias ? expanded.value : left | expanded.value;
                break;
            case 0x3: // ORN / MVN
                value = move_alias ? ~expanded.value : left | ~expanded.value;
                break;
            case 0x4: // EOR / TEQ
                value = left ^ expanded.value;
                break;
            case 0x8: // ADD / CMN
                arithmetic = add_with_carry(left, expanded.value, false);
                value = arithmetic.value;
                arithmetic_flags = true;
                break;
            case 0xA: // ADC
                arithmetic = add_with_carry(left, expanded.value,
                    get_flag(state_, flag_carry));
                value = arithmetic.value;
                arithmetic_flags = true;
                break;
            case 0xB: // SBC
                arithmetic = add_with_carry(left, ~expanded.value,
                    get_flag(state_, flag_carry));
                value = arithmetic.value;
                arithmetic_flags = true;
                break;
            case 0xD: // SUB / CMP
                arithmetic = add_with_carry(left, ~expanded.value, true);
                value = arithmetic.value;
                arithmetic_flags = true;
                break;
            case 0xE: // RSB
                arithmetic = add_with_carry(expanded.value, ~left, true);
                value = arithmetic.value;
                arithmetic_flags = true;
                break;
            default:
                break;
            }

            if (!test_alias) {
                state_.registers[destination] = value;
            }
            if (set_flags) {
                if (arithmetic_flags) {
                    set_arithmetic_flags(state_, arithmetic);
                } else {
                    set_negative_zero(state_, value);
                    set_flag(state_, flag_carry, expanded.carry);
                }
            }
            return {
                .reason = ArmStopReason::instruction_limit,
                .instructions_executed = state_.instruction_count,
                .final_pc = state_.registers[register_pc],
                .last_instruction = packed_instruction
            };
        }
    }

    // Thumb-2 wide single-register memory operations. This covers unsigned
    // loads/stores and signed loads for byte, halfword, and word sizes in the
    // imm12 offset and imm8 pre/post-indexed forms.
    const auto wide_memory_opcode = instruction & 0xFFF0u;
    const bool wide_memory_imm12 = wide_memory_opcode == 0xF880u || wide_memory_opcode == 0xF890u || wide_memory_opcode == 0xF8A0u || wide_memory_opcode == 0xF8B0u || wide_memory_opcode == 0xF8C0u || wide_memory_opcode == 0xF8D0u || wide_memory_opcode == 0xF990u || wide_memory_opcode == 0xF9B0u;
    const bool wide_memory_imm8 = wide_memory_opcode == 0xF800u || wide_memory_opcode == 0xF810u || wide_memory_opcode == 0xF820u || wide_memory_opcode == 0xF830u || wide_memory_opcode == 0xF840u || wide_memory_opcode == 0xF850u || wide_memory_opcode == 0xF910u || wide_memory_opcode == 0xF930u;
    if (wide_memory_imm12 || wide_memory_imm8) {
        std::uint16_t lower = 0;
        std::string lower_error;
        if (!read_guest_value(memory_, pc + 2u, lower, lower_error)) {
            return memory_fault("Thumb-2 wide memory suffix fetch", lower_error);
        }
        if (wide_memory_imm12 || (lower & 0x0800u) != 0) {
            const auto packed_instruction = static_cast<std::uint32_t>(instruction) | (static_cast<std::uint32_t>(lower) << 16);
            const auto displayed_instruction = (static_cast<std::uint32_t>(instruction) << 16) | lower;
            const bool load = wide_memory_opcode == 0xF890u || wide_memory_opcode == 0xF8B0u || wide_memory_opcode == 0xF8D0u || wide_memory_opcode == 0xF990u || wide_memory_opcode == 0xF9B0u || wide_memory_opcode == 0xF810u || wide_memory_opcode == 0xF830u || wide_memory_opcode == 0xF850u || wide_memory_opcode == 0xF910u || wide_memory_opcode == 0xF930u;
            const bool signed_load = wide_memory_opcode == 0xF990u || wide_memory_opcode == 0xF9B0u || wide_memory_opcode == 0xF910u || wide_memory_opcode == 0xF930u;
            const std::size_t access_size = (wide_memory_opcode == 0xF880u || wide_memory_opcode == 0xF890u || wide_memory_opcode == 0xF990u || wide_memory_opcode == 0xF800u || wide_memory_opcode == 0xF810u || wide_memory_opcode == 0xF910u)
                ? 1u
                : (wide_memory_opcode == 0xF8A0u || wide_memory_opcode == 0xF8B0u || wide_memory_opcode == 0xF9B0u || wide_memory_opcode == 0xF820u || wide_memory_opcode == 0xF830u || wide_memory_opcode == 0xF930u)
                ? 2u
                : 4u;
            const auto base_register = instruction & 0xFu;
            const auto target_register = (lower >> 12) & 0xFu;
            const bool pre_index = wide_memory_imm12 || (lower & 0x0400u) != 0;
            const bool add_offset = wide_memory_imm12 || (lower & 0x0200u) != 0;
            const bool writeback_bit = (lower & 0x0100u) != 0;
            const bool writeback = !wide_memory_imm12 && (!pre_index || writeback_bit);
            const auto immediate = static_cast<std::uint32_t>(
                wide_memory_imm12 ? lower & 0x0FFFu : lower & 0x00FFu);
            const bool literal_load = wide_memory_imm12 && load && base_register == register_pc;
            const bool unprivileged_access = !wide_memory_imm12 && pre_index && add_offset && !writeback_bit;
            const bool invalid = (!wide_memory_imm12 && !pre_index && !writeback_bit) || unprivileged_access || (!literal_load && base_register == register_pc) || target_register == register_pc || (target_register == register_sp && access_size != sizeof(std::uint32_t)) || (writeback && base_register == target_register);
            state_.registers[register_pc] = pc + 4u;
            if (invalid) {
                return {
                    .reason = ArmStopReason::unsupported_instruction,
                    .instructions_executed = state_.instruction_count,
                    .final_pc = pc,
                    .last_instruction = packed_instruction,
                    .detail = "Unsupported or unpredictable Thumb-2 wide memory instruction " + hexadecimal(displayed_instruction) + " at " + hexadecimal(pc) + "."
                };
            }

            const auto base = literal_load
                ? ((pc + 4u) & ~3u)
                : state_.registers[base_register];
            const auto offset_address_64 = add_offset
                ? static_cast<std::uint64_t>(base) + immediate
                : (base >= immediate
                          ? static_cast<std::uint64_t>(base - immediate)
                          : std::numeric_limits<std::uint64_t>::max());
            if (offset_address_64 > std::numeric_limits<std::uint32_t>::max()) {
                return {
                    .reason = ArmStopReason::memory_fault,
                    .instructions_executed = state_.instruction_count,
                    .final_pc = pc,
                    .last_instruction = packed_instruction,
                    .detail = "Thumb-2 wide memory address overflowed guest address space."
                };
            }
            const auto offset_address = static_cast<std::uint32_t>(offset_address_64);
            const auto address = pre_index ? offset_address : base;
            std::array<std::uint8_t, sizeof(std::uint32_t)> data{};
            std::string data_error;
            if (load) {
                if (!memory_.read(address, std::span(data).first(access_size), data_error)) {
                    return memory_fault("Thumb-2 wide load", data_error);
                }
                std::uint32_t value = 0;
                std::memcpy(&value, data.data(), access_size);
                if (signed_load && access_size == 1 && (value & 0x80u) != 0) {
                    value |= 0xFFFFFF00u;
                } else if (signed_load && access_size == 2 && (value & 0x8000u) != 0) {
                    value |= 0xFFFF0000u;
                }
                state_.registers[target_register] = value;
            } else {
                std::memcpy(data.data(), &state_.registers[target_register], access_size);
                if (!memory_.write(address,
                        std::span<const std::uint8_t>(data.data(), access_size), data_error)) {
                    return memory_fault("Thumb-2 wide store", data_error);
                }
            }
            if (writeback) {
                state_.registers[base_register] = offset_address;
            }
            return {
                .reason = ArmStopReason::instruction_limit,
                .instructions_executed = state_.instruction_count,
                .final_pc = state_.registers[register_pc],
                .last_instruction = packed_instruction
            };
        }
    }

    // MOVW/MOVT construct compiler constants without touching flags. The
    // accepted Amagami Milestone 20 boundary starts with MOVW r2, #0x7690 and
    // later completes the pointer with MOVT r2, #0x812c.
    const auto move_wide_opcode = instruction & 0xFBF0u;
    if (move_wide_opcode == 0xF240u || move_wide_opcode == 0xF2C0u) {
        std::array<std::uint8_t, sizeof(std::uint16_t)> lower_bytes{};
        std::string lower_error;
        if (!memory_.read(pc + 2u, lower_bytes, lower_error)) {
            return {
                .reason = ArmStopReason::memory_fault,
                .instructions_executed = state_.instruction_count,
                .final_pc = pc,
                .last_instruction = instruction,
                .detail = "Thumb-2 MOVW/MOVT suffix fetch failed: " + lower_error
            };
        }
        std::uint16_t lower = 0;
        std::memcpy(&lower, lower_bytes.data(), sizeof(lower));
        const auto packed_instruction = static_cast<std::uint32_t>(instruction) | (static_cast<std::uint32_t>(lower) << 16);
        const auto displayed_instruction = (static_cast<std::uint32_t>(instruction) << 16) | lower;
        const auto destination = (lower >> 8) & 0xFu;
        state_.registers[register_pc] = pc + 4u;
        if (destination == register_pc || (lower & 0x8000u) != 0) {
            return {
                .reason = ArmStopReason::unsupported_instruction,
                .instructions_executed = state_.instruction_count,
                .final_pc = pc,
                .last_instruction = packed_instruction,
                .detail = "Unsupported or unpredictable Thumb-2 MOVW/MOVT " + hexadecimal(displayed_instruction) + " at " + hexadecimal(pc) + "."
            };
        }
        const auto immediate = static_cast<std::uint32_t>(
            ((instruction & 0x000Fu) << 12) | ((instruction & 0x0400u) << 1) | ((lower & 0x7000u) >> 4) | (lower & 0x00FFu));
        if (move_wide_opcode == 0xF240u) {
            state_.registers[destination] = immediate;
        } else {
            state_.registers[destination] = (state_.registers[destination] & 0x0000FFFFu) | (immediate << 16);
        }
        return {
            .reason = ArmStopReason::instruction_limit,
            .instructions_executed = state_.instruction_count,
            .final_pc = state_.registers[register_pc],
            .last_instruction = packed_instruction
        };
    }

    // LDRD/STRD immediate forms. This covers both post-indexed/writeback T1
    // and offset/pre-indexed T2 encodings as one coherent doubleword-memory
    // family. The accepted Amagami sequence uses STRD r1, r0, [sp].
    const auto dual_t1_opcode = instruction & 0xFF70u;
    const auto dual_t2_opcode = instruction & 0xFF50u;
    const bool dual_t1 = dual_t1_opcode == 0xE860u || dual_t1_opcode == 0xE870u;
    const bool dual_t2 = dual_t2_opcode == 0xE940u || dual_t2_opcode == 0xE950u;
    if (dual_t1 || dual_t2) {
        std::array<std::uint8_t, sizeof(std::uint16_t)> lower_bytes{};
        std::string lower_error;
        if (!memory_.read(pc + 2u, lower_bytes, lower_error)) {
            return {
                .reason = ArmStopReason::memory_fault,
                .instructions_executed = state_.instruction_count,
                .final_pc = pc,
                .last_instruction = instruction,
                .detail = "Thumb-2 LDRD/STRD suffix fetch failed: " + lower_error
            };
        }
        std::uint16_t lower = 0;
        std::memcpy(&lower, lower_bytes.data(), sizeof(lower));
        const auto packed_instruction = static_cast<std::uint32_t>(instruction) | (static_cast<std::uint32_t>(lower) << 16);
        const auto displayed_instruction = (static_cast<std::uint32_t>(instruction) << 16) | lower;
        const bool load = dual_t1 ? dual_t1_opcode == 0xE870u : dual_t2_opcode == 0xE950u;
        const bool add_offset = (instruction & 0x0080u) != 0;
        const bool pre_index = dual_t2;
        const bool writeback = dual_t1 || (instruction & 0x0020u) != 0;
        const auto base_register = instruction & 0xFu;
        const auto first_register = (lower >> 12) & 0xFu;
        const auto second_register = (lower >> 8) & 0xFu;
        const auto immediate = static_cast<std::uint32_t>(lower & 0xFFu) << 2;
        const bool literal_load = load && base_register == register_pc;
        const bool invalid = first_register == register_pc || second_register == register_pc || (load && first_register == second_register) || (!load && base_register == register_pc) || (writeback && (base_register == first_register || base_register == second_register)) || (literal_load && (!pre_index || writeback));
        state_.registers[register_pc] = pc + 4u;
        if (invalid) {
            return {
                .reason = ArmStopReason::unsupported_instruction,
                .instructions_executed = state_.instruction_count,
                .final_pc = pc,
                .last_instruction = packed_instruction,
                .detail = "Unsupported or unpredictable Thumb-2 LDRD/STRD " + hexadecimal(displayed_instruction) + " at " + hexadecimal(pc) + "."
            };
        }

        const auto base = literal_load
            ? ((pc + 4u) & ~3u)
            : state_.registers[base_register];
        const auto offset_address = add_offset ? base + immediate : base - immediate;
        const auto address = pre_index ? offset_address : base;
        std::array<std::uint8_t, sizeof(std::uint64_t)> doubleword{};
        std::string data_error;
        if (load) {
            if (!memory_.read(address, doubleword, data_error)) {
                return memory_fault("Thumb-2 LDRD", data_error);
            }
            std::memcpy(&state_.registers[first_register], doubleword.data(), sizeof(std::uint32_t));
            std::memcpy(&state_.registers[second_register], doubleword.data() + sizeof(std::uint32_t), sizeof(std::uint32_t));
        } else {
            std::memcpy(doubleword.data(), &state_.registers[first_register], sizeof(std::uint32_t));
            std::memcpy(doubleword.data() + sizeof(std::uint32_t), &state_.registers[second_register], sizeof(std::uint32_t));
            if (!memory_.write(address, doubleword, data_error)) {
                return memory_fault("Thumb-2 STRD", data_error);
            }
        }
        if (writeback) {
            state_.registers[base_register] = offset_address;
        }
        return {
            .reason = ArmStopReason::instruction_limit,
            .instructions_executed = state_.instruction_count,
            .final_pc = state_.registers[register_pc],
            .last_instruction = packed_instruction
        };
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
            std::uint32_t encoded_offset = (sign << 24) | (i1 << 23) | (i2 << 22) | ((instruction & 0x03FFu) << 12);
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
                    .last_instruction = static_cast<std::uint32_t>(instruction) | (static_cast<std::uint32_t>(lower) << 16),
                    .detail = "Thumb-2 branch target leaves guest address space."
                };
            }
            state_.registers[register_lr] = (pc + 4u) | 1u;
            state_.thumb = link_to_thumb;
            state_.registers[register_pc] = static_cast<std::uint32_t>(target64) & (state_.thumb ? ~1u : ~3u);
            return {
                .reason = ArmStopReason::instruction_limit,
                .instructions_executed = state_.instruction_count,
                .final_pc = state_.registers[register_pc],
                .last_instruction = static_cast<std::uint32_t>(instruction) | (static_cast<std::uint32_t>(lower) << 16)
            };
        }
    }

    // A leading 11101/11110/11111 halfword begins a 32-bit Thumb-2 encoding.
    if ((instruction & 0xF800u) >= 0xE800u) {
        std::array<std::uint8_t, sizeof(std::uint16_t)> lower_bytes{};
        std::string lower_error;
        if (!memory_.read(pc + 2u, lower_bytes, lower_error)) {
            return {
                .reason = ArmStopReason::memory_fault,
                .instructions_executed = state_.instruction_count,
                .final_pc = pc,
                .last_instruction = instruction,
                .detail = "Thumb-2 instruction suffix fetch failed: " + lower_error
            };
        }
        std::uint16_t lower = 0;
        std::memcpy(&lower, lower_bytes.data(), sizeof(lower));
        const auto packed_instruction = static_cast<std::uint32_t>(instruction) | (static_cast<std::uint32_t>(lower) << 16);
        const auto displayed_instruction = (static_cast<std::uint32_t>(instruction) << 16) | lower;
        return {
            .reason = ArmStopReason::unsupported_thumb,
            .instructions_executed = state_.instruction_count,
            .final_pc = pc,
            .last_instruction = packed_instruction,
            .detail = "Unsupported 32-bit Thumb-2 instruction " + hexadecimal(displayed_instruction) + " at " + hexadecimal(pc) + "." + thumb_lookahead(memory_, pc + 4u)
        };
    }

    return {
        .reason = ArmStopReason::unsupported_instruction,
        .instructions_executed = state_.instruction_count,
        .final_pc = pc,
        .last_instruction = instruction,
        .detail = "Unsupported 16-bit Thumb instruction " + hexadecimal(instruction) + " at " + hexadecimal(pc) + "." + thumb_lookahead(memory_, pc + 2u)
    };
}

} // namespace vita3k::ios
