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

// iOS upstream-core frontend. Lets the user choose an installed title, then
// boots it through the real emulator. Modeled on
// vita3k/android/jni/main_android.cpp and the bootstrap sequence in
// vita3k/android/jni/native_bootstrap.cpp.

#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>

#include <app/functions.h>
#include <app/session_controller.h>
#include <app/state.h>
#include <audio/state.h>
#include <packages/archive.h>
#include <packages/functions.h>
#include <packages/license.h>
#include <packages/pkg.h>
#include <packages/sfo.h>
#include <compat/functions.h>
#include <compat/state.h>
#include <config/functions.h>
#include <config/state.h>
#include <config/version.h>
#include <ctrl/functions.h>
#include <ctrl/state.h>
#include <cpu/functions.h>
#include <display/state.h>
#include <emuenv/state.h>
#include <io/state.h>
#include <mem/functions.h>
#include <modules/module_parent.h>
#include <np/trophy/collection.h>
#include <np/trophy/trp_parser.h>
#include <renderer/frame_host.h>
#include <renderer/functions.h>
#include <renderer/state.h>
#include <touch/functions.h>
#include <util/fs.h>
#include <util/log.h>

#include <miniz.h>

#include <csignal>
#include <dlfcn.h>
#include <execinfo.h>
#include <os/proc.h>
#include <sys/sysctl.h>
#include <unistd.h>

#include <vita3k_ios/NativeFrontend.h>
#include <vita3k_ios/VirtualController.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cctype>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace {

std::string g_current_trophy_id;
std::string g_current_title;
std::string g_current_title_id;
std::atomic_bool g_jit_pool_ready{ false };
std::atomic_bool g_unhandled_universal_jit_breakpoint{ false };

bool safe_identifier(std::string_view value, const std::size_t maximum = 32) {
    return !value.empty() && value.size() <= maximum
        && std::all_of(value.begin(), value.end(), [](unsigned char character) {
               return std::isalnum(character) || character == '_' || character == '-';
           });
}

std::uint64_t directory_size(const fs::path &root) {
    boost::system::error_code error;
    if (!fs::exists(root, error) || error)
        return 0;
    std::uint64_t total = 0;
    for (fs::recursive_directory_iterator it(root, error), end; it != end && !error; it.increment(error)) {
        if (!fs::is_regular_file(it->path(), error) || error)
            continue;
        const auto size = fs::file_size(it->path(), error);
        if (!error && size <= std::numeric_limits<std::uint64_t>::max() - total)
            total += size;
    }
    return total;
}

std::string trophy_id_for_title(const EmuEnvState &emuenv, const std::string &title_id) {
    const fs::path param_path = emuenv.vita_fs_path / "ux0/app" / title_id / "sce_sys/param.sfo";
    fs::ifstream input(param_path, std::ios::binary);
    std::string trophy_id;
    if (input) {
        const std::vector<std::uint8_t> bytes((std::istreambuf_iterator<char>(input)), {});
        SfoFile sfo_file{};
        if (sfo::load(sfo_file, bytes))
            sfo::get_data_by_key(trophy_id, sfo_file, "NP_COMMUNICATION_ID");
    }

    // Some dumps omit NP_COMMUNICATION_ID from their visible param.sfo even
    // though they ship a normal trophy archive. Resolve the archive directory
    // as a fallback so the library does not incorrectly report "no data".
    const fs::path trophy_root = emuenv.vita_fs_path / "ux0/app" / title_id / "sce_sys/trophy";
    if (!trophy_id.empty() && fs::exists(trophy_root / trophy_id / "TROPHY.TRP"))
        return trophy_id;
    boost::system::error_code error;
    if (fs::exists(trophy_root, error) && !error) {
        for (fs::directory_iterator it(trophy_root, error), end; it != end && !error; it.increment(error)) {
            if (fs::is_directory(it->path(), error) && !error && fs::exists(it->path() / "TROPHY.TRP", error) && !error) {
                const std::string archive_id = it->path().filename().string();
                if (safe_identifier(archive_id)) {
                    if (!trophy_id.empty() && trophy_id != archive_id)
                        LOG_WARN("Trophy archive id {} differs from SFO id {} for {}; using archive", archive_id, trophy_id, title_id);
                    return archive_id;
                }
            }
        }
    }
    return trophy_id;
}

bool install_trophy_metadata_for_title(EmuEnvState &emuenv, const std::string &title_id,
    const std::string &trophy_id) {
    if (!safe_identifier(title_id, 16) || !safe_identifier(trophy_id))
        return false;
    const fs::path conf_path = emuenv.vita_fs_path / "ux0/user" / emuenv.io.user_id
        / "trophy/conf" / trophy_id;
    if (fs::exists(conf_path / "TROP.SFM") || fs::exists(conf_path / "TROP_00.SFM")
        || fs::exists(conf_path / "TROP_01.SFM"))
        return true;

    const fs::path archive_path = emuenv.vita_fs_path / "ux0/app" / title_id
        / "sce_sys/trophy" / trophy_id / "TROPHY.TRP";
    fs::ifstream archive(archive_path, std::ios::binary);
    if (!archive) {
        LOG_WARN("Trophy archive is missing for {} at {}", title_id, archive_path);
        return false;
    }

    np::trophy::TRPFile trp;
    trp.seek_func = [&archive](const int offset) {
        archive.clear();
        archive.seekg(offset, std::ios::beg);
        return static_cast<bool>(archive);
    };
    trp.read_func = [&archive](void *destination, const std::uint32_t amount) {
        archive.read(static_cast<char *>(destination), static_cast<std::streamsize>(amount));
        return archive.gcount() == static_cast<std::streamsize>(amount);
    };
    if (!trp.header_parse()) {
        LOG_ERROR("Failed to parse trophy archive {}", archive_path);
        return false;
    }

    fs::create_directories(conf_path);
    constexpr std::uint64_t maximum_trophy_file_size = 32 * 1024 * 1024;
    for (std::size_t index = 0; index < trp.entries.size(); ++index) {
        const auto &entry = trp.entries[index];
        const std::string filename(entry.filename.c_str());
        if (filename.empty() || filename.size() > 96 || filename.find('/') != std::string::npos
            || filename.find('\\') != std::string::npos || filename == "." || filename == ".."
            || entry.size > maximum_trophy_file_size) {
            LOG_ERROR("Rejected unsafe trophy entry '{}' ({} bytes) in {}", filename, entry.size, archive_path);
            return false;
        }
        const fs::path output_path = conf_path / filename;
        fs::ofstream output(output_path, std::ios::binary | std::ios::trunc);
        if (!output)
            return false;
        const bool copied = trp.get_entry_data(static_cast<std::uint32_t>(index),
            [&output](void *source, const std::uint32_t amount) {
                output.write(static_cast<const char *>(source), static_cast<std::streamsize>(amount));
                return static_cast<bool>(output);
            });
        output.close();
        if (!copied) {
            boost::system::error_code cleanup_error;
            fs::remove(output_path, cleanup_error);
            LOG_ERROR("Failed to extract trophy entry '{}' from {}", filename, archive_path);
            return false;
        }
    }
    LOG_INFO("Installed trophy metadata for {} ({}) from {}", title_id, trophy_id, archive_path);
    return true;
}

np::trophy::CollectionSource trophy_source(EmuEnvState &emuenv) {
    return {
        .io = &emuenv.io,
        .vita_fs_path = emuenv.vita_fs_path,
        .user_id = emuenv.io.user_id,
        .lang = static_cast<std::uint32_t>(emuenv.cfg.sys_lang),
    };
}

