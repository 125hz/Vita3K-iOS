#include <vita3k_ios/ExecutableLoader.h>

#include <vita3k_ios/ExecutableProbe.h>
#include <vita3k_ios/GuestMemory.h>
#include <vita3k_ios/ImportBinder.h>
#include <vita3k_ios/ModuleTableParser.h>
#include <vita3k_ios/RelocationEngine.h>

#include <miniz.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <limits>
#include <numeric>
#include <sstream>
#include <vector>

namespace vita3k::ios {
namespace {

constexpr std::uint16_t et_sce_exec = 0xFE00;
constexpr std::uint16_t et_sce_relexec = 0xFE04;
constexpr std::uint32_t diagnostic_stack_address = 0x7FF00000;
constexpr std::size_t module_info_size = 0x5C;
constexpr std::size_t module_name_offset = 4;
constexpr std::size_t module_name_size = 27;
constexpr std::size_t module_nid_offset = 0x34;
constexpr std::size_t module_start_offset = 0x44;
constexpr std::uint32_t guest_flag_execute = 1;
constexpr std::uint32_t guest_flag_write = 2;
constexpr std::uint32_t guest_flag_read = 4;

bool read_file_range(std::ifstream &stream, std::uint64_t offset,
    std::span<std::uint8_t> output) {
    if (output.empty()) {
        return true;
    }
    stream.clear();
    stream.seekg(static_cast<std::streamoff>(offset), std::ios::beg);
    stream.read(reinterpret_cast<char *>(output.data()), static_cast<std::streamsize>(output.size()));
    return stream.gcount() == static_cast<std::streamsize>(output.size());
}

bool read_executable_payload(std::ifstream &stream, std::uint32_t offset,
    std::uint32_t stored_size, std::uint32_t output_size, bool compressed,
    std::vector<std::uint8_t> &output) {
    const auto actual_stored_size = stored_size == 0 ? output_size : stored_size;
    std::vector<std::uint8_t> stored(actual_stored_size);
    if (!read_file_range(stream, offset, stored))
        return false;
    output.resize(output_size);
    if (!compressed) {
        if (actual_stored_size != output_size)
            return false;
        output = std::move(stored);
        return true;
    }
    mz_ulong decompressed_size = output_size;
    const auto decompressed = mz_uncompress(output.data(), &decompressed_size,
        stored.data(), static_cast<mz_ulong>(stored.size()));
    return decompressed == MZ_OK && decompressed_size == output_size;
}

std::string sanitize_module_name(std::span<const std::uint8_t> bytes) {
    const auto terminator = std::find(bytes.begin(), bytes.end(), 0);
    std::string name;
    name.reserve(static_cast<std::size_t>(terminator - bytes.begin()));
    for (auto iterator = bytes.begin(); iterator != terminator; ++iterator) {
        const auto value = static_cast<unsigned char>(*iterator);
        name.push_back(std::isprint(value) != 0 ? static_cast<char>(value) : '?');
    }
    return name.empty() ? "<unnamed>" : name;
}

} // namespace

PlainElfLoadResult load_plain_elf(const std::filesystem::path &path,
    const ExecutableProbeResult &probe,
    GuestMemory &memory) {
    PlainElfLoadResult result{
        .attempted = true,
        .relocation_segment_count = probe.relocation_segments.size()
    };
    result.relocation_bytes = std::accumulate(probe.relocation_segments.begin(),
        probe.relocation_segments.end(), std::uint64_t{0},
        [](std::uint64_t total, const RelocationSegmentPlan &segment) {
            return total + segment.file_size;
        });

    if (!probe.structurally_valid ||
        (probe.kind != "Vita ELF" && probe.kind != "Vita SELF")) {
        result.detail = "Only a structurally valid Vita ELF/SELF can be mapped.";
        return result;
    }
    if (probe.kind == "Vita SELF" && !probe.self_segments_plain) {
        result.detail = "The SELF contains encrypted segments and must be decrypted before mapping.";
        return result;
    }
    if (probe.executable_type != et_sce_exec &&
        probe.executable_type != et_sce_relexec) {
        result.detail = "The Vita ELF type is not supported by the diagnostic loader.";
        return result;
    }
    if (memory.mapped_segment_count() != 0) {
        result.detail = "Guest memory already contains a loaded executable.";
        return result;
    }

    std::ifstream stream(path, std::ios::binary);
    if (!stream) {
        result.detail = "The plain ELF could not be opened for segment loading.";
        return result;
    }

    std::vector<std::vector<std::uint8_t>> payloads(probe.load_segments.size());
    std::vector<GuestSegmentMapping> mappings;
    mappings.reserve(probe.load_segments.size() + 1);
    bool has_writable_stack = false;
    for (std::size_t index = 0; index < probe.load_segments.size(); ++index) {
        const auto &segment = probe.load_segments[index];
        if (!read_executable_payload(stream, segment.file_offset, segment.stored_size,
                segment.file_size, segment.compressed, payloads[index])) {
            result.detail = "A validated executable segment could not be read/decompressed.";
            return result;
        }
        mappings.push_back({
            .guest_address = segment.virtual_address,
            .file_data = payloads[index],
            .memory_size = segment.memory_size,
            .guest_flags = segment.flags
        });
        result.file_bytes += segment.file_size;
        result.memory_bytes += segment.memory_size;
        has_writable_stack = has_writable_stack ||
            ((segment.flags & guest_flag_write) != 0 && segment.memory_size >= 16);
    }

    const bool uses_diagnostic_stack = !has_writable_stack;
    if (uses_diagnostic_stack) {
        const auto stack_size = static_cast<std::uint32_t>(memory.host_page_size());
        mappings.push_back({
            .guest_address = diagnostic_stack_address,
            .file_data = {},
            .memory_size = stack_size,
            .guest_flags = guest_flag_read | guest_flag_write
        });
    }

    const auto page_size = static_cast<std::uint64_t>(memory.host_page_size());
    for (std::size_t left = 0; left < probe.load_segments.size(); ++left) {
        const auto &a = probe.load_segments[left];
        const auto a_start = (static_cast<std::uint64_t>(a.virtual_address) / page_size) * page_size;
        const auto a_end = ((static_cast<std::uint64_t>(a.virtual_address) + a.memory_size +
                                page_size - 1) /
            page_size) * page_size;
        for (std::size_t right = left + 1; right < probe.load_segments.size(); ++right) {
            const auto &b = probe.load_segments[right];
            const auto b_start = (static_cast<std::uint64_t>(b.virtual_address) / page_size) * page_size;
            const auto b_end = ((static_cast<std::uint64_t>(b.virtual_address) + b.memory_size +
                                    page_size - 1) /
                page_size) * page_size;
            if (a_start < b_end && b_start < a_end) {
                ++result.shared_page_pairs;
            }
        }
    }

    std::string memory_error;
    if (!memory.map_segments(mappings, memory_error)) {
        result.detail = "Guest segment mapping failed: " + memory_error;
        return result;
    }

    for (std::size_t index = 0; index < probe.load_segments.size(); ++index) {
        if (payloads[index].empty()) {
            continue;
        }
        std::vector<std::uint8_t> observed(payloads[index].size());
        if (!memory.read(probe.load_segments[index].virtual_address, observed, memory_error) ||
            observed != payloads[index]) {
            std::string unmap_error;
            (void)memory.unmap_all_segments(unmap_error);
            result.detail = "Mapped ELF segment readback failed: " + memory_error;
            return result;
        }
    }

    for (const auto &relocation_segment : probe.relocation_segments) {
        std::vector<std::uint8_t> relocation_bytes;
        if (!read_executable_payload(stream, relocation_segment.file_offset,
                relocation_segment.stored_size, relocation_segment.file_size,
                relocation_segment.compressed, relocation_bytes)) {
            std::string unmap_error;
            (void)memory.unmap_all_segments(unmap_error);
            result.detail = "A validated relocation segment could not be read from disk.";
            return result;
        }
        const auto relocation = apply_relocations(relocation_bytes, probe.load_segments, memory);
        if (!relocation.success) {
            std::string unmap_error;
            (void)memory.unmap_all_segments(unmap_error);
            result.detail = "Relocation application failed: " + relocation.detail;
            return result;
        }
        result.relocation_entry_count += relocation.entry_count;
        result.relocation_patch_count += relocation.patched_value_count;
    }
    result.relocations_applied = true;

    const auto module_segment = std::find_if(probe.load_segments.begin(), probe.load_segments.end(),
        [&probe](const LoadSegmentPlan &segment) {
            return segment.program_index == probe.module_info_segment_index;
        });
    if (module_segment == probe.load_segments.end() ||
        probe.module_info_offset > module_segment->memory_size ||
        module_info_size > module_segment->memory_size - probe.module_info_offset) {
        std::string unmap_error;
        (void)memory.unmap_all_segments(unmap_error);
        result.detail = "The ELF module-info locator does not point inside a loaded segment.";
        return result;
    }

    const auto module_address_64 = static_cast<std::uint64_t>(module_segment->virtual_address) +
        probe.module_info_offset;
    if (module_address_64 > std::numeric_limits<std::uint32_t>::max()) {
        std::string unmap_error;
        (void)memory.unmap_all_segments(unmap_error);
        result.detail = "The ELF module-info address overflows guest address space.";
        return result;
    }

    std::array<std::uint8_t, module_info_size> module_info{};
    if (!memory.read(static_cast<std::uint32_t>(module_address_64), module_info, memory_error)) {
        std::string unmap_error;
        (void)memory.unmap_all_segments(unmap_error);
        result.detail = "The ELF module-info header could not be read: " + memory_error;
        return result;
    }

    result.module_name = sanitize_module_name(std::span(module_info).subspan(
        module_name_offset, module_name_size));
    std::memcpy(&result.module_nid, module_info.data() + module_nid_offset,
        sizeof(result.module_nid));
    std::memcpy(&result.module_start_offset, module_info.data() + module_start_offset,
        sizeof(result.module_start_offset));
    result.module_info_valid = true;

    if (result.module_start_offset != 0 && result.module_start_offset != 0xFFFFFFFFu) {
        const auto start_address_64 = static_cast<std::uint64_t>(module_segment->virtual_address) +
            result.module_start_offset;
        if (start_address_64 > std::numeric_limits<std::uint32_t>::max()) {
            std::string unmap_error;
            (void)memory.unmap_all_segments(unmap_error);
            result.detail = "The module_start address overflows guest address space.";
            return result;
        }
        result.module_start_address = static_cast<std::uint32_t>(start_address_64);
        const auto code_address = result.module_start_address & ~1u;
        const auto executable_segment = std::find_if(probe.load_segments.begin(),
            probe.load_segments.end(), [code_address](const LoadSegmentPlan &segment) {
                const auto start = static_cast<std::uint64_t>(segment.virtual_address);
                const auto end = start + segment.memory_size;
                return (segment.flags & guest_flag_execute) != 0 && code_address >= start &&
                    static_cast<std::uint64_t>(code_address) + sizeof(std::uint32_t) <= end;
            });
        if (executable_segment == probe.load_segments.end()) {
            std::string unmap_error;
            (void)memory.unmap_all_segments(unmap_error);
            result.detail = "The module_start address is outside executable guest memory.";
            return result;
        }
        result.module_start_valid = true;
    }

    for (const auto &segment : probe.load_segments) {
        if ((segment.flags & guest_flag_write) == 0 || segment.memory_size < 16) {
            continue;
        }
        const auto end = static_cast<std::uint64_t>(segment.virtual_address) + segment.memory_size;
        if (end <= std::numeric_limits<std::uint32_t>::max()) {
            result.temporary_stack_pointer = static_cast<std::uint32_t>(end) & ~7u;
        }
    }
    if (uses_diagnostic_stack) {
        result.temporary_stack_pointer = diagnostic_stack_address +
            static_cast<std::uint32_t>(memory.host_page_size());
    }

    const auto module_tables = parse_module_tables(memory,
        module_segment->virtual_address, module_segment->memory_size, module_info);
    if (!module_tables.success) {
        std::string unmap_error;
        (void)memory.unmap_all_segments(unmap_error);
        result.detail = "Module-table parsing failed: " + module_tables.detail;
        return result;
    }
    const auto import_binding = bind_import_stubs(memory,
        module_tables.imported_function_stubs);
    if (!import_binding.success) {
        std::string unmap_error;
        (void)memory.unmap_all_segments(unmap_error);
        result.detail = "Import-stub binding failed: " + import_binding.detail;
        return result;
    }
    result.module_tables_parsed = true;
    result.import_stubs_bound = true;
    result.export_library_count = module_tables.export_library_count;
    result.import_library_count = module_tables.import_library_count;
    result.exported_nid_count = module_tables.exported_nids.size();
    result.imported_nid_count = module_tables.imported_nids.size();
    result.bound_import_stub_count = import_binding.bound_function_count;
    result.imported_nids = module_tables.imported_nids;
    result.loaded = true;

    std::ostringstream detail;
    detail << "Mapped " << probe.load_segments.size() <<
           (probe.executable_type == et_sce_relexec ? " preferred-address relocatable" : " fixed") <<
           " segment" << (probe.load_segments.size() == 1 ? "" : "s")
           << (probe.kind == "Vita SELF" ? " from SELF" : "") << " ("
           << result.file_bytes << " file bytes, " << result.memory_bytes
           << " guest bytes); readback passed; module " << result.module_name
           << " NID 0x" << std::hex << std::uppercase << std::setw(8)
           << std::setfill('0') << result.module_nid << std::dec << "; "
           << result.relocation_entry_count << " relocation entries/"
           << result.relocation_patch_count << " verified patches applied; "
           << result.export_library_count << " export librar"
           << (result.export_library_count == 1 ? "y/" : "ies/")
           << result.exported_nid_count << " NID"
           << (result.exported_nid_count == 1 ? "" : "s") << "; "
           << result.import_library_count << " import librar"
           << (result.import_library_count == 1 ? "y/" : "ies/")
           << result.imported_nid_count << " NID"
           << (result.imported_nid_count == 1 ? "" : "s") << "; "
           << result.bound_import_stub_count << " function stub"
           << (result.bound_import_stub_count == 1 ? "" : "s") << " rebound";
    if (result.module_start_valid) {
        detail << "; module_start 0x" << std::hex << std::uppercase << std::setw(8)
               << std::setfill('0') << result.module_start_address << std::dec;
        if (result.temporary_stack_pointer != 0) {
            detail << " with temporary SP 0x" << std::hex << std::uppercase
                   << std::setw(8) << std::setfill('0') << result.temporary_stack_pointer
                   << std::dec;
        }
    } else {
        detail << "; no module_start";
    }
    detail << ".";
    result.detail = detail.str();
    return result;
}

} // namespace vita3k::ios
