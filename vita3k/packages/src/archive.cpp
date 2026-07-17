#include <packages/archive.h>

#include <packages/sfo.h>

#include <miniz.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <fstream>
#include <limits>
#include <set>
#include <sstream>
#include <string_view>

namespace packages {
namespace {

constexpr std::size_t maximum_archive_entries = 100000;
constexpr std::size_t maximum_sfo_size = 16 * 1024 * 1024;
constexpr mz_uint maximum_archive_path_size = 4096;
constexpr std::uint64_t maximum_archive_install_size = 32ULL * 1024 * 1024 * 1024;
constexpr std::uint64_t install_free_space_margin = 64ULL * 1024 * 1024;
constexpr std::string_view sfo_suffix = "sce_sys/param.sfo";

struct SfoBuffer {
    std::vector<std::uint8_t> bytes;
    bool overflow{};
};

struct InstallOutput {
    std::ofstream stream;
    std::uint64_t written{};
};

bool read_archive_path(mz_zip_archive &zip, mz_uint index, std::string &name) {
    const auto name_size = mz_zip_reader_get_filename(&zip, index, nullptr, 0);
    if (name_size <= 1 || name_size > maximum_archive_path_size)
        return false;
    std::vector<char> storage(name_size);
    if (mz_zip_reader_get_filename(&zip, index, storage.data(), name_size) != name_size)
        return false;
    const std::string_view view(storage.data(), name_size - 1);
    if (view.find('\0') != std::string_view::npos)
        return false;
    name.assign(view);
    return true;
}

bool safe_title_id(std::string_view title_id) {
    return title_id.size() == 9 && std::ranges::all_of(title_id, [](unsigned char character) {
        return (character >= 'A' && character <= 'Z') || (character >= '0' && character <= '9');
    });
}

bool existing_parent_has_symlink(const std::filesystem::path &root,
    const std::filesystem::path &relative_parent, std::error_code &error) {
    auto current = root;
    for (const auto &component : relative_parent) {
        current /= component;
        const auto status = std::filesystem::symlink_status(current, error);
        if (error || std::filesystem::is_symlink(status))
            return true;
    }
    return false;
}

bool add_without_overflow(std::uint64_t &total, std::uint64_t value) {
    if (value > std::numeric_limits<std::uint64_t>::max() - total)
        return false;
    total += value;
    return true;
}

bool safe_archive_path(std::string_view path) {
    if (path.empty() || path.front() == '/' || path.front() == '\\' || path.find('\\') != std::string_view::npos || path.find(':') != std::string_view::npos)
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

size_t write_install_file(void *opaque, mz_uint64 file_offset, const void *buffer, size_t size) {
    auto &output = *static_cast<InstallOutput *>(opaque);
    if (file_offset != output.written || size > std::numeric_limits<std::uint64_t>::max() - output.written)
        return 0;
    output.stream.write(static_cast<const char *>(buffer), static_cast<std::streamsize>(size));
    if (!output.stream)
        return 0;
    output.written += size;
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
        std::string name;
        if (!read_archive_path(zip, index, name)) {
            ++result.unsafe_path_count;
            continue;
        }
        if (!safe_archive_path(name)) {
            ++result.unsafe_path_count;
            continue;
        }
        if (!add_without_overflow(result.compressed_size, stat.m_comp_size) || !add_without_overflow(result.uncompressed_size, stat.m_uncomp_size)) {
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
        if (!mz_zip_reader_extract_to_callback(&zip, entry.index, append_sfo, &buffer, 0) || buffer.overflow) {
            result.detail = "Could not extract bounded PARAM.SFO metadata from the archive.";
            return result;
        }
        sfo::SfoAppInfo app;
        sfo::get_param_info(app, buffer.bytes, 1);
        if (!safe_title_id(app.app_title_id) || app.app_title.empty()) {
            result.detail = "Archive PARAM.SFO is malformed or has an unsafe title identity.";
            return result;
        }
        result.applications.push_back({ .content_root = entry.root,
            .title_id = app.app_title_id,
            .title = app.app_title,
            .category = app.app_category,
            .app_version = app.app_version,
            .content_id = app.app_content_id,
            .install_target = install_target(app) });
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

ArchiveInstallResult install_archive_transactionally(const std::filesystem::path &archive_path,
    const std::filesystem::path &vfs_root) {
    ArchiveInstallResult result{ .attempted = true };
    mz_zip_archive zip{};
    const auto path_text = archive_path.string();
    if (!mz_zip_reader_init_file(&zip, path_text.c_str(), 0)) {
        result.detail = "Could not open the selected ZIP/VPK archive.";
        return result;
    }

    const auto inspection = inspect_open_archive(zip);
    if (!inspection.valid) {
        result.detail = "Installation rejected: " + inspection.detail;
        mz_zip_reader_end(&zip);
        return result;
    }
    if (inspection.uncompressed_size > maximum_archive_install_size) {
        result.detail = "Installation rejected: archive exceeds the 32 GiB safety limit.";
        mz_zip_reader_end(&zip);
        return result;
    }

    std::set<std::string> unique_targets;
    for (const auto &application : inspection.applications) {
        if (!unique_targets.insert(application.install_target).second) {
            result.detail = "Installation rejected: multiple archive roots resolve to the same Vita target.";
            mz_zip_reader_end(&zip);
            return result;
        }
    }

    std::error_code error;
    std::filesystem::create_directories(vfs_root, error);
    const auto staging_parent = vfs_root / ".install-staging";
    if (!error)
        std::filesystem::create_directories(staging_parent, error);
    const auto staging_status = std::filesystem::symlink_status(staging_parent, error);
    if (error || std::filesystem::is_symlink(staging_status)) {
        result.detail = "Could not create a safe installation staging directory: " + error.message();
        mz_zip_reader_end(&zip);
        return result;
    }
    const auto available = std::filesystem::space(vfs_root, error).available;
    if (error || inspection.uncompressed_size > available || install_free_space_margin > available - inspection.uncompressed_size) {
        result.detail = error ? "Could not query free storage: " + error.message()
                              : "Installation rejected: insufficient free storage for transactional extraction.";
        mz_zip_reader_end(&zip);
        return result;
    }

    static std::atomic_uint64_t transaction_counter{};
    const auto transaction_id = static_cast<std::uint64_t>(
                                    std::chrono::steady_clock::now().time_since_epoch().count())
        + transaction_counter.fetch_add(1, std::memory_order_relaxed);
    const auto transaction_root = staging_parent / ("txn-" + std::to_string(transaction_id));
    const auto payload_root = transaction_root / "payload";
    const auto backup_root = transaction_root / "backup";
    std::filesystem::create_directories(payload_root, error);
    if (error) {
        result.detail = "Could not create the installation transaction: " + error.message();
        mz_zip_reader_end(&zip);
        return result;
    }

    const auto cleanup = [&]() {
        std::error_code ignored;
        std::filesystem::remove_all(transaction_root, ignored);
    };
    const auto fail = [&](std::string detail) {
        result.detail = std::move(detail);
        cleanup();
        mz_zip_reader_end(&zip);
        return result;
    };

    const auto entry_count = mz_zip_reader_get_num_files(&zip);
    for (mz_uint index = 0; index < entry_count; ++index) {
        if (mz_zip_reader_is_file_a_directory(&zip, index))
            continue;
        std::string name;
        if (!read_archive_path(zip, index, name) || !safe_archive_path(name))
            return fail("Installation rejected an unsafe archive path during extraction.");

        const ArchiveApplicationInfo *owner = nullptr;
        std::string_view relative;
        for (const auto &application : inspection.applications) {
            if (name.starts_with(application.content_root) && (!owner || application.content_root.size() > owner->content_root.size())) {
                owner = &application;
                relative = std::string_view(name).substr(application.content_root.size());
            }
        }
        if (!owner || relative.empty())
            continue;
        if (!safe_archive_path(relative))
            return fail("Installation rejected an unsafe application-relative path.");

        mz_zip_archive_file_stat stat{};
        if (!mz_zip_reader_file_stat(&zip, index, &stat) || !mz_zip_reader_is_file_supported(&zip, index))
            return fail("Installation encountered an unsupported or encrypted ZIP entry.");

        const auto output_path = payload_root / owner->install_target / std::filesystem::path(std::string(relative));
        std::filesystem::create_directories(output_path.parent_path(), error);
        if (error)
            return fail("Could not create a staged application directory: " + error.message());
        InstallOutput output{ .stream = std::ofstream(output_path, std::ios::binary) };
        if (!output.stream)
            return fail("Could not create a staged application file.");
        if (!mz_zip_reader_extract_to_callback(&zip, index, write_install_file, &output, 0) || output.written != stat.m_uncomp_size)
            return fail("A staged ZIP entry failed decompression or size verification.");
        output.stream.close();
        ++result.file_count;
        result.bytes_written += output.written;
    }
    mz_zip_reader_end(&zip);

    if (result.file_count == 0) {
        cleanup();
        result.detail = "Installation rejected: no application files matched the discovered roots.";
        return result;
    }

    struct TargetMove {
        std::filesystem::path staged;
        std::filesystem::path destination;
        std::filesystem::path backup;
        bool backed_up{};
        bool installed{};
    };
    std::vector<TargetMove> moves;
    moves.reserve(inspection.applications.size());
    for (const auto &application : inspection.applications) {
        const auto relative_target = std::filesystem::path(application.install_target);
        if (existing_parent_has_symlink(vfs_root, relative_target.parent_path(), error)) {
            cleanup();
            result.detail = "Installation rejected a symlinked Vita destination path.";
            return result;
        }
        moves.push_back({ .staged = payload_root / application.install_target,
            .destination = vfs_root / application.install_target,
            .backup = backup_root / application.install_target });
        if (!std::filesystem::exists(moves.back().staged, error) || error) {
            cleanup();
            result.detail = "Installation transaction is missing a staged application root.";
            return result;
        }
    }

    const auto rollback = [&]() {
        for (auto iterator = moves.rbegin(); iterator != moves.rend(); ++iterator) {
            std::error_code ignored;
            if (iterator->installed)
                std::filesystem::remove_all(iterator->destination, ignored);
            if (iterator->backed_up) {
                std::filesystem::create_directories(iterator->destination.parent_path(), ignored);
                std::filesystem::rename(iterator->backup, iterator->destination, ignored);
            }
        }
        cleanup();
    };

    for (auto &move : moves) {
        std::filesystem::create_directories(move.destination.parent_path(), error);
        if (error) {
            rollback();
            result.detail = "Could not prepare an emulated Vita destination: " + error.message();
            return result;
        }
        if (std::filesystem::exists(move.destination, error) && !error) {
            std::filesystem::create_directories(move.backup.parent_path(), error);
            if (!error)
                std::filesystem::rename(move.destination, move.backup, error);
            if (error) {
                rollback();
                result.detail = "Could not back up the previous installed title: " + error.message();
                return result;
            }
            move.backed_up = true;
        }
    }
    for (auto &move : moves) {
        std::filesystem::rename(move.staged, move.destination, error);
        if (error) {
            rollback();
            result.detail = "Could not commit the staged Vita application: " + error.message();
            return result;
        }
        move.installed = true;
        result.installed_targets.push_back(
            std::filesystem::relative(move.destination, vfs_root).generic_string());
    }

    cleanup();
    result.success = true;
    result.application_count = inspection.applications.size();
    result.installed_applications = inspection.applications;
    std::ostringstream detail;
    detail << "Installed " << result.application_count << " application root"
           << (result.application_count == 1 ? "" : "s") << " transactionally; "
           << result.file_count << " files; " << result.bytes_written << " bytes; targets ";
    for (std::size_t index = 0; index < result.installed_targets.size(); ++index) {
        if (index != 0)
            detail << ", ";
        detail << result.installed_targets[index];
    }
    detail << ".";
    result.detail = detail.str();
    return result;
}

} // namespace packages
