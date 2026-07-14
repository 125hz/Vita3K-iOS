#include <packages/archive.h>

#include <packages/sfo.h>

#include <miniz.h>

#include <algorithm>
#include <limits>
#include <set>
#include <sstream>
#include <string_view>

namespace packages {
namespace {

constexpr std::size_t maximum_archive_entries = 100000;
constexpr std::size_t maximum_sfo_size = 16 * 1024 * 1024;
constexpr mz_uint maximum_archive_path_size = 4096;
constexpr std::string_view sfo_suffix = "sce_sys/param.sfo";

struct SfoBuffer {
    std::vector<std::uint8_t> bytes;
    bool overflow{};
};

bool add_without_overflow(std::uint64_t &total, std::uint64_t value) {
    if (value > std::numeric_limits<std::uint64_t>::max() - total)
        return false;
    total += value;
    return true;
}

bool safe_archive_path(std::string_view path) {
    if (path.empty() || path.front() == '/' || path.front() == '\\' ||
        path.find('\\') != std::string_view::npos || path.find(':') != std::string_view::npos)
        return false;

    std::size_t offset = 0;
    while (offset < path.size()) {
        const auto separator = path.find('/', offset);
        const auto end = separator == std::string_view::npos ? path.size() : separator;
        const auto component = path.substr(offset, end - offset);
        if (component.empty() || component == "." || component == "..")
            return false;
        if (separator == std::string_view::npos)
            break;
        offset = separator + 1;
        if (offset == path.size())
            break; // A single trailing slash denotes a directory.
    }
    return true;
}

bool find_content_root(std::string_view name, std::string &root) {
    if (!name.ends_with(sfo_suffix))
        return false;
    const auto prefix_size = name.size() - sfo_suffix.size();
    if (prefix_size != 0 && name[prefix_size - 1] != '/')
        return false;
    root.assign(name.substr(0, prefix_size));
    return true;
}

size_t append_sfo(void *opaque, mz_uint64 file_offset, const void *buffer, size_t size) {
    auto &output = *static_cast<SfoBuffer *>(opaque);
    if (file_offset != output.bytes.size() || size > maximum_sfo_size - output.bytes.size()) {
        output.overflow = true;
        return 0;
    }
    const auto *first = static_cast<const std::uint8_t *>(buffer);
    output.bytes.insert(output.bytes.end(), first, first + size);
    return size;
}

std::string install_target(const sfo::SfoAppInfo &app) {
    if (app.app_category.find("gp") != std::string::npos)
        return "ux0/patch/" + app.app_title_id;
    if (app.app_category == "ac")
        return "ux0/addcont/" + app.app_title_id;
    return "ux0/app/" + app.app_title_id;
}

ArchiveInspection inspect_open_archive(mz_zip_archive &zip) {
    ArchiveInspection result{ .inspected = true };
    const auto entry_count = static_cast<std::size_t>(mz_zip_reader_get_num_files(&zip));
    if (entry_count == 0 || entry_count > maximum_archive_entries) {
        result.detail = entry_count == 0 ? "Archive contains no entries."
                                         : "Archive entry count exceeds the inspection limit.";
        return result;
    }

    struct SfoEntry {
        mz_uint index;
        std::string root;
    };
    std::vector<SfoEntry> sfo_entries;
    std::set<std::string> roots;
    for (mz_uint index = 0; index < entry_count; ++index) {
        mz_zip_archive_file_stat stat{};
        if (!mz_zip_reader_file_stat(&zip, index, &stat)) {
            result.detail = "Could not read an archive directory entry.";
            return result;
        }
        const auto name_size = mz_zip_reader_get_filename(&zip, index, nullptr, 0);
        if (name_size <= 1 || name_size > maximum_archive_path_size) {
            ++result.unsafe_path_count;
            continue;
        }
        std::vector<char> name_storage(name_size);
        if (mz_zip_reader_get_filename(&zip, index, name_storage.data(), name_size) != name_size) {
            result.detail = "Could not read a complete archive entry path.";
            return result;
        }
        const std::string_view name(name_storage.data(), name_size - 1);
        if (name.find('\0') != std::string_view::npos || !safe_archive_path(name)) {
            ++result.unsafe_path_count;
            continue;
        }
        if (!add_without_overflow(result.compressed_size, stat.m_comp_size) ||
            !add_without_overflow(result.uncompressed_size, stat.m_uncomp_size)) {
            result.detail = "Archive size totals overflowed the supported range.";
            return result;
        }
        if (mz_zip_reader_is_file_a_directory(&zip, index)) {
            ++result.directory_count;
            continue;
        }
        ++result.file_count;
        std::string root;
        if (find_content_root(name, root) && roots.insert(root).second)
            sfo_entries.push_back({ index, std::move(root) });
    }

    if (result.unsafe_path_count != 0) {
        result.detail = "Archive contains unsafe absolute, traversal, or malformed paths.";
        return result;
    }
    if (sfo_entries.empty()) {
        result.detail = "Archive contains no sce_sys/param.sfo application metadata.";
        return result;
    }

    for (const auto &entry : sfo_entries) {
        SfoBuffer buffer;
        if (!mz_zip_reader_extract_to_callback(&zip, entry.index, append_sfo, &buffer, 0) ||
            buffer.overflow) {
            result.detail = "Could not extract bounded PARAM.SFO metadata from the archive.";
            return result;
        }
        sfo::SfoAppInfo app;
        sfo::get_param_info(app, buffer.bytes, 1);
        if (app.app_title_id.empty() || app.app_title.empty()) {
            result.detail = "Archive PARAM.SFO is malformed or missing title identity.";
            return result;
        }
        result.applications.push_back({
            .content_root = entry.root,
            .title_id = app.app_title_id,
            .title = app.app_title,
            .category = app.app_category,
            .app_version = app.app_version,
            .content_id = app.app_content_id,
            .install_target = install_target(app)
        });
    }

    result.valid = true;
    std::ostringstream detail;
    detail << "Vita3K package archive inspector: " << result.file_count << " files; "
           << result.directory_count << " directories; " << result.applications.size()
           << " application" << (result.applications.size() == 1 ? "" : "s") << "; "
           << result.uncompressed_size << " uncompressed bytes";
    for (const auto &app : result.applications) {
        detail << "; title ID " << app.title_id << "; title " << app.title
               << "; planned target " << app.install_target;
    }
    detail << ". Inspection only; extraction and decryption are not active.";
    result.detail = detail.str();
    return result;
}

} // namespace

ArchiveInspection inspect_archive(std::span<const std::uint8_t> content) {
    ArchiveInspection result{ .inspected = true };
    if (content.empty()) {
        result.detail = "Archive is empty.";
        return result;
    }
    mz_zip_archive zip{};
    if (!mz_zip_reader_init_mem(&zip, content.data(), content.size(), 0)) {
        result.detail = "miniz rejected the archive container.";
        return result;
    }
    result = inspect_open_archive(zip);
    mz_zip_reader_end(&zip);
    return result;
}

ArchiveInspection inspect_archive(const std::filesystem::path &path) {
    ArchiveInspection result{ .inspected = true };
    const auto path_text = path.string();
    mz_zip_archive zip{};
    if (!mz_zip_reader_init_file(&zip, path_text.c_str(), 0)) {
        result.detail = "miniz could not open the archive container.";
        return result;
    }
    result = inspect_open_archive(zip);
    mz_zip_reader_end(&zip);
    return result;
}

} // namespace packages
