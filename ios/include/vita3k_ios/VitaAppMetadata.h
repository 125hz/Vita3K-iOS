#pragma once

#include <cstdint>
#include <filesystem>
#include <span>
#include <string>
#include <vector>

namespace vita3k::ios {

struct VitaAppMetadata {
    bool parsed{};
    std::string title_id;
    std::string title;
    std::string category;
    std::string app_version;
    std::string detail;
};

VitaAppMetadata parse_vita_app_metadata(std::span<const std::uint8_t> content);
VitaAppMetadata parse_vita_app_metadata(const std::filesystem::path &path);
std::vector<std::uint8_t> make_synthetic_param_sfo(std::string title_id,
    std::string title, std::string category = "gd", std::string app_version = "01.00");

} // namespace vita3k::ios
