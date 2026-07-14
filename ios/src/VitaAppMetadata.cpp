#include <vita3k_ios/VitaAppMetadata.h>

#include <packages/sfo.h>

#include <array>
#include <cstring>
#include <fstream>
#include <limits>
#include <sstream>
#include <utility>

namespace vita3k::ios {
namespace {

template <typename T>
void append_value(std::vector<std::uint8_t> &bytes, T value) {
    const auto offset = bytes.size();
    bytes.resize(offset + sizeof(value));
    std::memcpy(bytes.data() + offset, &value, sizeof(value));
}

} // namespace

VitaAppMetadata parse_vita_app_metadata(std::span<const std::uint8_t> content) {
    VitaAppMetadata metadata;
    if (content.empty()) {
        metadata.detail = "PARAM.SFO is empty.";
        return metadata;
    }

    std::vector<std::uint8_t> owned(content.begin(), content.end());
    SfoFile file;
    if (!sfo::load(file, owned)) {
        metadata.detail = "Vita3K's upstream SFO parser rejected malformed metadata.";
        return metadata;
    }
    sfo::get_data_by_key(metadata.title_id, file, "TITLE_ID");
    sfo::get_data_by_key(metadata.title, file, "TITLE");
    sfo::get_data_by_key(metadata.category, file, "CATEGORY");
    sfo::get_data_by_key(metadata.app_version, file, "APP_VER");
    if (metadata.title_id.empty() || metadata.title.empty()) {
        metadata.detail = "PARAM.SFO parsed but does not contain TITLE_ID and TITLE.";
        return metadata;
    }

    metadata.parsed = true;
    std::ostringstream detail;
    detail << "Vita3K upstream SFO parser: title ID " << metadata.title_id
           << "; title " << metadata.title;
    if (!metadata.category.empty()) {
        detail << "; category " << metadata.category;
    }
    if (!metadata.app_version.empty()) {
        detail << "; app version " << metadata.app_version;
    }
    detail << '.';
    metadata.detail = detail.str();
    return metadata;
}

VitaAppMetadata parse_vita_app_metadata(const std::filesystem::path &path) {
    constexpr std::uintmax_t maximum_sfo_size = 16 * 1024 * 1024;
    std::error_code error;
    const auto file_size = std::filesystem::file_size(path, error);
    if (error || file_size > maximum_sfo_size ||
        file_size > static_cast<std::uintmax_t>(std::numeric_limits<std::streamsize>::max())) {
        return { .detail = error ? "Could not size PARAM.SFO: " + error.message()
                                 : "PARAM.SFO exceeds the bounded metadata size." };
    }
    std::ifstream stream(path, std::ios::binary);
    if (!stream) {
        return { .detail = "Could not open PARAM.SFO." };
    }
    std::vector<std::uint8_t> content(static_cast<std::size_t>(file_size));
    stream.read(reinterpret_cast<char *>(content.data()),
        static_cast<std::streamsize>(content.size()));
    if (!stream && !content.empty()) {
        return { .detail = "Could not read PARAM.SFO." };
    }
    return parse_vita_app_metadata(content);
}

std::vector<std::uint8_t> make_synthetic_param_sfo(std::string title_id,
    std::string title, std::string category, std::string app_version) {
    struct Entry {
        std::string key;
        std::string value;
    };
    const std::array entries{
        Entry{ "TITLE_ID", std::move(title_id) },
        Entry{ "TITLE", std::move(title) },
        Entry{ "CATEGORY", std::move(category) },
        Entry{ "APP_VER", std::move(app_version) }
    };

    constexpr std::size_t header_size = sizeof(SfoHeader);
    constexpr std::size_t index_size = sizeof(SfoIndexTableEntry);
    std::vector<std::uint8_t> keys;
    std::vector<std::uint8_t> data;
    std::vector<SfoIndexTableEntry> indexes;
    indexes.reserve(entries.size());
    for (const auto &entry : entries) {
        const auto key_offset = keys.size();
        const auto data_offset = data.size();
        keys.insert(keys.end(), entry.key.begin(), entry.key.end());
        keys.push_back(0);
        data.insert(data.end(), entry.value.begin(), entry.value.end());
        data.push_back(0);
        indexes.push_back({
            .key_offset = static_cast<std::uint16_t>(key_offset),
            .data_fmt = SfoDataFormat::UTF8_NULL,
            .data_len = static_cast<std::uint32_t>(entry.value.size() + 1),
            .data_max_len = static_cast<std::uint32_t>(entry.value.size() + 1),
            .data_offset = static_cast<std::uint32_t>(data_offset)
        });
    }

    const auto key_table_start = header_size + indexes.size() * index_size;
    const auto data_table_start = (key_table_start + keys.size() + 3) & ~std::size_t{3};
    std::vector<std::uint8_t> result;
    result.reserve(data_table_start + data.size());
    append_value(result, static_cast<std::uint32_t>(0x46535000));
    append_value(result, static_cast<std::uint32_t>(0x00000101));
    append_value(result, static_cast<std::uint32_t>(key_table_start));
    append_value(result, static_cast<std::uint32_t>(data_table_start));
    append_value(result, static_cast<std::uint32_t>(indexes.size()));
    for (const auto &index : indexes) {
        append_value(result, index);
    }
    result.insert(result.end(), keys.begin(), keys.end());
    result.resize(data_table_start);
    result.insert(result.end(), data.begin(), data.end());

    return result;
}

} // namespace vita3k::ios
