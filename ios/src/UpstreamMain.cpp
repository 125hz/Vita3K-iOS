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
#include <SDL3/SDL_messagebox.h>

#include <app/functions.h>
#include <app/session_controller.h>
#include <app/state.h>
#include <compat/functions.h>
#include <compat/state.h>
#include <config/functions.h>
#include <config/state.h>
#include <config/version.h>
#include <ctrl/functions.h>
#include <ctrl/state.h>
#include <display/state.h>
#include <emuenv/state.h>
#include <modules/module_parent.h>
#include <renderer/frame_host.h>
#include <renderer/functions.h>
#include <touch/functions.h>
#include <util/fs.h>
#include <util/log.h>

#include <chrono>
#include <cstddef>
#include <filesystem>
#include <iterator>
#include <memory>
#include <optional>
#include <string>
#include <thread>
#include <vector>

namespace {

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

private:
    SDL_Window *m_window = nullptr;
};

fs::path ios_storage_path() {
    // Documents/Vita3K inside the app sandbox. UIFileSharingEnabled is set,
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
    return documents / "Vita3K" / "";
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
        compat::load_from_disk(emuenv->compat, std::filesystem::path(emuenv->cache_path.string()));
        return true;
    } catch (const std::exception &error) {
        LOG_ERROR("Failed to initialize iOS storage path '{}': {}", storage_path, error.what());
        emuenv.reset();
        return false;
    }
}