void show_trophies(EmuEnvState &emuenv, const std::string &requested_id,
    const std::string &fallback_title, const std::string &title_id) {
    std::string trophy_id = safe_identifier(requested_id) ? requested_id : std::string{};
    if (trophy_id.empty() && safe_identifier(title_id, 16))
        trophy_id = trophy_id_for_title(emuenv, title_id);
    if (!trophy_id.empty() && safe_identifier(title_id, 16))
        install_trophy_metadata_for_title(emuenv, title_id, trophy_id);
    if (trophy_id.empty()) {
        const auto ids = np::trophy::list_collection_ids(trophy_source(emuenv));
        if (ids.size() == 1)
            trophy_id = ids.front();
    }
    np::trophy::CollectionSnapshot snapshot;
    Vita3KIOSTrophyCollection collection;
    collection.title = fallback_title.empty() ? "Trophies" : fallback_title;
    collection.trophy_id = trophy_id;
    if (!trophy_id.empty() && np::trophy::load_collection(trophy_source(emuenv), trophy_id, snapshot)) {
        collection.title = snapshot.title.empty() ? collection.title : snapshot.title;
        collection.unlocked = snapshot.unlocked;
        collection.total = snapshot.total;
        collection.trophies.reserve(snapshot.trophies.size());
        for (const auto &trophy : snapshot.trophies) {
            collection.trophies.push_back({
                .id = trophy.id,
                .name = trophy.name,
                .detail = trophy.detail,
                .icon_path = trophy.icon_path,
                .grade = trophy.grade,
                .hidden = trophy.hidden,
                .earned = trophy.earned,
                .timestamp = trophy.timestamp,
            });
        }
        std::stable_sort(collection.trophies.begin(), collection.trophies.end(), [](const auto &left, const auto &right) {
            return left.earned != right.earned ? left.earned > right.earned : left.id < right.id;
        });
    }
    vita3k_ios_present_trophies(collection);
}

class IOSFrameHost final : public renderer::FrameHost {
public:
    explicit IOSFrameHost(SDL_Window *window)
        : m_window(window) {
    }

    renderer::DisplayHandle handle() const override {
        // The Vulkan screen renderer creates the surface for this handle
        // through SDL_Vulkan_CreateSurface, which works on iOS as well.
        return renderer::AndroidDisplayHandle{ m_window };
    }

    int drawable_width() const override {
        int width = 960;
        int height = 544;
        SDL_GetWindowSizeInPixels(m_window, &width, &height);
        return width;
    }

    int drawable_height() const override {
        int width = 960;
        int height = 544;
        SDL_GetWindowSizeInPixels(m_window, &width, &height);
        return height;
    }

    std::vector<std::string> font_dirs() const override {
        // Guest-visible fonts come from the installed firmware font package.
        return {};
    }

    bool custom_screen_viewport(const int drawable_width, const int drawable_height,
        float &x, float &y, float &width, float &height) const override {
        if (drawable_width <= 0 || drawable_height <= drawable_width)
            return false;

        constexpr float vita_width = 960.0f;
        constexpr float vita_height = 544.0f;
        const float safe_top = std::clamp(vita3k_ios_safe_area_top_pixels(),
            0.0f, static_cast<float>(drawable_height) * 0.2f);
        const float game_zone_height = std::max(1.0f,
            static_cast<float>(drawable_height) * 0.5f - safe_top);
        const float scale = std::min(static_cast<float>(drawable_width) / vita_width,
            game_zone_height / vita_height);
        width = vita_width * scale;
        height = vita_height * scale;
        x = (static_cast<float>(drawable_width) - width) * 0.5f;
        y = safe_top;
        return true;
    }

private:
    SDL_Window *m_window = nullptr;
};

// csops() is a private syscall but is the standard way to read the code-signing
// status flags. CS_DEBUGGED stays set for the process lifetime once a debugger
// has attached and enabled invalid-page execution (JIT), whereas P_TRACED drops
// the moment StikDebug detaches.
extern "C" int csops(pid_t pid, unsigned int ops, void *useraddr, size_t usersize);
#ifndef CS_OPS_STATUS
#define CS_OPS_STATUS 0
#endif
#ifndef CS_DEBUGGED
#define CS_DEBUGGED 0x10000000u
#endif

bool ios_debugger_attached() {
    struct kinfo_proc info{};
    std::size_t size = sizeof(info);
    int mib[4] = { CTL_KERN, KERN_PROC, KERN_PROC_PID, getpid() };
    return sysctl(mib, 4, &info, &size, nullptr, 0) == 0
        && (info.kp_proc.p_flag & P_TRACED) != 0;
}

bool ios_jit_capability_enabled() {
    uint32_t cs_flags = 0;
    return (csops(getpid(), CS_OPS_STATUS, &cs_flags, sizeof(cs_flags)) == 0
               && (cs_flags & CS_DEBUGGED) != 0)
        || ios_debugger_attached();
}

// iOS 26 universal JIT needs the debugger attached while the permanent RX/RW
// region pool is prepared. CS_DEBUGGED survives a detach, but it is not enough
// to service Oaknut's BRK request. Once the pool is complete, detaching is safe.
bool ios_jit_available() {
    return g_jit_pool_ready.load(std::memory_order_relaxed)
        || (ios_jit_capability_enabled() && ios_debugger_attached());
}

fs::path ios_storage_path() {
    // Documents/Tsubomi inside the app sandbox. UIFileSharingEnabled is set,
    // so the user can inspect it and drop firmware/game data through the
    // Files app or Finder file sharing.
    char *pref = SDL_GetPrefPath(nullptr, nullptr);
    fs::path documents;
    if (pref) {
        // SDL pref path is <sandbox>/Library/Application Support/; Documents
        // sits next to Library.
        documents = fs::path(pref).parent_path().parent_path().parent_path() / "Documents";
        SDL_free(pref);
    }
    if (documents.empty() || !fs::exists(documents.parent_path()))
        documents = fs::path(SDL_GetUserFolder(SDL_FOLDER_DOCUMENTS) ? SDL_GetUserFolder(SDL_FOLDER_DOCUMENTS) : "Documents");

    // The data root was renamed Vita3K -> Tsubomi. Migrate an existing install
    // once so games/saves/firmware carry over; only when the new name does not
    // already exist. Failure is non-fatal (fall back to whichever exists).
    const fs::path legacy_root = documents / "Vita3K";
    const fs::path current_root = documents / "Tsubomi";
    boost::system::error_code migrate_error;
    if (fs::exists(legacy_root, migrate_error) && !fs::exists(current_root, migrate_error)) {
        fs::rename(legacy_root, current_root, migrate_error);
        if (migrate_error)
            LOG_ERROR("iOS storage migration Vita3K -> Tsubomi failed: {}", migrate_error.message());
        else
            LOG_INFO("iOS storage migrated: '{}' -> '{}'", legacy_root, current_root);
    }

    return current_root / "";
}

bool initialize_session(const fs::path &storage_path, Root &root_paths,
    std::unique_ptr<EmuEnvState> &emuenv) {
    try {
        const fs::path vita_path = storage_path / "vita" / "";

        // The app bundle carries shaders-builtin (and future static assets);
        // SDL_GetBasePath resolves to the bundle resource directory on iOS.
        const char *bundle_path = SDL_GetBasePath();
        root_paths.set_static_assets_path(
            bundle_path ? fs::path(bundle_path) : fs::path{});
        root_paths.set_vita_fs_path(vita_path);
        root_paths.set_log_path(storage_path);
        root_paths.set_config_path(storage_path);
        root_paths.set_shared_path(storage_path);
        root_paths.set_cache_path(storage_path / "cache" / "");
        root_paths.set_patch_path(storage_path / "patch" / "");

        if (!fs::exists(root_paths.get_vita_fs_path()))
            fs::create_directories(root_paths.get_vita_fs_path());

        fs::create_directories(root_paths.get_config_path());
        fs::create_directories(root_paths.get_cache_path());
        fs::create_directories(root_paths.get_log_path() / "shaderlog");
        fs::create_directories(root_paths.get_log_path() / "texturelog");
        fs::create_directories(root_paths.get_patch_path());
        fs::create_directories(root_paths.get_shared_path() / "textures");

        if (logging::init(root_paths, true) != Success)
            return false;

        LOG_INFO("{}", window_title);
        LOG_INFO("iOS storage path: {}", storage_path);

        emuenv = std::make_unique<EmuEnvState>();

        Config cfg{};
        char arg0[] = "vita3k";
        char *argv[] = { arg0, nullptr };
        if (config::init_config(cfg, 1, argv, root_paths, false) != Success) {
            LOG_ERROR("Failed to initialise config.");
            emuenv.reset();
            return false;
        }

        // MoltenVK-backed Vulkan is the only renderer on iOS.
        cfg.backend_renderer = "Vulkan";

        // iOS assigns the app container a new absolute path on every
        // reinstall while keeping Documents' contents, so an absolute path
        // persisted in config.yml points into the previous container. Always
        // use the freshly resolved sandbox location instead.
        cfg.set_vita_fs_path(root_paths.get_vita_fs_path());

        fs::create_directories(cfg.get_vita_fs_path());

        if (!app::init(*emuenv, cfg, root_paths)) {
            LOG_ERROR("Failed to initialise emulated environment.");
            emuenv.reset();
            return false;
        }

        if (emuenv->cfg.controller_binds.empty() || emuenv->cfg.controller_binds.size() != 15
            || emuenv->cfg.controller_axis_binds.empty() || emuenv->cfg.controller_axis_binds.size() != 6) {
            app::reset_controller_binding(*emuenv);
        }

        init_libraries(*emuenv);

        if (!app::init_apps_list(*emuenv))
            LOG_ERROR("Failed to initialise apps list.");

        app::load_users(*emuenv);
        if (!app::ensure_current_user(*emuenv)) {
            LOG_ERROR("Failed to initialize active user.");
            return false;
        }
        // init_apps_list does not load ux0/user/time.xml. Without this, a new
        // process always showed every title as 0m / Never played even though
        // begin_launch had persisted a valid record in the previous process.
        app::load_app_times(*emuenv);
        compat::load_from_disk(emuenv->compat, std::filesystem::path(emuenv->cache_path.string()));
        return true;
    } catch (const std::exception &error) {
        LOG_ERROR("Failed to initialize iOS storage path '{}': {}", storage_path, error.what());
        emuenv.reset();
        return false;
    }
}

