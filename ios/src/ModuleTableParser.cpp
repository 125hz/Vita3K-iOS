#include <vita3k_ios/ModuleTableParser.h>

#include <vita3k_ios/GuestMemory.h>

#include <array>
#include <cstring>
#include <limits>
#include <sstream>

namespace vita3k::ios {
namespace {

constexpr std::size_t module_info_size = 0x5C;
constexpr std::size_t export_top_offset = 0x24;
constexpr std::size_t export_end_offset = 0x28;
constexpr std::size_t import_top_offset = 0x2C;
constexpr std::size_t import_end_offset = 0x30;
constexpr std::size_t export_record_size = 0x20;
constexpr std::size_t import_record_size = 0x34;
constexpr std::size_t short_import_record_size = 0x24;
constexpr std::uint64_t maximum_symbols = 100000;

std::uint16_t read_u16(std::span<const std::uint8_t> bytes, std::size_t offset) {
    std::uint16_t value = 0;
    std::memcpy(&value, bytes.data() + offset, sizeof(value));
    return value;
}

std::uint32_t read_u32(std::span<const std::uint8_t> bytes, std::size_t offset) {
    std::uint32_t value = 0;
    std::memcpy(&value, bytes.data() + offset, sizeof(value));
    return value;
}

bool checked_guest_address(std::uint32_t base, std::uint32_t offset,
    std::uint32_t &address, std::string &error) {
    const auto address64 = static_cast<std::uint64_t>(base) + offset;
    if (address64 > std::numeric_limits<std::uint32_t>::max()) {
        error = "A module-table address overflows guest address space.";
        return false;
    }
    address = static_cast<std::uint32_t>(address64);
    return true;
}

bool read_nid_table(GuestMemory &memory, std::uint32_t address, std::uint64_t count,
    std::vector<std::uint32_t> &output, std::string &error) {
    if (count == 0) {
        return true;
    }
    if (address == 0 || count > maximum_symbols ||
        count > maximum_symbols - output.size()) {
        error = "A module NID table has an invalid pointer or symbol count.";
        return false;
    }
    const auto bytes64 = count * sizeof(std::uint32_t);
    if (bytes64 > std::numeric_limits<std::size_t>::max()) {
        error = "A module NID table is too large for this host.";
        return false;
    }
    std::vector<std::uint8_t> bytes(static_cast<std::size_t>(bytes64));
    if (!memory.read(address, bytes, error)) {
        error = "Could not read a module NID table: " + error;
        return false;
    }
    const auto old_size = output.size();
    output.resize(old_size + static_cast<std::size_t>(count));
    std::memcpy(output.data() + old_size, bytes.data(), bytes.size());
    return true;
}

} // namespace

ModuleTableSummary parse_module_tables(GuestMemory &memory,
    std::uint32_t module_segment_base,
    std::uint32_t module_segment_size,
    std::span<const std::uint8_t> module_info) {
    ModuleTableSummary result;
    if (module_info.size() < module_info_size) {
        result.detail = "The Vita module-info header is truncated.";
        return result;
    }

    const auto export_top = read_u32(module_info, export_top_offset);
    const auto export_end = read_u32(module_info, export_end_offset);
    const auto import_top = read_u32(module_info, import_top_offset);
    const auto import_end = read_u32(module_info, import_end_offset);
    if (export_top > export_end || export_end > module_segment_size ||
        import_top > import_end || import_end > module_segment_size) {
        result.detail = "Module import/export table bounds leave the module-info segment.";
        return result;
    }

    std::string error;
    for (std::uint32_t cursor = export_top; cursor < export_end;) {
        if (export_end - cursor < export_record_size) {
            result.detail = "A module export record is truncated.";
            return result;
        }
        std::uint32_t address = 0;
        if (!checked_guest_address(module_segment_base, cursor, address, error)) {
            result.detail = error;
            return result;
        }
        std::array<std::uint8_t, export_record_size> record{};
        if (!memory.read(address, record, error)) {
            result.detail = "Could not read a module export record: " + error;
            return result;
        }
        if (read_u16(record, 0) != export_record_size) {
            result.detail = "A module export record has an unsupported size.";
            return result;
        }

        const auto function_count = read_u16(record, 6);
        const auto variable_count = read_u32(record, 8);
        const auto tls_count = read_u32(record, 12);
        const auto symbol_count = static_cast<std::uint64_t>(function_count) +
            variable_count + tls_count;
        if (symbol_count > maximum_symbols ||
            !read_nid_table(memory, read_u32(record, 24), symbol_count,
                result.exported_nids, error)) {
            result.detail = error.empty() ? "A module export symbol count is excessive." : error;
            return result;
        }
        ++result.export_library_count;
        result.exported_function_count += function_count;
        result.exported_variable_count += variable_count + tls_count;
        cursor += export_record_size;
    }

    for (std::uint32_t cursor = import_top; cursor < import_end;) {
        if (import_end - cursor < sizeof(std::uint16_t)) {
            result.detail = "A module import record is truncated.";
            return result;
        }
        std::uint32_t address = 0;
        if (!checked_guest_address(module_segment_base, cursor, address, error)) {
            result.detail = error;
            return result;
        }
        std::array<std::uint8_t, sizeof(std::uint16_t)> size_bytes{};
        if (!memory.read(address, size_bytes, error)) {
            result.detail = "Could not read a module import record size: " + error;
            return result;
        }
        const auto record_size = read_u16(size_bytes, 0);
        if ((record_size != import_record_size && record_size != short_import_record_size) ||
            record_size > import_end - cursor) {
            result.detail = "A module import record has an unsupported or truncated size.";
            return result;
        }
        std::vector<std::uint8_t> record(record_size);
        if (!memory.read(address, record, error)) {
            result.detail = "Could not read a module import record: " + error;
            return result;
        }

        const auto function_count = read_u16(record, 6);
        const auto variable_count = read_u16(record, 8);
        const auto tls_count = read_u16(record, 10);
        const auto total = static_cast<std::uint64_t>(function_count) +
            variable_count + tls_count;
        if (total > maximum_symbols) {
            result.detail = "A module import symbol count is excessive.";
            return result;
        }
        const auto function_nids = read_u32(record,
            record_size == import_record_size ? 28 : 20);
        const auto variable_nids = read_u32(record,
            record_size == import_record_size ? 36 : 28);
        const auto tls_nids = record_size == import_record_size ? read_u32(record, 44) : 0;
        if (!read_nid_table(memory, function_nids, function_count,
                result.imported_nids, error) ||
            !read_nid_table(memory, variable_nids, variable_count,
                result.imported_nids, error) ||
            !read_nid_table(memory, tls_nids, tls_count,
                result.imported_nids, error)) {
            result.detail = error;
            return result;
        }
        ++result.import_library_count;
        result.imported_function_count += function_count;
        result.imported_variable_count += variable_count + tls_count;
        cursor += record_size;
    }

    result.success = true;
    std::ostringstream detail;
    detail << "Parsed " << result.export_library_count << " export libraries/"
           << result.exported_nids.size() << " NIDs and "
           << result.import_library_count << " import libraries/"
           << result.imported_nids.size() << " NIDs.";
    result.detail = detail.str();
    return result;
}

} // namespace vita3k::ios
