#pragma once

#include <filesystem>
#include <string>

namespace vita3k::ios {

struct ExecutableProbeResult {
    std::string kind;
    bool recognized = false;
    bool structurally_valid = false;
    std::string detail;
};

ExecutableProbeResult probe_artifact(const std::filesystem::path &path);

} // namespace vita3k::ios