// Built-in firmware apps record the shipping firmware in their param.sfo
// PSP2_SYSTEM_VER key, BCD-encoded (0x03650000 == 3.65). Derive the number
// from one of them so firmware copied in manually (not via the app's PUP
// importer, which writes fw_version.txt) still shows a real "FW 3.65".
std::optional<std::string> derive_firmware_version(EmuEnvState &emuenv) {
    static const char *const firmware_app_sfos[] = {
        "vs0/app/NPXS10015/sce_sys/param.sfo", // Settings
        "vs0/app/NPXS10013/sce_sys/param.sfo", // PS Store
        "vs0/app/NPXS10008/sce_sys/param.sfo", // Trophy Collection
    };
    for (const char *relative : firmware_app_sfos) {
        fs::ifstream file(emuenv.vita_fs_path / relative, std::ios::binary);
        if (!file.is_open())
            continue;
        const std::vector<uint8_t> content((std::istreambuf_iterator<char>(file)),
            std::istreambuf_iterator<char>());
        SfoFile sfo;
        if (!sfo::load(sfo, content))
            continue;
        std::string raw;
        if (!sfo::get_data_by_key(raw, sfo, "PSP2_SYSTEM_VER"))
            continue;
        uint32_t value = 0;
        try {
            value = static_cast<uint32_t>(std::stoul(raw));
        } catch (const std::exception &) {
            continue;
        }
        const uint32_t major = (value >> 24) & 0xFF;
        const uint32_t minor = (value >> 16) & 0xFF;
        // Reject non-BCD / implausible values instead of showing garbage.
        if (value == 0 || major > 0x09 || (minor & 0x0F) > 0x09 || ((minor >> 4) & 0x0F) > 0x09)
            continue;
        return fmt::format("{:X}.{:02X}", major, minor);
    }
    return std::nullopt;
}

std::string firmware_version_display(EmuEnvState &emuenv) {
    // install_pup returns the version string; the frontend persists it here
    // because the extracted firmware does not keep version.txt around.
    std::string version;
    fs::ifstream file(emuenv.log_path / "fw_version.txt");
    if (file.is_open())
        std::getline(file, version);
    if (!version.empty())
        return "FW " + version;

    // Firmware present but no PUP-recorded version (manually copied). Derive it
    // from the installed content and cache it so later boots are instant.
    if (const auto derived = derive_firmware_version(emuenv)) {
        fs::ofstream out(emuenv.log_path / "fw_version.txt");
        out << *derived;
        LOG_INFO("Derived firmware version from installed content: {}", *derived);
        return "FW " + *derived;
    }
    return app::get_firmware_state(emuenv).main_firmware ? "FW installed" : "No firmware";
}

bool firmware_setup_complete(const EmuEnvState &emuenv) {
    const auto state = app::get_firmware_state(emuenv);
    return state.font_package && state.preinstalled_package && state.main_firmware;
}

Vita3KIOSSettings native_settings(EmuEnvState &emuenv) {
    const auto &current = emuenv.cfg.current_config;
    const auto firmware = app::get_firmware_state(emuenv);
    std::vector<std::string> missing;
    if (!firmware.font_package)
        missing.emplace_back("FONTPKG.PUP");
    if (!firmware.preinstalled_package)
        missing.emplace_back("PREINSTALL.PUP");
    if (!firmware.main_firmware)
        missing.emplace_back("PSVUPDAT.PUP");
    std::string missing_text;
    for (const auto &name : missing) {
        if (!missing_text.empty())
            missing_text += ", ";
        missing_text += name;
    }
    return {
        .resolution_multiplier = current.resolution_multiplier,
        .v_sync = current.v_sync,
        .fps_limit = vita3k_ios_load_fps_limit(),
        .cpu_opt = current.cpu_opt,
        .ngs_enable = current.ngs_enable,
        .async_pipeline_compilation = current.async_pipeline_compilation,
        .anisotropic_filtering = current.anisotropic_filtering,
        .firmware_version = firmware_version_display(emuenv),
        .firmware_ready = missing.empty(),
        .missing_firmware = std::move(missing_text),
    };
}

struct ImportJob {
    std::atomic_bool done{ false };
    bool firmware = false;
    bool success = false;
    bool rescan_apps = true;
    std::string message;
    std::string share_path;
    // Populated for a successful game archive install so the frontend can offer
    // a follow-up NoNpDrm work.bin import for retail titles that need one.
    std::vector<packages::ArchiveApplicationInfo> installed_applications;
};
std::shared_ptr<ImportJob> g_import_job;

bool safe_save_archive_path(std::string_view name) {
    if (name.empty() || name.front() == '/' || name.front() == '\\'
        || name.find('\\') != std::string_view::npos || name.find(':') != std::string_view::npos)
        return false;
    for (std::size_t offset = 0; offset < name.size();) {
        const auto separator = name.find('/', offset);
        const auto end = separator == std::string_view::npos ? name.size() : separator;
        const auto part = name.substr(offset, end - offset);
        if (part.empty() || part == "." || part == "..")
            return false;
        if (separator == std::string_view::npos)
            break;
        offset = separator + 1;
    }
    return true;
}

fs::path save_path_for_title(const EmuEnvState &emuenv, const std::string &title_id) {
    return emuenv.vita_fs_path / "ux0/user" / emuenv.io.user_id / "savedata" / title_id;
}

void start_save_export(EmuEnvState &emuenv, const std::string &title_id) {
    if (!safe_identifier(title_id, 16)) {
        vita3k_ios_report_import_result("Save export rejected an invalid title ID", false);
        return;
    }
    if (g_import_job && !g_import_job->done.load()) {
        vita3k_ios_report_import_result("Another file operation is still running", false);
        return;
    }
    auto job = std::make_shared<ImportJob>();
    job->rescan_apps = false;
    g_import_job = job;
    std::thread([job, title_id, &emuenv] {
        const fs::path source = save_path_for_title(emuenv, title_id);
        const fs::path export_dir = emuenv.log_path / "exports";
        const fs::path output = export_dir / (title_id + "-save.zip");
        try {
            if (!fs::exists(source) || fs::is_empty(source)) {
                job->message = "No save data exists for " + title_id;
            } else {
                fs::create_directories(export_dir);
                mz_zip_archive zip{};
                const std::string output_text = fs_utils::path_to_utf8(output);
                if (!mz_zip_writer_init_file(&zip, output_text.c_str(), 0)) {
                    job->message = "Could not create the save archive";
                } else {
                    bool ok = true;
                    std::size_t files = 0;
                    boost::system::error_code error;
                    for (fs::recursive_directory_iterator it(source, error), end; it != end && !error; it.increment(error)) {
                        if (!fs::is_regular_file(it->path(), error) || error)
                            continue;
                        const std::string relative = fs_utils::path_to_utf8(fs::relative(it->path(), source));
                        const std::string disk_path = fs_utils::path_to_utf8(it->path());
                        if (!safe_save_archive_path(relative)
                            || !mz_zip_writer_add_file(&zip, relative.c_str(), disk_path.c_str(), nullptr, 0, MZ_DEFAULT_COMPRESSION)) {
                            ok = false;
                            break;
                        }
                        ++files;
                    }
                    ok = ok && !error && files > 0 && mz_zip_writer_finalize_archive(&zip);
                    mz_zip_writer_end(&zip);
                    if (ok) {
                        job->success = true;
                        job->message = "Save exported";
                        job->share_path = output_text;
                    } else {
                        boost::system::error_code cleanup_error;
                        fs::remove(output, cleanup_error);
                        job->message = "Could not archive the complete save";
                    }
                }
            }
        } catch (const std::exception &error) {
            job->message = std::string("Save export failed: ") + error.what();
        }
        job->done.store(true);
    }).detach();
}

