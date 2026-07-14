#pragma once

#include <filesystem>
#include <string>
#include <vector>

namespace vita3k::ios {

struct HostStorage {
    bool ready = false;
    std::filesystem::path root;
    std::filesystem::path imports;
    std::vector<std::filesystem::directory_entry> imported_files;
    std::string error;
};

HostStorage initialize_host_storage(const std::filesystem::path &documents_root);
void scan_imports(HostStorage &storage);

} // namespace vita3k::ios
