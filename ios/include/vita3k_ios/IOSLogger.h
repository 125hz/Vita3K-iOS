#pragma once

#include <filesystem>
#include <string_view>

namespace vita3k::ios {

void initialize_logging();
void log_message(std::string_view level, std::string_view message);
std::filesystem::path log_file_path();

} // namespace vita3k::ios
