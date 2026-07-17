#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <span>
#include <string>
#include <vector>

namespace packages {

struct ArchiveApplicationInfo {
    std::string content_root;
    std::string title_id;
    std::string title;
    std::string category;
    std::string app_version;
    std::string content_id;
    std::string install_target;
};

struct ArchiveInspection {
    bool inspected{};
    bool valid{};
    std::size_t file_count{};
    std::size_t directory_count{};
    std::size_t unsafe_path_count{};
    std::uint64_t compressed_size{};
    std::uint64_t uncompressed_size{};
    std::vector<ArchiveApplicationInfo> applications;
    std::string detail;
};

struct ArchiveInstallResult {
    bool attempted{};
    bool success{};
    std::size_t application_count{};
    std::size_t file_count{};
    std::uint64_t bytes_written{};
    std::vector<std::string> installed_targets;
    // Metadata for the applications that were installed (title id, content id,
    // category, ...). Used by frontends to offer follow-up steps such as a
    // NoNpDrm work.bin license import for retail `gd` titles.
    std::vector<ArchiveApplicationInfo> installed_applications;
    std::string detail;
};

ArchiveInspection inspect_archive(std::span<const std::uint8_t> content);
ArchiveInspection inspect_archive(const std::filesystem::path &path);
ArchiveInstallResult install_archive_transactionally(const std::filesystem::path &archive_path,
    const std::filesystem::path &vfs_root);

} // namespace packages
