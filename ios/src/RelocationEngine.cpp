#include <vita3k_ios/RelocationEngine.h>

#include <vita3k_ios/ExecutableProbe.h>
#include <vita3k_ios/GuestMemory.h>

#include <array>
#include <cstring>
#include <limits>
#include <sstream>

namespace vita3k::ios {
namespace {

enum RelocationCode : std::uint32_t {
    None = 0,
    Abs32 = 2,
    Rel32 = 3,
    Abs8 = 8,
    ThumbCall = 10,
    Call = 28,
    Jump24 = 29,
    Target1 = 38,
    V4BX = 40,
    Target2 = 41,
    Prel31 = 42,
    MovwAbsNc = 43,
    MovtAbs = 44,
    ThumbMovwAbsNc = 47,
    ThumbMovtAbs = 48,
    RBase = 255
};

std::uint32_t read_word(std::span<const std::uint8_t> input, std::size_t offset) {
    std::uint32_t value = 0;
    std::memcpy(&value, input.data() + offset, sizeof(value));
    return value;
}

const LoadSegmentPlan *find_segment(std::span<const LoadSegmentPlan> segments,
    std::uint32_t program_index) {
    for (const auto &segment : segments) {
        if (segment.program_index == program_index) {
            return &segment;
        }
    }
    return nullptr;
}

bool patch_address(const LoadSegmentPlan &segment, std::uint32_t offset,
    std::size_t width, std::uint32_t &address, std::string &error) {
    if (offset > segment.memory_size || width > segment.memory_size - offset) {
        error = "A relocation patch extends outside its target segment.";
        return false;
    }
    const auto address64 = static_cast<std::uint64_t>(segment.virtual_address) + offset;
    if (address64 > std::numeric_limits<std::uint32_t>::max()) {
        error = "A relocation patch address overflows guest address space.";
        return false;
    }
    address = static_cast<std::uint32_t>(address64);
    return true;
}

bool read_guest_word(GuestMemory &memory, std::uint32_t address,
    std::uint32_t &value, std::string &error) {
    std::array<std::uint8_t, sizeof(value)> bytes{};
    if (!memory.read(address, bytes, error)) {
        return false;
    }
    std::memcpy(&value, bytes.data(), sizeof(value));
    return true;
}

bool write_guest_word(GuestMemory &memory, std::uint32_t address,
    std::uint32_t value, std::string &error) {
    std::array<std::uint8_t, sizeof(value)> bytes{};
    std::memcpy(bytes.data(), &value, sizeof(value));
    if (!memory.write(address, bytes, error)) {
        return false;
    }
    std::uint32_t observed = 0;
    if (!read_guest_word(memory, address, observed, error) || observed != value) {
        error = "A relocation write failed readback verification.";
        return false;
    }
    return true;
}

bool write_guest_byte(GuestMemory &memory, std::uint32_t address,
    std::uint8_t value, std::string &error) {
    const std::array bytes{value};
    if (!memory.write(address, bytes, error)) {
        return false;
    }
    std::array<std::uint8_t, 1> observed{};
    if (!memory.read(address, observed, error) || observed[0] != value) {
        error = "A byte relocation write failed readback verification.";
        return false;
    }
    return true;
}

bool apply_code(GuestMemory &memory, std::uint32_t code, std::uint32_t symbol,
    std::uint32_t addend, std::uint32_t address, std::size_t &patch_count,
    std::string &error) {
    std::uint32_t value = 0;
    switch (code) {
    case None:
    case V4BX:
    case RBase:
        return true;
    case Abs32:
    case Target1:
        value = symbol + addend;
        break;
    case Rel32:
    case Target2:
        value = symbol + addend - address;
        break;
    case Prel31:
        value = (symbol + addend - address) & static_cast<std::uint32_t>(INT32_MAX);
        break;
    case Abs8:
        if (!write_guest_byte(memory, address,
                static_cast<std::uint8_t>(symbol + addend), error)) {
            return false;
        }
        ++patch_count;
        return true;
    case Call:
    case Jump24:
        if (!read_guest_word(memory, address, value, error)) {
            return false;
        }
        value = (value & 0xFF000000u) |
            (((symbol + addend - address) >> 2) & 0x00FFFFFFu);
        break;
    case ThumbCall: {
        if (!read_guest_word(memory, address, value, error)) {
            return false;
        }
        const auto branch = symbol + addend - address;
        std::uint16_t upper = static_cast<std::uint16_t>(value);
        std::uint16_t lower = static_cast<std::uint16_t>(value >> 16);
        const auto sign = (branch >> 24) & 1u;
        const auto j2 = sign ^ ((~branch >> 22) & 1u);
        const auto j1 = sign ^ ((~branch >> 23) & 1u);
        upper = static_cast<std::uint16_t>((upper & ~0x07FFu) |
            ((branch >> 12) & 0x03FFu) | (sign << 10));
        lower = static_cast<std::uint16_t>((lower & ~0x2FFFu) |
            ((branch >> 1) & 0x07FFu) | (j2 << 11) | (j1 << 13));
        value = static_cast<std::uint32_t>(upper) |
            (static_cast<std::uint32_t>(lower) << 16);
        break;
    }
    case MovwAbsNc:
    case MovtAbs: {
        if (!read_guest_word(memory, address, value, error)) {
            return false;
        }
        auto immediate = symbol + addend;
        if (code == MovtAbs) {
            immediate >>= 16;
        }
        value = (value & ~(0x000F0FFFu)) | (immediate & 0x0FFFu) |
            ((immediate & 0xF000u) << 4);
        break;
    }
    case ThumbMovwAbsNc:
    case ThumbMovtAbs: {
        if (!read_guest_word(memory, address, value, error)) {
            return false;
        }
        auto immediate = symbol + addend;
        if (code == ThumbMovtAbs) {
            immediate >>= 16;
        }
        std::uint16_t upper = static_cast<std::uint16_t>(value);
        std::uint16_t lower = static_cast<std::uint16_t>(value >> 16);
        upper = static_cast<std::uint16_t>((upper & ~0x040Fu) |
            ((immediate >> 12) & 0xFu) | (((immediate >> 11) & 1u) << 10));
        lower = static_cast<std::uint16_t>((lower & ~0x70FFu) |
            (immediate & 0xFFu) | (((immediate >> 8) & 0x7u) << 12));
        value = static_cast<std::uint32_t>(upper) |
            (static_cast<std::uint32_t>(lower) << 16);
        break;
    }
    default:
        error = "Unsupported Vita relocation code " + std::to_string(code) + ".";
        return false;
    }

    if (!write_guest_word(memory, address, value, error)) {
        return false;
    }
    ++patch_count;
    return true;
}

} // namespace

RelocationApplyResult apply_relocations(std::span<const std::uint8_t> entries,
    std::span<const LoadSegmentPlan> segments,
    GuestMemory &memory) {
    RelocationApplyResult result;
    if (entries.empty()) {
        result.success = true;
        result.detail = "No relocation entries.";
        return result;
    }

    const LoadSegmentPlan *state_patch_segment = nullptr;
    std::uint32_t state_offset = 0;
    std::uint32_t state_symbol = 0;
    std::uint32_t state_addend = 0;
    std::uint32_t state_code = 0;
    std::uint32_t state_code2 = 0;
    std::size_t cursor = 0;
    std::string error;

    const auto require_segment = [&](std::uint32_t index, const char *role) {
        const auto *segment = find_segment(segments, index);
        if (!segment) {
            error = std::string("A relocation references a missing ") + role + " segment.";
        }
        return segment;
    };

    const auto apply_at_state_offset = [&](std::uint32_t code, std::uint32_t extra_offset = 0) {
        if (!state_patch_segment) {
            error = "A compact relocation entry appears before relocation state is initialized.";
            return false;
        }
        const auto offset64 = static_cast<std::uint64_t>(state_offset) + extra_offset;
        if (offset64 > std::numeric_limits<std::uint32_t>::max()) {
            error = "A compact relocation offset overflows.";
            return false;
        }
        std::uint32_t address = 0;
        if (!patch_address(*state_patch_segment, static_cast<std::uint32_t>(offset64),
                code == Abs8 ? 1 : 4, address, error)) {
            return false;
        }
        return apply_code(memory, code, state_symbol, state_addend, address,
            result.patched_value_count, error);
    };

    while (cursor < entries.size()) {
        const auto format = entries[cursor] & 0x0Fu;
        std::size_t entry_size = 0;
        switch (format) {
        case 0:
            entry_size = 12;
            break;
        case 1:
        case 2:
        case 3:
            entry_size = 8;
            break;
        case 4:
        case 5:
        case 6:
        case 7:
        case 8:
        case 9:
            entry_size = 4;
            break;
        default:
            result.detail = "Unknown relocation entry format " + std::to_string(format) + ".";
            return result;
        }
        if (entry_size > entries.size() - cursor) {
            result.detail = "A relocation entry is truncated.";
            return result;
        }

        const auto word0 = read_word(entries, cursor);
        const auto word1 = entry_size >= 8 ? read_word(entries, cursor + 4) : 0;
        const auto word2 = entry_size >= 12 ? read_word(entries, cursor + 8) : 0;

        if (format == 0) {
            const auto symbol_index = (word0 >> 4) & 0xFu;
            const auto code = (word0 >> 8) & 0xFFu;
            const auto patch_index = (word0 >> 16) & 0xFu;
            const auto code2 = (word0 >> 20) & 0xFFu;
            const auto dist2 = (word0 >> 28) & 0xFu;
            const auto *patch = require_segment(patch_index, "patch");
            const auto *symbol = symbol_index == 0xFu ? nullptr :
                require_segment(symbol_index, "symbol");
            if (!patch || (symbol_index != 0xFu && !symbol)) {
                result.detail = error;
                return result;
            }
            state_patch_segment = patch;
            state_offset = word2;
            state_symbol = symbol ? symbol->virtual_address : 0;
            state_addend = word1;
            state_code = code;
            state_code2 = code2;
            if (!apply_at_state_offset(code) ||
                (code2 != 0 && !apply_at_state_offset(code2, dist2 * 2))) {
                result.detail = error;
                return result;
            }
        } else if (format == 1) {
            const auto symbol_index = (word0 >> 4) & 0xFu;
            const auto code = (word0 >> 8) & 0xFFu;
            const auto patch_index = (word0 >> 16) & 0xFu;
            const auto offset = ((word0 >> 20) & 0xFFFu) | ((word1 & 0x3FFu) << 12);
            const auto *patch = require_segment(patch_index, "patch");
            const auto *symbol = symbol_index == 0xFu ? nullptr :
                require_segment(symbol_index, "symbol");
            if (!patch || (symbol_index != 0xFu && !symbol)) {
                result.detail = error;
                return result;
            }
            state_patch_segment = patch;
            state_offset = offset;
            state_symbol = symbol ? symbol->virtual_address : 0;
            state_addend = word1 >> 10;
            state_code = code;
            state_code2 = 0;
            if (!apply_at_state_offset(code)) {
                result.detail = error;
                return result;
            }
        } else if (format == 2) {
            if (!state_patch_segment) {
                result.detail = "Format-2 relocation appears before patch state is initialized.";
                return result;
            }
            const auto symbol_index = (word0 >> 4) & 0xFu;
            const auto *symbol = symbol_index == 0xFu ? nullptr :
                require_segment(symbol_index, "symbol");
            if (symbol_index != 0xFu && !symbol) {
                result.detail = error;
                return result;
            }
            state_offset += word0 >> 16;
            state_symbol = symbol ? symbol->virtual_address : 0;
            state_addend = word1;
            state_code = (word0 >> 8) & 0xFFu;
            state_code2 = 0;
            if (!apply_at_state_offset(state_code)) {
                result.detail = error;
                return result;
            }
        } else if (format == 3) {
            if (!state_patch_segment) {
                result.detail = "Format-3 relocation appears before patch state is initialized.";
                return result;
            }
            const auto symbol_index = (word0 >> 4) & 0xFu;
            const auto *symbol = symbol_index == 0xFu ? nullptr :
                require_segment(symbol_index, "symbol");
            if (symbol_index != 0xFu && !symbol) {
                result.detail = error;
                return result;
            }
            const auto thumb = (word0 >> 8) & 1u;
            state_offset += (word0 >> 9) & 0x3FFFFu;
            state_symbol = symbol ? symbol->virtual_address : 0;
            state_addend = word1 & 0x3FFFFFu;
            state_code = thumb ? ThumbMovwAbsNc : MovwAbsNc;
            state_code2 = thumb ? ThumbMovtAbs : MovtAbs;
            const auto dist2 = word0 >> 27;
            if (!apply_at_state_offset(state_code) ||
                !apply_at_state_offset(state_code2, dist2)) {
                result.detail = error;
                return result;
            }
        } else if (format == 4) {
            state_offset += (word0 >> 4) & 0x7FFFFFu;
            const auto dist2 = word0 >> 27;
            if (!apply_at_state_offset(state_code) ||
                !apply_at_state_offset(state_code2, dist2)) {
                result.detail = error;
                return result;
            }
        } else if (format == 5) {
            state_offset += (word0 >> 4) & 0x1FFu;
            if (!apply_at_state_offset(state_code) ||
                !apply_at_state_offset(state_code2, (word0 >> 13) & 0x1Fu)) {
                result.detail = error;
                return result;
            }
            state_offset += (word0 >> 18) & 0x1FFu;
            if (!apply_at_state_offset(state_code) ||
                !apply_at_state_offset(state_code2, word0 >> 27)) {
                result.detail = error;
                return result;
            }
        } else if (format == 6) {
            state_offset += word0 >> 4;
            std::uint32_t address = 0;
            if (!state_patch_segment ||
                !patch_address(*state_patch_segment, state_offset, 4, address, error)) {
                result.detail = error.empty() ? "Format-6 relocation state is invalid." : error;
                return result;
            }
            std::uint32_t original = 0;
            if (!read_guest_word(memory, address, original, error)) {
                result.detail = error;
                return result;
            }
            const LoadSegmentPlan *original_segment = nullptr;
            for (const auto &candidate : segments) {
                const auto end = static_cast<std::uint64_t>(candidate.virtual_address) + candidate.memory_size;
                if (original >= candidate.virtual_address && original < end) {
                    original_segment = &candidate;
                    break;
                }
            }
            if (!original_segment) {
                result.detail = "Format-6 relocation value does not point into a loaded segment.";
                return result;
            }
            state_symbol = original_segment->virtual_address;
            state_addend = original - original_segment->virtual_address;
            state_code = Abs32;
            state_code2 = 0;
            if (!apply_at_state_offset(state_code)) {
                result.detail = error;
                return result;
            }
        } else {
            const auto bits = format == 7 ? 7u : (format == 8 ? 4u : 2u);
            const auto mask = (1u << bits) - 1u;
            auto offsets = word0 >> 4;
            do {
                state_offset += (offsets & mask) * sizeof(std::uint32_t);
                std::uint32_t address = 0;
                if (!state_patch_segment ||
                    !patch_address(*state_patch_segment, state_offset, 4, address, error)) {
                    result.detail = error.empty() ? "Compact relocation state is invalid." : error;
                    return result;
                }
                std::uint32_t original = 0;
                if (!read_guest_word(memory, address, original, error)) {
                    result.detail = error;
                    return result;
                }
                const LoadSegmentPlan *original_segment = nullptr;
                for (const auto &candidate : segments) {
                    const auto end = static_cast<std::uint64_t>(candidate.virtual_address) + candidate.memory_size;
                    if (original >= candidate.virtual_address && original < end) {
                        original_segment = &candidate;
                        break;
                    }
                }
                if (!original_segment) {
                    result.detail = "Compact relocation value does not point into a loaded segment.";
                    return result;
                }
                state_symbol = original_segment->virtual_address;
                state_addend = original - original_segment->virtual_address;
                state_code = Abs32;
                state_code2 = 0;
                if (!apply_at_state_offset(state_code)) {
                    result.detail = error;
                    return result;
                }
                offsets >>= bits;
            } while (offsets != 0);
        }

        ++result.entry_count;
        cursor += entry_size;
    }

    result.success = true;
    std::ostringstream detail;
    detail << "Applied " << result.entry_count << " relocation entries with "
           << result.patched_value_count << " verified guest-memory writes.";
    result.detail = detail.str();
    return result;
}

} // namespace vita3k::ios
