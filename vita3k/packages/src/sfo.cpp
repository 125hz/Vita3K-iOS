// Vita3K emulator project
// Copyright (C) 2026 Vita3K team
//
// This program is free software; you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation; either version 2 of the License, or
// (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License along
// with this program; if not, write to the Free Software Foundation, Inc.,
// 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.

/**
 * @file sfo.cpp
 * @brief PlayStation setting file (`.sfo`) handling
 *
 * PlayStation setting files (`.sfo`) contain metadata information usually describing
 * the content they are accompanying.
 */

#include <packages/sfo.h>

#include <algorithm>
#include <cctype>
#include <cstring>
#include <iomanip>
#include <iterator>
#include <sstream>

namespace sfo {

bool get_data_by_id(std::string &out_data, SfoFile &file, int id) {
    std::string key;
    switch (id) {
    case 6:
        key = "CONTENT_ID";
        break;
    case 7:
        key = "NP_COMMUNICATION_ID";
        break;
    case 8:
        key = "CATEGORY";
        break;
    case 9:
        key = "TITLE";
        break;
    case 10:
        key = "STITLE";
        break;
    case 0xc:
        key = "TITLE_ID";
        break;
    case 0xe: // Todo
    default:
        return false;
    }

    return get_data_by_key(out_data, file, key);
}

bool get_data_by_key(std::string &out_data, SfoFile &file, const std::string &key) {
    auto res = std::find_if(file.entries.begin(), file.entries.end(),
        [key](const auto &et) { return et.data.first == key; });

    if (res == file.entries.end()) {
        return false;
    }
    out_data = res->data.second;

    return true;
}

namespace {

std::string localized_key(const char *prefix, int sys_lang) {
    std::ostringstream key;
    key << prefix << '_' << std::setfill('0') << std::setw(2) << sys_lang;
    return key.str();
}

void trim(std::string &value) {
    const auto first = std::find_if_not(value.begin(), value.end(), [](unsigned char character) {
        return std::isspace(character) != 0;
    });
    const auto last = std::find_if_not(value.rbegin(), value.rend(), [](unsigned char character) {
        return std::isspace(character) != 0;
    }).base();
    if (first >= last) {
        value.clear();
        return;
    }
    const auto start = static_cast<std::size_t>(std::distance(value.begin(), first));
    const auto length = static_cast<std::size_t>(std::distance(first, last));
    value = value.substr(start, length);
}

} // namespace

void get_param_info(sfo::SfoAppInfo &app_info, const std::vector<uint8_t> &param, int sys_lang) {
    app_info = {};
    SfoFile sfo_handle;
    if (!sfo::load(sfo_handle, param))
        return;
    sfo::get_data_by_key(app_info.app_version, sfo_handle, "APP_VER");
    if (!app_info.app_version.empty() && app_info.app_version[0] == '0')
        app_info.app_version.erase(app_info.app_version.begin());
    sfo::get_data_by_key(app_info.app_category, sfo_handle, "CATEGORY");
    sfo::get_data_by_key(app_info.app_content_id, sfo_handle, "CONTENT_ID");
    if (!sfo::get_data_by_key(app_info.app_addcont, sfo_handle, "INSTALL_DIR_ADDCONT"))
        sfo::get_data_by_key(app_info.app_addcont, sfo_handle, "TITLE_ID");
    if (!sfo::get_data_by_key(app_info.app_savedata, sfo_handle, "INSTALL_DIR_SAVEDATA"))
        sfo::get_data_by_key(app_info.app_savedata, sfo_handle, "TITLE_ID");
    sfo::get_data_by_key(app_info.app_parental_level, sfo_handle, "PARENTAL_LEVEL");
    if (!sfo::get_data_by_key(app_info.app_short_title, sfo_handle, localized_key("STITLE", sys_lang)))
        sfo::get_data_by_key(app_info.app_short_title, sfo_handle, "STITLE");
    if (!sfo::get_data_by_key(app_info.app_title, sfo_handle, localized_key("TITLE", sys_lang)))
        sfo::get_data_by_key(app_info.app_title, sfo_handle, "TITLE");
    std::replace(app_info.app_title.begin(), app_info.app_title.end(), '\n', ' ');
    trim(app_info.app_title);
    sfo::get_data_by_key(app_info.app_title_id, sfo_handle, "TITLE_ID");
}

bool load(SfoFile &sfile, const std::vector<uint8_t> &content) {
    constexpr uint32_t psf_magic = 0x46535000;
    sfile = {};
    if (content.size() < sizeof(SfoHeader)) {
        return false;
    }

    memcpy(&sfile.header, content.data(), sizeof(SfoHeader));
    const auto entry_count = static_cast<std::size_t>(sfile.header.tables_entries);
    const auto index_bytes = entry_count * sizeof(SfoIndexTableEntry);
    const auto index_start = sizeof(SfoHeader);
    if (sfile.header.magic != psf_magic || sfile.header.version != 0x00000101 ||
        entry_count > (content.size() - sizeof(SfoHeader)) / sizeof(SfoIndexTableEntry) ||
        index_start + index_bytes > content.size() ||
        sfile.header.key_table_start < index_start + index_bytes ||
        sfile.header.key_table_start > sfile.header.data_table_start ||
        sfile.header.data_table_start > content.size()) {
        sfile = {};
        return false;
    }

    sfile.entries.resize(entry_count);
    for (std::size_t i = 0; i < entry_count; ++i) {
        auto &item = sfile.entries[i];
        memcpy(&item.entry, content.data() + index_start + i * sizeof(SfoIndexTableEntry),
            sizeof(SfoIndexTableEntry));

        const auto key_start = static_cast<std::size_t>(sfile.header.key_table_start) +
            item.entry.key_offset;
        if (key_start >= sfile.header.data_table_start) {
            sfile = {};
            return false;
        }
        const auto key_limit = content.begin() + sfile.header.data_table_start;
        const auto key_begin = content.begin() + key_start;
        const auto key_end = std::find(key_begin, key_limit, uint8_t{0});
        if (key_end == key_limit) {
            sfile = {};
            return false;
        }
        item.data.first.assign(key_begin, key_end);

        const auto data_start = static_cast<std::size_t>(sfile.header.data_table_start) +
            item.entry.data_offset;
        const auto data_size = static_cast<std::size_t>(item.entry.data_len);
        if (item.entry.data_len > item.entry.data_max_len || data_start > content.size() ||
            data_size > content.size() - data_start) {
            sfile = {};
            return false;
        }
        const auto data_begin = content.begin() + data_start;
        const auto data_end = data_begin + data_size;

        switch (item.entry.data_fmt) {
        case SfoDataFormat::UINT32_T:
            if (data_size < sizeof(uint32_t)) {
                sfile = {};
                return false;
            }
            uint32_t value;
            memcpy(&value, content.data() + data_start, sizeof(value));
            item.data.second = std::to_string(value);
            break;
        case SfoDataFormat::ASCII:
        case SfoDataFormat::UTF8:
            item.data.second.assign(data_begin, data_end);
            break;
        case SfoDataFormat::UTF8_NULL: {
            if (data_size == 0) {
                sfile = {};
                return false;
            }
            const auto terminator = std::find(data_begin, data_end, uint8_t{0});
            if (terminator == data_end) {
                sfile = {};
                return false;
            }
            item.data.second.assign(data_begin, terminator);
            break;
        }
        default:
            sfile = {};
            return false;
        }
    }

    return true;
}

} // namespace sfo