void start_save_import(EmuEnvState &emuenv, const std::string &title_id, const std::string &archive_path) {
    if (!safe_identifier(title_id, 16)) {
        vita3k_ios_report_import_result("Save import rejected an invalid title ID", false);
        return;
    }
    if (g_import_job && !g_import_job->done.load()) {
        vita3k_ios_report_import_result("Another file operation is still running", false);
        return;
    }
    auto job = std::make_shared<ImportJob>();
    job->rescan_apps = false;
    g_import_job = job;
    std::thread([job, title_id, archive_path, &emuenv] {
        const fs::path destination = save_path_for_title(emuenv, title_id);
        const fs::path staging = destination.parent_path() / (title_id + ".importing");
        const fs::path backup = destination.parent_path() / (title_id + ".backup");
        mz_zip_archive zip{};
        try {
            const std::string archive_text = fs_utils::path_to_utf8(fs::path(archive_path));
            if (!mz_zip_reader_init_file(&zip, archive_text.c_str(), 0)) {
                job->message = "The selected save is not a readable ZIP archive";
            } else {
                boost::system::error_code error;
                fs::remove_all(staging, error);
                fs::create_directories(staging, error);
                bool ok = !error;
                std::uint64_t total_size = 0;
                const mz_uint entries = mz_zip_reader_get_num_files(&zip);
                if (entries == 0 || entries > 100000)
                    ok = false;
                for (mz_uint index = 0; ok && index < entries; ++index) {
                    mz_zip_archive_file_stat stat{};
                    if (!mz_zip_reader_file_stat(&zip, index, &stat)
                        || !safe_save_archive_path(stat.m_filename)) {
                        ok = false;
                        break;
                    }
                    const unsigned unix_type = (stat.m_external_attr >> 16) & 0170000;
                    if (unix_type == 0120000 || stat.m_uncomp_size > (32ULL << 30)
                        || total_size > (32ULL << 30) - stat.m_uncomp_size) {
                        ok = false;
                        break;
                    }
                    total_size += stat.m_uncomp_size;
                    const fs::path output = staging / fs::path(stat.m_filename);
                    if (mz_zip_reader_is_file_a_directory(&zip, index)) {
                        fs::create_directories(output, error);
                    } else {
                        fs::create_directories(output.parent_path(), error);
                        const std::string output_text = fs_utils::path_to_utf8(output);
                        if (!error && !mz_zip_reader_extract_to_file(&zip, index, output_text.c_str(), 0))
                            ok = false;
                    }
                    if (error)
                        ok = false;
                }
                mz_zip_reader_end(&zip);
                if (ok) {
                    fs::remove_all(backup, error);
                    if (fs::exists(destination))
                        fs::rename(destination, backup, error);
                    if (!error)
                        fs::rename(staging, destination, error);
                    if (error && fs::exists(backup) && !fs::exists(destination)) {
                        boost::system::error_code rollback_error;
                        fs::rename(backup, destination, rollback_error);
                    }
                    if (!error) {
                        fs::remove_all(backup, error);
                        job->success = true;
                        job->message = "Save imported for " + title_id;
                    }
                }
                if (!job->success) {
                    fs::remove_all(staging, error);
                    job->message = "Save import was rejected; the existing save was left unchanged";
                }
            }
        } catch (const std::exception &error) {
            job->message = std::string("Save import failed: ") + error.what();
            mz_zip_reader_end(&zip);
        }
        boost::system::error_code cleanup_error;
        fs::remove(fs::path(archive_path), cleanup_error);
        job->done.store(true);
    }).detach();
}

void start_import(EmuEnvState &emuenv, const std::string &path, const bool firmware) {
    if (!firmware && !firmware_setup_complete(emuenv)) {
        vita3k_ios_report_import_result(
            "Install FONTPKG.PUP, PREINSTALL.PUP, and PSVUPDAT.PUP before importing games", false);
        return;
    }
    if (g_import_job && !g_import_job->done.load()) {
        vita3k_ios_report_import_result("Another import is still running", false);
        return;
    }
    auto job = std::make_shared<ImportJob>();
    job->firmware = firmware;
    g_import_job = job;
    // Installs take minutes for a PUP; never block the SDL/UIKit thread.
    std::thread([job, path, &emuenv] {
        try {
            if (job->firmware) {
                const std::string version = install_pup(emuenv.vita_fs_path, fs::path(path), nullptr);
                if (version.empty()) {
                    job->message = "Firmware install failed (see tsubomi.log)";
                } else {
                    fs::ofstream out(emuenv.log_path / "fw_version.txt");
                    out << version;
                    job->success = true;
                    job->message = "Firmware " + version + " installed";
                }
            } else if (fs::path(path).extension() == ".pkg" || fs::path(path).extension() == ".PKG") {
                std::string zrif = find_pkg_zrif(fs::path(path), emuenv.vita_fs_path);
                if (zrif.empty()) {
                    job->message = "PKG needs a matching license. Import its work.bin first, then select the .pkg again.";
                } else {
                    job->success = install_pkg(fs::path(path), emuenv, zrif, [](float) {});
                    job->message = job->success ? "PKG installed" : "PKG install failed (see tsubomi.log)";
                }
            } else {
                const auto result = packages::install_archive_transactionally(
                    std::filesystem::path(path), std::filesystem::path(emuenv.vita_fs_path.string()));
                job->success = result.success;
                job->installed_applications = result.installed_applications;
                job->message = result.success
                    ? "Installed " + std::to_string(result.application_count) + " application(s)"
                    : "Import failed: " + result.detail;
                if (!result.success)
                    LOG_ERROR("iOS archive install rejected: {}", result.detail);
            }
        } catch (const std::exception &error) {
            job->message = std::string("Import failed: ") + error.what();
        }
        boost::system::error_code cleanup_error;
        fs::remove(fs::path(path), cleanup_error);
        LOG_INFO("iOS import finished (success={}): {}", job->success, job->message);
        job->done.store(true);
    }).detach();
}

// After a successful game install, offer to import a NoNpDrm work.bin for the
// first retail full-game (`gd`) root that has no `.rif` license yet. DLC and
// patches ride on their base game's license, so they are skipped.
void maybe_prompt_license_import(EmuEnvState &emuenv,
    const std::vector<packages::ArchiveApplicationInfo> &applications) {
    for (const auto &application : applications) {
        if (application.category != "gd" || !application.title_id.starts_with("PCS"))
            continue;
        const fs::path rif = emuenv.vita_fs_path / "ux0/license" / application.title_id
            / (application.content_id + ".rif");
        boost::system::error_code exists_error;
        if (fs::exists(rif, exists_error) && !exists_error)
            continue;
        LOG_INFO("iOS: installed retail title {} has no license at {}; prompting for work.bin",
            application.title_id, rif);
        vita3k_ios_prompt_license_import(application.title_id);
        return; // One prompt at a time.
    }
}

std::string installed_version_for_title(const EmuEnvState &emuenv,
    const std::string &title_id, const std::string &base_version) {
    // The apps list is built from ux0/app, but Vita updates keep their newer
    // APP_VER in ux0/patch. Match desktop Vita3K by showing the installed
    // patch version whenever that SFO is present.
    const fs::path patch_sfo = emuenv.vita_fs_path / "ux0/patch" / title_id / "sce_sys/param.sfo";
    fs::ifstream input(patch_sfo, std::ios::binary);
    if (!input)
        return base_version;
    const std::vector<std::uint8_t> bytes((std::istreambuf_iterator<char>(input)), {});
    SfoFile sfo_file{};
    std::string patch_version;
    if (sfo::load(sfo_file, bytes) && sfo::get_data_by_key(patch_version, sfo_file, "APP_VER")
        && !patch_version.empty()) {
        LOG_INFO("iOS library version: {} base={} installed_patch={}", title_id, base_version, patch_version);
        return patch_version;
    }
    return base_version;
}

