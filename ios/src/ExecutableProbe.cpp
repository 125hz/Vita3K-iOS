#include <vita3k_ios/ExecutableProbe.h>

#include <util/elf.h>

#include <array>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <limits>
#include <sstream>
#include <utility>
#include <vector>

namespace vita3k::ios {
namespace {

constexpr std::uint32_t SCE_MAGIC = 0x00454353u;
constexpr std::uint32_t PSF_MAGIC = 0x46535000u;
constexpr std::uint32_t ZIP_LOCAL_MAGIC = 0x04034B50u;
constexpr std::uint64_t SELF_ELF_OFFSET_FIELD = 64;

template <typename T>
bool read_value(std::ifstream &stream, std::uint64_t offset, T &value) {
    if (offset > static_cast<std::uint64_t>(std::numeric_limits<std::streamoff>::max())) {
        return false;
    }
    stream.seekg(static_cast<std::streamoff>(offset), std::ios::beg);
    stream.read(reinterpret_cast<char *>(&value), sizeof(value));
    return stream.good();
}

ExecutableProbeResult validate_elf(std::ifstream &stream, std::uint64_t file_size, std::uint64_t offset, std::string kind) {
    ExecutableProbeResult result{
        .kind = std::move(kind),
        .recognized = true
    };

    if (offset > file_size || sizeof(Elf32_Ehdr) > file_size - offset) {
        result.detail = "ELF header is truncated or points outside the file.";
        return result;
    }

    Elf32_Ehdr header{};
    if (!read_value(stream, offset, header)) {
        result.detail = "Could not read the ELF header.";
        return result;
    }
    if (!EHDR_HAS_VALID_MAGIC(header)) {
        result.detail = "Embedded ELF magic is invalid.";
        return result;
    }
    if (header.e_ident[EI_CLASS] != ELFCLASS32 || header.e_ident[EI_DATA] != ELFDATA2LSB ||
        header.e_ident[EI_VERSION] != EV_CURRENT) {
        result.detail = "ELF must be 32-bit, little-endian, and version 1.";
        return result;
    }
    if (header.e_machine != EM_ARM) {
        result.detail = "ELF target is not ARM.";
        return result;
    }
    if (header.e_type != ET_SCE_EXEC && header.e_type != ET_SCE_RELEXEC) {
        result.detail = "ELF type is not a supported Vita executable type.";
        return result;
    }
    if (header.e_ehsize != sizeof(Elf32_Ehdr)) {
        result.detail = "ELF header size is inconsistent.";
        return result;
    }

    const std::uint64_t program_table_offset = offset + header.e_phoff;
    const std::uint64_t program_table_size = static_cast<std::uint64_t>(header.e_phentsize) * header.e_phnum;
    if (program_table_offset > file_size || program_table_size > file_size - program_table_offset) {
        result.detail = "ELF program-header table extends outside the file.";
        return result;
    }

    result.structurally_valid = true;
    std::ostringstream detail;
    detail << "ARM32 Vita ELF; " << header.e_phnum << " program headers; entry 0x"
           << std::hex << header.e_entry << ".";
    result.detail = detail.str();
    return result;
}

} // namespace

ExecutableProbeResult probe_artifact(const std::filesystem::path &path) {
    std::error_code size_error;
    const auto file_size = std::filesystem::file_size(path, size_error);
    if (size_error) {
        return { .kind = "Unreadable", .detail = size_error.message() };
    }

    std::ifstream stream(path, std::ios::binary);
    if (!stream) {
        return { .kind = "Unreadable", .detail = "The file could not be opened." };
    }

    std::uint32_t magic = 0;
    if (!read_value(stream, 0, magic)) {
        return { .kind = "Unknown", .detail = "The file is too small to identify." };
    }

    if (magic == SCE_MAGIC) {
        std::uint32_t version = 0;
        std::uint64_t elf_offset = 0;
        if (!read_value(stream, 4, version) || !read_value(stream, SELF_ELF_OFFSET_FIELD, elf_offset)) {
            return { .kind = "Vita SELF", .recognized = true, .detail = "SELF header is truncated." };
        }
        if (version != 3) {
            return { .kind = "Vita SELF", .recognized = true, .detail = "Only SELF version 3 is supported." };
        }
        return validate_elf(stream, file_size, elf_offset, "Vita SELF");
    }

    std::array<std::uint8_t, 4> elf_magic{};
    std::memcpy(elf_magic.data(), &magic, elf_magic.size());
    if (elf_magic[0] == ELFMAG0 && elf_magic[1] == ELFMAG1 && elf_magic[2] == ELFMAG2 && elf_magic[3] == ELFMAG3) {
        return validate_elf(stream, file_size, 0, "Vita ELF");
    }

    if (magic == ZIP_LOCAL_MAGIC) {
        return {
            .kind = "VPK/ZIP",
            .recognized = true,
            .structurally_valid = file_size >= 30,
            .detail = file_size >= 30 ? "ZIP container recognized; VPK extraction is not connected yet."
                                      : "ZIP header is truncated."
        };
    }

    if (magic == PSF_MAGIC) {
        return {
            .kind = "PARAM.SFO",
            .recognized = true,
            .structurally_valid = file_size >= 20,
            .detail = file_size >= 20 ? "PSF metadata recognized; metadata parsing is not connected yet."
                                      : "PSF header is truncated."
        };
    }

    return { .kind = "Unknown", .detail = "No supported Vita container or executable header was found." };
}

} // namespace vita3k::ios
