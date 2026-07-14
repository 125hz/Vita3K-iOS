#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace vita3k::ios {

struct ImportedArtifact {
    std::string filename;
    std::uintmax_t size;
    std::string kind;
    bool structurally_valid;
    std::string detail;
};

struct CoreStatus {
    bool linked;
    bool self_tests_passed;
    bool storage_ready;
    std::string storage_root;
    std::vector<ImportedArtifact> imported_artifacts;
    std::string summary;
};

CoreStatus initialize_core(const std::filesystem::path &documents_root);
CoreStatus query_core_status();
CoreStatus rescan_imports();

} // namespace vita3k::ios