std::vector<Vita3KIOSGameEntry> native_games(EmuEnvState &emuenv) {
    const auto apps = app::get_apps(emuenv);
    const auto user_times = app::get_user_app_times(emuenv);
    std::vector<Vita3KIOSGameEntry> games;
    games.reserve(apps.size());
    for (const auto &entry : apps) {
        LOG_INFO("Installed title: {} ({}) category={} path={}",
            entry.title, entry.title_id, entry.category, entry.path);
        const fs::path art_directory = emuenv.vita_fs_path / "ux0/app" / entry.title_id / "sce_sys";
        const fs::path icon = art_directory / "icon0.png";
        const fs::path banner = art_directory / "pic0.png";
        // util/fs.h maps fs:: to boost::filesystem, whose non-throwing
        // overloads take boost::system::error_code, not std::error_code.
        boost::system::error_code icon_error;
        boost::system::error_code banner_error;
        const bool icon_exists = fs::exists(icon, icon_error);
        const bool banner_exists = fs::exists(banner, banner_error);
        LOG_INFO("iOS library art: title_id={} icon='{}' exists={} pic0='{}' exists={}",
            entry.title_id, icon, icon_exists, banner, banner_exists);
        const fs::path selected_art = banner_exists ? banner : icon;
        const auto time_it = user_times.find(entry.path.empty() ? entry.title_id : entry.path);
        const app::AppTime *app_time = time_it == user_times.end() ? nullptr : &time_it->second;
        const std::uint64_t installed_size = directory_size(emuenv.vita_fs_path / "ux0/app" / entry.title_id)
            + directory_size(emuenv.vita_fs_path / "ux0/patch" / entry.title_id)
            + directory_size(emuenv.vita_fs_path / "ux0/addcont" / entry.title_id);
        games.push_back({
            .title = entry.title,
            .title_id = entry.title_id,
            .category = entry.category,
            .app_path = entry.path.empty() ? entry.title_id : entry.path,
            .icon_path = fs_utils::path_to_utf8(selected_art),
            .version = installed_version_for_title(emuenv, entry.title_id, entry.app_ver),
            .trophy_id = trophy_id_for_title(emuenv, entry.title_id),
            .size_bytes = installed_size,
            .time_played_seconds = app_time ? app_time->time_used : 0,
            .last_played_timestamp = app_time ? app_time->last_time_used : 0,
        });
    }
    return games;
}

std::string restart_setting_name(config::RestartRequiredSetting setting) {
    switch (setting) {
    case config::RestartRequiredSetting::CpuOpt:
        return "CPU optimisation";
    case config::RestartRequiredSetting::ResolutionMultiplier:
        return "resolution multiplier";
    case config::RestartRequiredSetting::AudioBackend:
        return "audio backend";
    case config::RestartRequiredSetting::BackendRenderer:
        return "renderer";
    case config::RestartRequiredSetting::GraphicsDevice:
        return "graphics device";
    case config::RestartRequiredSetting::CustomDriver:
        return "custom driver";
    case config::RestartRequiredSetting::HighAccuracy:
        return "high accuracy";
    case config::RestartRequiredSetting::MemoryMapping:
        return "memory mapping";
    case config::RestartRequiredSetting::ValidationLayer:
        return "validation layer";
    }
    return "unknown setting";
}

void apply_native_settings(EmuEnvState &emuenv, const Vita3KIOSSettings &settings) {
    Config desired;
    desired = emuenv.cfg;
    auto apply = [&](Config::CurrentConfig &current) {
        current.resolution_multiplier = settings.resolution_multiplier;
        current.v_sync = settings.v_sync;
        // The game-dependent FPS hack was removed from the iOS UI because it
        // changes guest timing. Clear any value persisted by an older build.
        current.fps_hack = false;
        current.cpu_opt = settings.cpu_opt;
        current.ngs_enable = settings.ngs_enable;
        current.async_pipeline_compilation = settings.async_pipeline_compilation;
        current.anisotropic_filtering = settings.anisotropic_filtering;
        current.audio_backend = "SDL";
    };
    apply(desired.current_config);
    desired.resolution_multiplier = settings.resolution_multiplier;
    desired.v_sync = settings.v_sync;
    desired.fps_hack = false;
    desired.cpu_opt = settings.cpu_opt;
    desired.ngs_enable = settings.ngs_enable;
    desired.async_pipeline_compilation = settings.async_pipeline_compilation;
    desired.anisotropic_filtering = settings.anisotropic_filtering;
    desired.audio_backend = "SDL";

    const auto result = app::commit_settings(emuenv, desired);
    emuenv.display.fps_hack = false;
    emuenv.display.fps_limit.store(std::clamp(settings.fps_limit, 15, 60), std::memory_order_relaxed);
    std::vector<std::string> restart_required;
    restart_required.reserve(result.restart_required_settings.size());
    for (const auto setting : result.restart_required_settings)
        restart_required.push_back(restart_setting_name(setting));
    vita3k_ios_report_settings_result(restart_required);
    LOG_INFO("iOS settings saved: runtime_applied={} restart_required={}",
        result.runtime_settings_applied, restart_required.size());
}

std::optional<AppLaunchRequest> choose_boot_title(EmuEnvState &emuenv) {
    auto games = native_games(emuenv);
    if (games.empty()) {
        LOG_WARN("No installed titles were found under {}. Showing native empty-library instructions.",
            emuenv.vita_fs_path / "ux0/app");
    }
    vita3k_ios_show_library(games, native_settings(emuenv));

    for (;;) {
        SDL_Event event;
        while (SDL_PollEvent(&event)) {
            if (event.type == SDL_EVENT_QUIT || event.type == SDL_EVENT_WINDOW_CLOSE_REQUESTED
                || event.type == SDL_EVENT_TERMINATING) {
                vita3k_ios_hide_library();
                return std::nullopt;
            }
        }

        if (g_import_job && g_import_job->done.load()) {
            const bool was_firmware = g_import_job->firmware;
            const bool rescan_apps = g_import_job->rescan_apps;
            const bool success = g_import_job->success;
            const std::string message = g_import_job->message;
            const std::string share_path = g_import_job->share_path;
            const auto installed_applications = g_import_job->installed_applications;
            g_import_job.reset();
            if (rescan_apps && !was_firmware && !app::init_apps_list(emuenv))
                LOG_ERROR("Failed to rescan apps list after import.");
            if (rescan_apps) {
                games = native_games(emuenv);
                vita3k_ios_update_library(games, native_settings(emuenv));
            }
            vita3k_ios_report_import_result(message, success);
            if (!share_path.empty())
                vita3k_ios_share_file(share_path);
            if (success && rescan_apps && !was_firmware)
                maybe_prompt_license_import(emuenv, installed_applications);
        }

        if (auto action = vita3k_ios_take_frontend_action()) {
            switch (action->kind) {
            case Vita3KIOSFrontendActionKind::Launch:
                if (!firmware_setup_complete(emuenv)) {
                    vita3k_ios_show_boot_error(
                        "Install FONTPKG.PUP, PREINSTALL.PUP, and PSVUPDAT.PUP before playing games.");
                    break;
                }
                // Defense in depth: the library already refuses launches without
                // JIT, but re-probe here so a debugger attached after the probe
                // is honored and one attached-then-detached is caught.
                if (!ios_jit_available()) {
                    LOG_WARN("Refusing launch of '{}': JIT is not available (no debugger attached).",
                        action->app_path);
                    vita3k_ios_set_jit_available(false);
                    break;
                }
                vita3k_ios_set_jit_available(true);
                g_current_trophy_id.clear();
                g_current_title.clear();
                g_current_title_id.clear();
                for (const auto &game : games) {
                    if (game.app_path == action->app_path) {
                        g_current_trophy_id = game.trophy_id;
                        g_current_title = game.title;
                        g_current_title_id = game.title_id;
                        break;
                    }
                }
                LOG_INFO("Booting selected iOS library title: {}", action->app_path);
                vita3k_ios_hide_library();
                return AppLaunchRequest{.app_path = action->app_path};
            case Vita3KIOSFrontendActionKind::Refresh:
                LOG_INFO("Rescanning iOS game library");
                if (!app::init_apps_list(emuenv))
                    LOG_ERROR("Failed to rescan apps list.");
                games = native_games(emuenv);
                vita3k_ios_update_library(games, native_settings(emuenv));
                break;
            case Vita3KIOSFrontendActionKind::ApplySettings:
                apply_native_settings(emuenv, action->settings);
                vita3k_ios_update_library(games, native_settings(emuenv));
                break;
            case Vita3KIOSFrontendActionKind::ImportGame:
                LOG_INFO("Importing game archive: {}", action->app_path);
                start_import(emuenv, action->app_path, false);
                break;
            case Vita3KIOSFrontendActionKind::ImportFirmware:
                LOG_INFO("Importing firmware PUP: {}", action->app_path);
                start_import(emuenv, action->app_path, true);
                break;
            case Vita3KIOSFrontendActionKind::ImportLicense: {
                LOG_INFO("Importing NoNpDrm work.bin license: {}", action->app_path);
                const bool copied = copy_license(emuenv, fs::path(action->app_path));
                // A NoNpDrm dump's ux0:app content is still PFS-encrypted on
                // disk; copying the .rif alone leaves eboot.bin/PNGs encrypted
                // (decrypt_fself fails, art won't decode). Decrypt the installed
                // title in place with the work.bin, exactly like desktop.
                bool decrypted = false;
                if (copied && !emuenv.license_title_id.empty()) {
                    const fs::path title_path = emuenv.vita_fs_path / "ux0/app" / emuenv.license_title_id;
                    boost::system::error_code exists_error;
                    if (fs::exists(title_path, exists_error) && !exists_error) {
                        try {
                            decrypted = decrypt_install_nonpdrm(emuenv, fs::path(action->app_path), title_path);
                        } catch (const std::exception &error) {
                            LOG_ERROR("NoNpDrm content decrypt failed: {}", error.what());
                        }
                    }
                }
                boost::system::error_code cleanup_error;
                fs::remove(fs::path(action->app_path), cleanup_error);
                // Rescan so the (now decryptable) art and titles refresh without
                // an app restart.
                if (!app::init_apps_list(emuenv))
                    LOG_ERROR("Failed to rescan apps after license import.");
                games = native_games(emuenv);
                vita3k_ios_update_library(games, native_settings(emuenv));
                vita3k_ios_report_import_result(
                    copied ? (decrypted ? "License installed; content decrypted"
                                        : "License installed")
                           : "License import failed (see tsubomi.log)", copied);
                break;
            }
            case Vita3KIOSFrontendActionKind::ImportSave:
                start_save_import(emuenv, action->title_id, action->app_path);
                break;
            case Vita3KIOSFrontendActionKind::ExportSave:
                start_save_export(emuenv, action->title_id);
                break;
            case Vita3KIOSFrontendActionKind::ShowTrophies:
                show_trophies(emuenv, action->trophy_id, action->title_id, action->app_path);
                break;
            case Vita3KIOSFrontendActionKind::Quit:
                vita3k_ios_hide_library();
                return std::nullopt;
            }
        }

        // Re-probe JIT roughly once a second so the banner clears live if the
        // user attaches StikDebug while the library is on screen.
        {
            static Uint64 last_jit_probe_ms = 0;
            const Uint64 now_ms = SDL_GetTicks();
            if (now_ms - last_jit_probe_ms >= 1000) {
                last_jit_probe_ms = now_ms;
                vita3k_ios_set_jit_available(ios_jit_available());
            }
        }

        // Service UIKit instead of a blind sleep so library scrolling and the
        // settings sliders stay smooth on this SDL/UIKit-owning thread.
        vita3k_ios_pump_runloop(0.016);
    }
}