std::optional<AppLaunchRequest> choose_boot_title(EmuEnvState &emuenv, SDL_Window *window) {
    const auto apps = app::get_apps(emuenv);
    if (apps.empty()) {
        LOG_ERROR("No installed titles were found under {}. Copy a working "
                  "desktop Vita3K data directory (vita/ux0/app/<TITLE_ID>, "
                  "firmware os0/vs0/sa0) into Documents/Vita3K via file sharing.",
            emuenv.vita_fs_path / "ux0/app");
        SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "No games installed",
            "No installed Vita titles were found in Documents/Vita3K/vita/ux0/app.", window);
        return std::nullopt;
    }

    for (const auto &entry : apps) {
        LOG_INFO("Installed title: {} ({}) category={} path={}",
            entry.title, entry.title_id, entry.category, entry.path);
    }

    std::vector<std::string> labels;
    labels.reserve(apps.size() + 1);
    for (const auto &entry : apps)
        labels.emplace_back(entry.title + " (" + entry.title_id + ")");
    labels.emplace_back("Cancel");

    std::vector<SDL_MessageBoxButtonData> buttons;
    buttons.reserve(labels.size());
    for (std::size_t index = 0; index < apps.size(); ++index) {
        SDL_MessageBoxButtonData button{};
        button.buttonID = static_cast<int>(index);
        button.text = labels[index].c_str();
        buttons.push_back(button);
    }
    SDL_MessageBoxButtonData cancel{};
    cancel.flags = SDL_MESSAGEBOX_BUTTON_ESCAPEKEY_DEFAULT;
    cancel.buttonID = -1;
    cancel.text = labels.back().c_str();
    buttons.push_back(cancel);

    SDL_MessageBoxData dialog{};
    dialog.flags = SDL_MESSAGEBOX_INFORMATION;
    dialog.window = window;
    dialog.title = "Vita3K iOS";
    dialog.message = "Choose a game to boot";
    dialog.numbuttons = static_cast<int>(buttons.size());
    dialog.buttons = buttons.data();

    int selection = -1;
    if (!SDL_ShowMessageBox(&dialog, &selection)) {
        LOG_ERROR("Could not show game picker: {}", SDL_GetError());
        return std::nullopt;
    }
    if (selection < 0 || static_cast<std::size_t>(selection) >= apps.size()) {
        LOG_INFO("Game selection cancelled.");
        return std::nullopt;
    }

    const auto &chosen = apps[static_cast<std::size_t>(selection)];
    LOG_INFO("Booting title: {} ({})", chosen.title, chosen.title_id);
    return AppLaunchRequest{
        .app_path = chosen.path.empty() ? chosen.title_id : chosen.path,
    };
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
    SDL_SetStringProperty(window_props, SDL_PROP_WINDOW_CREATE_TITLE_STRING, "Vita3K");
    SDL_SetNumberProperty(window_props, SDL_PROP_WINDOW_CREATE_WIDTH_NUMBER, 960);
    SDL_SetNumberProperty(window_props, SDL_PROP_WINDOW_CREATE_HEIGHT_NUMBER, 544);
    SDL_SetNumberProperty(window_props, SDL_PROP_WINDOW_CREATE_FLAGS_NUMBER,
        SDL_WINDOW_VULKAN | SDL_WINDOW_FULLSCREEN);

    SDL_Window *window = SDL_CreateWindowWithProperties(window_props);
    SDL_DestroyProperties(window_props);
    if (!window) {
        LOG_ERROR("SDL_CreateWindowWithProperties failed: {}", SDL_GetError());
        return -1;
    }

    auto launch_request = choose_boot_title(*emuenv, window);
    if (!launch_request) {
        SDL_DestroyWindow(window);
        SDL_Quit();
        return 0;
    }

    app::AppSessionController session_controller(*emuenv);
    SDL_Log("Vita3K iOS: begin_launch '%s'", launch_request->app_path.c_str());
    if (!session_controller.begin_launch(*launch_request)) {
        LOG_ERROR("Could not find app '{}' in apps list.", launch_request->app_path);
        return -1;
    }

    IOSFrameHost frame_host(window);

    SDL_Log("Vita3K iOS: initialize_renderer (Vulkan/MoltenVK)");
    if (!session_controller.initialize_renderer(frame_host)) {
        LOG_ERROR("Failed to initialise renderer.");
        return -1;
    }

    SDL_Log("Vita3K iOS: initialize_runtime (kernel/CPU - requires JIT)");
    if (!session_controller.initialize_runtime()) {
        LOG_ERROR("Failed late initialisation.");
        return -1;
    }

    SDL_Log("Vita3K iOS: load_and_run");
    if (!session_controller.load_and_run()) {
        LOG_ERROR("Failed to load or start the app session.");
        return -1;
    }

    LOG_INFO("Game started: {} ({})", emuenv->current_app_title, launch_request->app_path);

    // Run the guest watchdog on its own host thread. Keeping it in the SDL
    // event loop meant a blocked frontend call could suppress the very dump
    // needed to diagnose the hang. The early three-second sample catches the
    // CRI filesystem worker boundary before iOS is backgrounded to copy logs.
    std::jthread guest_watchdog([&](const std::stop_token stop_token) {
        using namespace std::chrono_literals;

        const Uint64 watchdog_start_ms = SDL_GetTicks();
        constexpr Uint64 scheduled_dump_at_ms[] = { 3000, 10000, 30000, 60000, 180000 };
        std::size_t next_scheduled_dump = 0;
        uint64_t last_setframe_seen = emuenv->display.last_setframe_vblank_count.load();
        Uint64 last_setframe_change_ms = watchdog_start_ms;
        Uint64 next_stall_dump_ms = watchdog_start_ms + 8000;

        while (!stop_token.stop_requested()) {
            std::this_thread::sleep_for(250ms);
            if (stop_token.stop_requested())
                break;

            const Uint64 now_ms = SDL_GetTicks();
            const uint64_t setframe_count = emuenv->display.last_setframe_vblank_count.load();
            if (setframe_count != last_setframe_seen) {
                last_setframe_seen = setframe_count;
                last_setframe_change_ms = now_ms;
            }

            if (next_scheduled_dump < std::size(scheduled_dump_at_ms)
                && now_ms - watchdog_start_ms >= scheduled_dump_at_ms[next_scheduled_dump]) {
                app::dump_guest_state(*emuenv, "scheduled iOS boot diagnostic");
                ++next_scheduled_dump;
            }

            if (now_ms - last_setframe_change_ms >= 8000 && now_ms >= next_stall_dump_ms) {
                app::dump_guest_state(*emuenv, "no sceDisplaySetFrameBuf progress for 8s");
                next_stall_dump_ms = now_ms + 30000;
            }
        }
    });

    bool running = true;
    while (running) {
        SDL_Event event;
        while (SDL_PollEvent(&event)) {
            switch (event.type) {
            case SDL_EVENT_QUIT:
            case SDL_EVENT_WINDOW_CLOSE_REQUESTED:
                running = false;
                break;

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

        if (auto request = emuenv->take_app_launch_request()) {
            // In-process relaunch (LoadExec) is not supported yet on iOS.
            LOG_WARN("Title requested relaunch of '{}'; stopping instead.", request->self_path);
            running = false;
        }

        if (!session_controller.is_running())
            running = false;

        if (running)
            SDL_Delay(16);
    }

    LOG_INFO("Shutting down game");
    guest_watchdog.request_stop();
    guest_watchdog.join();
    session_controller.stop(app::AppSessionStopReason::FrontendShutdown);
    return 0;
}
