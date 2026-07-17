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
#include <modules/module_parent.h>
#include <renderer/frame_host.h>
#include <renderer/functions.h>
#include <renderer/state.h>
#include <renderer/vulkan/state.h>
#include <touch/functions.h>
#include <util/fs.h>
#include <util/log.h>

#include <csignal>
#include <dlfcn.h>

#include <vita3k_ios/NativeFrontend.h>
#include <vita3k_ios/VirtualController.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <filesystem>
#include <iterator>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
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

Vita3KIOSSettings native_settings(const Config &cfg) {
    const auto &current = cfg.current_config;
    return {
        .resolution_multiplier = current.resolution_multiplier,
        .v_sync = current.v_sync,
        .fps_hack = current.fps_hack,
        .cpu_opt = current.cpu_opt,
        .ngs_enable = current.ngs_enable,
        .async_pipeline_compilation = current.async_pipeline_compilation,
        .anisotropic_filtering = current.anisotropic_filtering,
    };
}

std::vector<Vita3KIOSGameEntry> native_games(EmuEnvState &emuenv) {
    const auto apps = app::get_apps(emuenv);
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
        games.push_back({
            .title = entry.title,
            .title_id = entry.title_id,
            .category = entry.category,
            .app_path = entry.path.empty() ? entry.title_id : entry.path,
            .icon_path = fs_utils::path_to_utf8(selected_art),
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
        current.fps_hack = settings.fps_hack;
        current.cpu_opt = settings.cpu_opt;
        current.ngs_enable = settings.ngs_enable;
        current.async_pipeline_compilation = settings.async_pipeline_compilation;
        current.anisotropic_filtering = settings.anisotropic_filtering;
        current.audio_backend = "SDL";
    };
    apply(desired.current_config);
    desired.resolution_multiplier = settings.resolution_multiplier;
    desired.v_sync = settings.v_sync;
    desired.fps_hack = settings.fps_hack;
    desired.cpu_opt = settings.cpu_opt;
    desired.ngs_enable = settings.ngs_enable;
    desired.async_pipeline_compilation = settings.async_pipeline_compilation;
    desired.anisotropic_filtering = settings.anisotropic_filtering;
    desired.audio_backend = "SDL";

    const auto result = app::commit_settings(emuenv, desired);
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
    vita3k_ios_show_library(games, native_settings(emuenv.cfg));

    for (;;) {
        SDL_Event event;
        while (SDL_PollEvent(&event)) {
            if (event.type == SDL_EVENT_QUIT || event.type == SDL_EVENT_WINDOW_CLOSE_REQUESTED) {
                vita3k_ios_hide_library();
                return std::nullopt;
            }
        }

        if (auto action = vita3k_ios_take_frontend_action()) {
            switch (action->kind) {
            case Vita3KIOSFrontendActionKind::Launch:
                LOG_INFO("Booting selected iOS library title: {}", action->app_path);
                vita3k_ios_hide_library();
                return AppLaunchRequest{.app_path = action->app_path};
            case Vita3KIOSFrontendActionKind::Refresh:
                LOG_INFO("Rescanning iOS game library");
                if (!app::init_apps_list(emuenv))
                    LOG_ERROR("Failed to rescan apps list.");
                games = native_games(emuenv);
                vita3k_ios_update_library(games, native_settings(emuenv.cfg));
                break;
            case Vita3KIOSFrontendActionKind::ApplySettings:
                apply_native_settings(emuenv, action->settings);
                vita3k_ios_update_library(games, native_settings(emuenv.cfg));
                break;
            case Vita3KIOSFrontendActionKind::Quit:
                vita3k_ios_hide_library();
                return std::nullopt;
            }
        }
        SDL_Delay(16);
    }
}

// Fatal-signal logger: several device deaths left no trace in vita3k.log
// because they were not SEGV/BUS data faults (mem.cpp already logs those).
// Log the signal, fault address, PC and its owning image, then re-raise with
// the default action so the OS still writes its crash report.
void fatal_signal_handler(int sig, siginfo_t *info, void *uct) {
    uintptr_t pc = 0;
#if defined(__APPLE__) && defined(__aarch64__)
    if (uct)
        pc = static_cast<ucontext_t *>(uct)->uc_mcontext->__ss.__pc;
#endif
    Dl_info dl_info{};
    const char *image = "?";
    uintptr_t image_base = 0;
    if (pc && dladdr(reinterpret_cast<void *>(pc), &dl_info) && dl_info.dli_fname) {
        image = dl_info.dli_fname;
        image_base = reinterpret_cast<uintptr_t>(dl_info.dli_fbase);
    }
    // Not async-signal-safe, but the process is dying anyway and this is the
    // only channel that reaches vita3k.log before the kill.
    LOG_CRITICAL("FATAL SIGNAL {}: PC=0x{:X} (image '{}' +0x{:X}) fault_addr=0x{:X}",
        sig, pc, image, image_base ? pc - image_base : 0,
        info ? reinterpret_cast<uintptr_t>(info->si_addr) : 0);
    if (auto logger = spdlog::default_logger())
        logger->flush();
    signal(sig, SIG_DFL);
    raise(sig);
}

void install_fatal_signal_logger() {
    struct sigaction sa{};
    sa.sa_flags = SA_SIGINFO;
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

    auto launch_request = choose_boot_title(*emuenv);
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

    // Prepare every JIT mapping the session is expected to need while
    // StikDebug is known to be attached. iOS 26 keeps these RX/RW aliases
    // executable after the debugger app is suspended, so later guest worker
    // threads can take a prepared region without issuing BRK #0xf00d.
    constexpr std::size_t IOS_JIT_CACHE_SIZE = 16 * 1024 * 1024;
    constexpr std::size_t IOS_JIT_POOL_TARGET = 24;
    const std::size_t warmed_jit_regions =
        prewarm_ios_jit_code_cache_pool(IOS_JIT_POOL_TARGET, IOS_JIT_CACHE_SIZE);
    if (warmed_jit_regions < IOS_JIT_POOL_TARGET) {
        LOG_CRITICAL("iOS JIT region pool is under target: target={} available={}",
            IOS_JIT_POOL_TARGET, warmed_jit_regions);
        if (auto logger = spdlog::default_logger())
            logger->flush();
    }

    SDL_Log("Vita3K iOS: load_and_run");
    if (!session_controller.load_and_run()) {
        LOG_ERROR("Failed to load or start the app session.");
        return -1;
    }

    LOG_INFO("Game started: {} ({})", emuenv->current_app_title, launch_request->app_path);

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

    bool running = true;
    while (running) {
        SDL_Event event;
        while (SDL_PollEvent(&event)) {
            switch (event.type) {
            case SDL_EVENT_QUIT:
            case SDL_EVENT_WINDOW_CLOSE_REQUESTED:
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
                if (emuenv->renderer && emuenv->renderer->current_backend == renderer::Backend::Vulkan) {
                    auto &vk_state = static_cast<renderer::vulkan::VKState &>(*emuenv->renderer);
                    vk_state.screen_renderer.need_rebuild = true;
                }
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
                vita3k_ios_update_perf_overlay(fps);
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
    stop_guest_watchdog.store(true, std::memory_order_relaxed);
    guest_watchdog.join();
    vita3k_ios_hide_perf_overlay();
    vita3k_ios_hide_virtual_controller();
    session_controller.stop(app::AppSessionStopReason::FrontendShutdown);
    if (has_virtual_controller)
        vita3k_ios_detach_virtual_controller();
    return 0;
}