// Fatal-signal logger: several device deaths left no trace in vita3k.log
// because they were not SEGV/BUS data faults (mem.cpp already logs those).
// Log the signal, fault address, PC and its owning image, then re-raise with
// the default action so the OS still writes its crash report.
void fatal_signal_handler(int sig, siginfo_t *info, void *uct) {
    uintptr_t pc = 0;
#if defined(__APPLE__) && defined(__aarch64__)
    if (uct) {
        auto *context = static_cast<ucontext_t *>(uct);
        pc = context->uc_mcontext->__ss.__pc;
        // Oaknut asks StikDebug to prepare an iOS 26 executable mapping with
        // BRK #0xf00d. If StikDebug detaches in the tiny interval after our
        // P_TRACED check, that BRK reaches the app as SIGTRAP. Return nullptr
        // from the naked helper so the allocator throws and the launch returns
        // to the library with an actionable error instead of killing Tsubomi.
        constexpr std::uint32_t universal_jit_breakpoint = 0xD43E01A0;
        if (sig == SIGTRAP && pc != 0
            && *reinterpret_cast<const std::uint32_t *>(pc) == universal_jit_breakpoint) {
            context->uc_mcontext->__ss.__x[0] = 0;
            context->uc_mcontext->__ss.__pc = pc + sizeof(std::uint32_t);
            g_unhandled_universal_jit_breakpoint.store(true, std::memory_order_relaxed);
            static constexpr char message[] =
                "Tsubomi: StikDebug detached during universal JIT preparation; aborting launch safely.\n";
            write(STDERR_FILENO, message, sizeof(message) - 1);
            return;
        }
    }
#endif
    Dl_info dl_info{};
    const char *image = "?";
    uintptr_t image_base = 0;
    if (pc && dladdr(reinterpret_cast<void *>(pc), &dl_info) && dl_info.dli_fname) {
        image = dl_info.dli_fname;
        image_base = reinterpret_cast<uintptr_t>(dl_info.dli_fbase);
    }
    // Not async-signal-safe, but the process is dying anyway and this is the
    // only channel that reaches the log before the kill.
    LOG_CRITICAL("FATAL SIGNAL {}: PC=0x{:X} (image '{}' +0x{:X}) fault_addr=0x{:X} available_mem={} MiB",
        sig, pc, image, image_base ? pc - image_base : 0,
        info ? reinterpret_cast<uintptr_t>(info->si_addr) : 0,
        static_cast<unsigned long long>(os_proc_available_memory() / (1024 * 1024)));

    // Host-side backtrace of the crashing thread. backtrace_symbols_fd is
    // async-signal-safe and reaches the device console; also log the raw
    // frames so they land in the file log we ship back.
    void *frames[64];
    const int frame_count = backtrace(frames, 64);
    backtrace_symbols_fd(frames, frame_count, STDERR_FILENO);
    for (int i = 0; i < frame_count; ++i) {
        Dl_info frame_info{};
        const char *frame_image = "?";
        uintptr_t frame_off = 0;
        if (dladdr(frames[i], &frame_info) && frame_info.dli_fname) {
            frame_image = frame_info.dli_fname;
            frame_off = reinterpret_cast<uintptr_t>(frames[i])
                - reinterpret_cast<uintptr_t>(frame_info.dli_fbase);
        }
        LOG_CRITICAL("  #{:02} 0x{:X} ({} +0x{:X})", i,
            reinterpret_cast<uintptr_t>(frames[i]), frame_image, frame_off);
    }
    if (auto logger = spdlog::default_logger())
        logger->flush();
    signal(sig, SIG_DFL);
    raise(sig);
}

void install_fatal_signal_logger() {
    // Run the handler on its own stack so a stack-overflow fault can still be
    // reported instead of double-faulting silently.
    static std::array<char, SIGSTKSZ> alt_stack_storage;
    stack_t alt_stack{};
    alt_stack.ss_sp = alt_stack_storage.data();
    alt_stack.ss_size = alt_stack_storage.size();
    sigaltstack(&alt_stack, nullptr);

    struct sigaction sa{};
    sa.sa_flags = SA_SIGINFO | SA_ONSTACK;
    sigemptyset(&sa.sa_mask);
    sa.sa_sigaction = fatal_signal_handler;
    // SIGSEGV/SIGBUS belong to mem.cpp's guest-fault handler; it raises
    // SIGTRAP for anything it cannot handle, which lands here and gets logged.
    for (const int sig : { SIGABRT, SIGILL, SIGTRAP, SIGFPE })
        sigaction(sig, &sa, nullptr);
}

bool has_physical_controller(CtrlState &state) {
    const std::lock_guard lock(state.mutex);
    return std::any_of(state.controllers.begin(), state.controllers.end(), [](const auto &entry) {
        const char *name = entry.second.name;
        return name == nullptr || std::string_view(name) != "Vita3K iOS Touch Controller";
    });
}

constexpr std::size_t IOS_JIT_CACHE_SIZE = 16 * 1024 * 1024;
constexpr std::size_t IOS_JIT_POOL_TARGET = 24;

bool prepare_ios_jit_pool() {
    if (g_jit_pool_ready.load(std::memory_order_relaxed))
        return true;
    if (!ios_debugger_attached())
        return false;

    g_unhandled_universal_jit_breakpoint.store(false, std::memory_order_relaxed);
    try {
        const std::size_t warmed_jit_regions =
            prewarm_ios_jit_code_cache_pool(IOS_JIT_POOL_TARGET, IOS_JIT_CACHE_SIZE);
        if (warmed_jit_regions < IOS_JIT_POOL_TARGET) {
            LOG_CRITICAL("iOS JIT region pool is under target: target={} available={}",
                IOS_JIT_POOL_TARGET, warmed_jit_regions);
            if (auto logger = spdlog::default_logger())
                logger->flush();
            return false;
        }
    } catch (const std::exception &error) {
        LOG_ERROR("iOS JIT region pool preparation failed: {}", error.what());
        return false;
    } catch (...) {
        LOG_ERROR("iOS JIT region pool preparation failed with an unknown exception");
        return false;
    }

    g_jit_pool_ready.store(true, std::memory_order_relaxed);
    vita3k_ios_set_jit_available(true);
    return true;
}

} // namespace

int main(int argc, char *argv[]) {
    Root root_paths;
    std::unique_ptr<EmuEnvState> emuenv;

    if (!initialize_session(ios_storage_path(), root_paths, emuenv) || !emuenv) {
        // Logging may not be up; SDL_Log reaches the device console either way.
        SDL_Log("Vita3K iOS: session initialisation failed.");
        return -1;
    }

    install_fatal_signal_logger();

    // Activate the iOS audio session before SDL opens the audio device, so the
    // audio unit actually runs and the SDL stream drains (otherwise games that
    // wait on audio playback position freeze).
    vita3k_ios_configure_audio_session();

    SDL_SetHint(SDL_HINT_ORIENTATIONS, "Portrait LandscapeLeft LandscapeRight");
    if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_GAMEPAD)) {
        LOG_ERROR("SDL_Init failed: {}", SDL_GetError());
        return -1;
    }

    // SDL may not emit GAMEPAD_ADDED for a controller that was already
    // connected before Vita3K launched. Match the Android frontend by doing
    // an initial enumeration so guest sceCtrl polling works from frame one.
    refresh_controllers(emuenv->ctrl, *emuenv);
    LOG_INFO("iOS controller discovery: {} connected controller(s)", emuenv->ctrl.controllers_num);

    SDL_PropertiesID window_props = SDL_CreateProperties();
    if (!window_props) {
        LOG_ERROR("SDL_CreateProperties failed: {}", SDL_GetError());
        return -1;
    }
    SDL_SetStringProperty(window_props, SDL_PROP_WINDOW_CREATE_TITLE_STRING, "Tsubomi");
    SDL_SetNumberProperty(window_props, SDL_PROP_WINDOW_CREATE_WIDTH_NUMBER, 960);
    SDL_SetNumberProperty(window_props, SDL_PROP_WINDOW_CREATE_HEIGHT_NUMBER, 544);
    // HIGH_PIXEL_DENSITY is essential: without it SDL's Metal layer stays at
    // contentsScale 1 and the whole game renders at point resolution (the
    // 402x874 "extremely pixelated" drawable seen in device logs).
    SDL_SetNumberProperty(window_props, SDL_PROP_WINDOW_CREATE_FLAGS_NUMBER,
        SDL_WINDOW_VULKAN | SDL_WINDOW_FULLSCREEN | SDL_WINDOW_HIGH_PIXEL_DENSITY);

    SDL_Window *window = SDL_CreateWindowWithProperties(window_props);
    SDL_DestroyProperties(window_props);
    if (!window) {
        LOG_ERROR("SDL_CreateWindowWithProperties failed: {}", SDL_GetError());
        return -1;
    }

    const bool initial_jit_available = ios_jit_available();
    vita3k_ios_set_jit_available(initial_jit_available);
    LOG_INFO("iOS JIT availability probe: {}",
        initial_jit_available ? "available (process is traced)"
                              : "unavailable (no debugger attached)");

    // Reserve the guest address space FIRST (the 24 JIT mappings fragment it
    // otherwise and mem::init later fails with ENOMEM), then allocate the JIT
    // region pool immediately while StikDebug is still attached. StikDebug
    // commonly detaches within a minute of app launch; new RWX regions cannot
    // be created after that, which used to make any delayed first boot fail.
    if (!prereserve_guest_memory()) {
        LOG_CRITICAL("Could not prereserve guest memory at startup; JIT pool prewarm deferred to first boot");
    } else if (initial_jit_available) {
        if (prepare_ios_jit_pool())
            LOG_INFO("iOS JIT region pool prepared at startup; later StikDebug detach no longer blocks boots");
        else
            LOG_WARN("iOS JIT region pool startup preparation failed; will retry at first boot");
    }

    // Library -> game -> library loop: quitting a game returns to the
    // library instead of leaving a dead process behind (the old "freeze").
    bool app_terminating = false;
    bool jit_pool_prewarmed = g_jit_pool_ready.load(std::memory_order_relaxed);
    while (!app_terminating) {
    auto launch_request = choose_boot_title(*emuenv);
    if (!launch_request)
        break;

    app::AppSessionController session_controller(*emuenv);
    SDL_Log("Vita3K iOS: begin_launch '%s'", launch_request->app_path.c_str());
    if (!session_controller.begin_launch(*launch_request)) {
        LOG_ERROR("Could not find app '{}' in apps list.", launch_request->app_path);
        continue;
    }

    IOSFrameHost frame_host(window);

    // A failed launch (bad renderer init, encrypted/undecryptable content, a
    // throwing loader) must return to the library with an explanation instead
    // of tearing the whole app down (the old "Unhandled std::terminate()").
    std::string boot_error;
    try {
        SDL_Log("Vita3K iOS: initialize_renderer (Vulkan/MoltenVK)");
        if (!session_controller.initialize_renderer(frame_host)) {
            boot_error = "Could not initialise the graphics renderer.";
        } else {
            SDL_Log("Vita3K iOS: initialize_runtime (kernel/CPU - requires JIT)");
            if (!session_controller.initialize_runtime()) {
                boot_error = "Could not initialise the emulator runtime or reserve guest memory. "
                             "Restart Tsubomi, re-enable JIT in StikDebug, and try again.";
            } else {
                // Prepare every JIT mapping the session is expected to need
                // while StikDebug is known to be attached. iOS 26 keeps these
                // RX/RW aliases executable after the debugger app is suspended.
                if (!jit_pool_prewarmed && !prepare_ios_jit_pool()) {
                    boot_error = "StikDebug detached while Tsubomi was preparing JIT. Re-enable JIT, keep "
                                 "StikDebug attached until preparation completes, then try again.";
                    vita3k_ios_set_jit_available(false);
                } else {
                    jit_pool_prewarmed = true;
                }

                if (boot_error.empty())
                    SDL_Log("Vita3K iOS: load_and_run");
                if (boot_error.empty() && !session_controller.load_and_run())
                    boot_error = "Could not load or start the game. If this is a retail dump, the "
                                 "content may still be encrypted — import the .pkg with its "
                                 "work.bin/zRIF instead of a pre-extracted copy.";
            }
        }
    } catch (const std::exception &error) {
        if (!jit_pool_prewarmed
            && (g_unhandled_universal_jit_breakpoint.exchange(false, std::memory_order_relaxed)
                || !ios_debugger_attached())) {
            boot_error = "StikDebug detached while Tsubomi was preparing JIT. Re-enable JIT, keep "
                         "StikDebug attached until preparation completes, then try again.";
            vita3k_ios_set_jit_available(false);
        } else {
            boot_error = std::string("The game crashed during startup: ") + error.what();
        }
    } catch (...) {
        boot_error = "The game crashed during startup.";
    }

    if (!boot_error.empty()) {
        LOG_ERROR("iOS boot failed: {}", boot_error);
        session_controller.stop(app::AppSessionStopReason::UserRequest);
        emuenv->audio.adapter.reset();
        emuenv->audio.audio_backend.clear();
        vita3k_ios_show_boot_error(boot_error);
        continue;
    }

    LOG_INFO("Game started: {} ({})", emuenv->current_app_title, launch_request->app_path);
    // Never inherit the removed iOS FPS-hack setting from an older config.
    emuenv->display.fps_hack = false;
    emuenv->display.fps_limit.store(vita3k_ios_load_fps_limit(), std::memory_order_relaxed);

    const bool has_virtual_controller = vita3k_ios_attach_virtual_controller();
    if (has_virtual_controller) {
        // Register the virtual joystick immediately instead of waiting for the
        // queued SDL_EVENT_GAMEPAD_ADDED. It is merged with any physical pad
        // by the normal sceCtrl polling path.
        refresh_controllers(emuenv->ctrl, *emuenv);
        LOG_INFO("iOS virtual controller ready: {} total controller(s)", emuenv->ctrl.controllers_num);
        vita3k_ios_set_physical_controller_connected(has_physical_controller(emuenv->ctrl));
        vita3k_ios_show_virtual_controller();
    }

    // Run the guest watchdog on its own host thread. Keeping it in the SDL
    // event loop meant a blocked frontend call could suppress the very dump
    // needed to diagnose the hang. The early two-second sample catches the
    // first CRI filesystem worker even if iOS is backgrounded soon afterward.
    std::atomic_bool stop_guest_watchdog = false;
    std::thread guest_watchdog([&] {
        using namespace std::chrono_literals;

        const Uint64 watchdog_start_ms = SDL_GetTicks();
        constexpr Uint64 scheduled_dump_at_ms[] = { 2000, 3000, 10000, 30000, 60000, 180000 };
        std::size_t next_scheduled_dump = 0;
        uint64_t last_setframe_seen = emuenv->display.last_setframe_vblank_count.load();
        Uint64 last_setframe_change_ms = watchdog_start_ms;
        Uint64 next_stall_dump_ms = watchdog_start_ms + 8000;

        LOG_INFO("iOS guest watchdog started: first snapshot at {}ms", scheduled_dump_at_ms[0]);

        while (!stop_guest_watchdog.load(std::memory_order_relaxed)) {
            std::this_thread::sleep_for(250ms);
            if (stop_guest_watchdog.load(std::memory_order_relaxed))
                break;

            const Uint64 now_ms = SDL_GetTicks();
            const uint64_t setframe_count = emuenv->display.last_setframe_vblank_count.load();
            if (setframe_count != last_setframe_seen) {
                last_setframe_seen = setframe_count;
                last_setframe_change_ms = now_ms;
            }

            // Sample the OS memory headroom every ~2s. If a freeze is really a
            // jetsam kill, the log shows this number collapsing toward zero
            // right before the process dies (no signal is delivered for jetsam).
            static Uint64 last_mem_log_ms = 0;
            if (now_ms - last_mem_log_ms >= 2000) {
                last_mem_log_ms = now_ms;
                LOG_INFO("iOS memory headroom: {} MiB available before jetsam",
                    static_cast<unsigned long long>(os_proc_available_memory() / (1024 * 1024)));
            }

            if (next_scheduled_dump < std::size(scheduled_dump_at_ms)
                && now_ms - watchdog_start_ms >= scheduled_dump_at_ms[next_scheduled_dump]) {
                LOG_INFO("iOS guest watchdog snapshot firing at {}ms", scheduled_dump_at_ms[next_scheduled_dump]);
                app::dump_guest_state(*emuenv, "scheduled iOS boot diagnostic");
                ++next_scheduled_dump;
            }

            if (now_ms - last_setframe_change_ms >= 8000 && now_ms >= next_stall_dump_ms) {
                app::dump_guest_state(*emuenv, "no sceDisplaySetFrameBuf progress for 8s");
                next_stall_dump_ms = now_ms + 30000;
            }
        }
    });

    Uint64 perf_last_ms = SDL_GetTicks();
    std::size_t perf_last_frame_count = emuenv->frame_count;
    Uint64 playtime_checkpoint_ms = perf_last_ms;

    bool running = true;
    while (running) {
        SDL_Event event;
        while (SDL_PollEvent(&event)) {
            switch (event.type) {
            case SDL_EVENT_TERMINATING:
                app_terminating = true;
                running = false;
                break;

            case SDL_EVENT_QUIT:
            case SDL_EVENT_WINDOW_CLOSE_REQUESTED:
                // In-game menu "Quit Game" pushes SDL_EVENT_QUIT: end the
                // session and fall back to the library.
                running = false;
                break;

            case SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED:
            case SDL_EVENT_WINDOW_RESIZED: {
                int drawable_width = 0;
                int drawable_height = 0;
                SDL_GetWindowSizeInPixels(window, &drawable_width, &drawable_height);
                LOG_INFO("iOS window resized: drawable={}x{} layout={}",
                    drawable_width, drawable_height,
                    drawable_height > drawable_width ? "portrait" : "landscape");
                // MoltenVK does not reliably report the swapchain as
                // out-of-date after a rotation; it scales the stale-extent
                // swapchain to the layer instead (nearest-filtered, visibly
                // pixelated). Force a rebuild at the new drawable size.
                if (emuenv->renderer)
                    emuenv->renderer->request_screen_rebuild();
                break;
            }

            case SDL_EVENT_FINGER_DOWN:
            case SDL_EVENT_FINGER_MOTION:
            case SDL_EVENT_FINGER_UP: {
                handle_touch_event(emuenv->touch, event.tfinger);
                auto &mouse = emuenv->ctrl.overlay_mouse;
                mouse.x.store(event.tfinger.x * 960.f, std::memory_order_relaxed);
                mouse.y.store(event.tfinger.y * 544.f, std::memory_order_relaxed);
                mouse.pressed.store(event.type != SDL_EVENT_FINGER_UP, std::memory_order_relaxed);
                break;
            }

            case SDL_EVENT_GAMEPAD_ADDED:
            case SDL_EVENT_GAMEPAD_REMOVED:
                refresh_controllers(emuenv->ctrl, *emuenv);
                vita3k_ios_set_physical_controller_connected(has_physical_controller(emuenv->ctrl));
                LOG_INFO("iOS controller refresh: {} connected controller(s)", emuenv->ctrl.controllers_num);
                break;

            case SDL_EVENT_GAMEPAD_BUTTON_DOWN:
                // Vita inputs are polled from SDL by sceCtrl; this breadcrumb
                // proves the host controller event reached the iOS frontend.
                LOG_DEBUG("iOS gamepad button down: gamepad={} button={}",
                    event.gbutton.which, static_cast<int>(event.gbutton.button));
                break;

            case SDL_EVENT_GAMEPAD_TOUCHPAD_DOWN:
            case SDL_EVENT_GAMEPAD_TOUCHPAD_MOTION:
            case SDL_EVENT_GAMEPAD_TOUCHPAD_UP:
                handle_touchpad_event(emuenv->touch, event.gtouchpad);
                break;

            default:
                break;
            }
        }

        {
            const Uint64 now_ms = SDL_GetTicks();
            if (now_ms - perf_last_ms >= 1000) {
                const std::size_t frames = emuenv->frame_count;
                const float fps = static_cast<float>(frames - perf_last_frame_count) * 1000.0f
                    / static_cast<float>(now_ms - perf_last_ms);
                perf_last_frame_count = frames;
                perf_last_ms = now_ms;
                const float frametime_ms = fps > 0.01f ? 1000.0f / fps : 0.0f;
                vita3k_ios_update_perf_overlay(fps, frametime_ms);
            }
            // Persist progress periodically, not only on a clean in-app quit.
            // iOS users commonly terminate a stalled title from the app
            // switcher, which previously discarded the whole session length.
            if (now_ms - playtime_checkpoint_ms >= 30000) {
                app::update_app_time_used(*emuenv, emuenv->io.app_path);
                playtime_checkpoint_ms = now_ms;
            }
        }

        if (auto action = vita3k_ios_take_frontend_action()) {
            if (action->kind == Vita3KIOSFrontendActionKind::ShowTrophies)
                show_trophies(*emuenv, g_current_trophy_id, g_current_title, g_current_title_id);
        }

        if (auto request = emuenv->take_app_launch_request()) {
            // In-process relaunch (LoadExec) is not supported yet on iOS.
            LOG_WARN("Title requested relaunch of '{}'; stopping instead.", request->self_path);
            running = false;
        }

        if (!session_controller.is_running())
            running = false;

        // Service UIKit (virtual controller, in-game glass menu, perf overlay)
        // instead of a blind sleep so touch controls stay responsive.
        if (running)
            vita3k_ios_pump_runloop(0.016);
    }

    LOG_INFO("Shutting down game");
    stop_guest_watchdog.store(true, std::memory_order_relaxed);
    guest_watchdog.join();
    vita3k_ios_hide_perf_overlay();
    vita3k_ios_hide_virtual_controller();
    session_controller.stop(app_terminating
            ? app::AppSessionStopReason::FrontendShutdown
            : app::AppSessionStopReason::UserRequest);
    if (has_virtual_controller)
        vita3k_ios_detach_virtual_controller();

    // Match the Android frontend: drop the SDL audio adapter so the next
    // session opens a fresh device instead of reusing torn-down state.
    emuenv->audio.adapter.reset();
    emuenv->audio.audio_backend.clear();

    LOG_INFO("Returning to game library");
    } // while (!app_terminating)

    SDL_DestroyWindow(window);
    SDL_Quit();
    return 0;
}
