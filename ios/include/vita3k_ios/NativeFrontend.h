// Vita3K emulator project
// Copyright (C) 2026 Vita3K team
//
// Native UIKit frontend layered above SDL's iOS window.

#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

struct Vita3KIOSGameEntry {
    std::string title;
    std::string title_id;
    std::string category;
    std::string app_path;
    std::string icon_path;
    std::string version;
    std::string trophy_id;
    std::uint64_t size_bytes = 0;
    std::int64_t time_played_seconds = 0;
    std::int64_t last_played_timestamp = 0;
    // Trophy progress for the library badge; total == 0 means "no trophy data".
    int trophies_unlocked = 0;
    int trophies_total = 0;
};

struct Vita3KIOSTrophyEntry {
    int id = 0;
    std::string name;
    std::string detail;
    std::string icon_path;
    int grade = 0;
    bool hidden = false;
    bool earned = false;
    std::uint64_t timestamp = 0;
};

struct Vita3KIOSTrophyCollection {
    std::string title;
    std::string trophy_id;
    int unlocked = 0;
    int total = 0;
    std::vector<Vita3KIOSTrophyEntry> trophies;
};

struct Vita3KIOSSettings {
    float resolution_multiplier = 1.0f;
    bool v_sync = true;
    int fps_limit = 60;
    bool cpu_opt = true;
    bool ngs_enable = true;
    bool async_pipeline_compilation = true;
    int anisotropic_filtering = 1;
    // Upstream's accurate render paths (no texture-viewport shortcut, shader
    // interlock where available). Slower, but bypasses the fast paths that
    // misrender some titles under MoltenVK.
    bool high_accuracy = false;
    // Keep GPU render targets synchronized with guest-visible surface data.
    // Some titles, including Gravity Rush, need this for correct lighting.
    bool surface_sync = false;
    // Physical face-button remap: which physical face-button position
    // (0=Bottom, 1=Right, 2=Left, 3=Top) triggers each Vita face button.
    // Global only (not a per-game override) - a controller's button layout
    // is a device property, not a per-game preference. Defaults are the
    // identity mapping (Cross=Bottom, Circle=Right, Square=Left,
    // Triangle=Top); some third-party controllers report face buttons in
    // Xbox-style positions where Vita3K expects PlayStation-style ones.
    int bind_cross = 0;
    int bind_circle = 1;
    int bind_square = 2;
    int bind_triangle = 3;
    // Display-only: installed firmware version shown on the library header.
    std::string firmware_version;
    // Games stay unavailable until all three official firmware packages have
    // populated their canonical partitions (pd0, vs0, and sa0).
    bool firmware_ready = false;
    bool font_package_ready = false;
    bool preinstalled_package_ready = false;
    bool main_firmware_ready = false;
    std::string missing_firmware;
};

enum class Vita3KIOSFrontendActionKind {
    Launch,
    Refresh,
    ApplySettings,
    // app_path carries the local file path of the picked archive/PUP.
    ImportGame,
    ImportFirmware,
    // app_path carries the local file path of a NoNpDrm work.bin license.
    ImportLicense,
    ImportSave,
    ExportSave,
    ShowTrophies,
    // title_id carries the title to remove from ux0 (app/patch/addcont).
    DeleteGame,
    Quit,
};

struct Vita3KIOSFrontendAction {
    Vita3KIOSFrontendActionKind kind = Vita3KIOSFrontendActionKind::Quit;
    std::string app_path;
    std::string title_id;
    std::string trophy_id;
    Vita3KIOSSettings settings;
    // Launch only: `settings` carries this title's per-game overrides and must
    // be applied for the session (without persisting to the global config).
    bool has_settings_override = false;
};

void vita3k_ios_show_library(const std::vector<Vita3KIOSGameEntry> &games,
    const Vita3KIOSSettings &settings);
void vita3k_ios_update_library(const std::vector<Vita3KIOSGameEntry> &games,
    const Vita3KIOSSettings &settings);
void vita3k_ios_hide_library();
std::optional<Vita3KIOSFrontendAction> vita3k_ios_take_frontend_action();
void vita3k_ios_report_settings_result(const std::vector<std::string> &restart_required);
int vita3k_ios_load_fps_limit();
void vita3k_ios_present_trophies(const Vita3KIOSTrophyCollection &collection);
void vita3k_ios_share_file(const std::string &path);
void vita3k_ios_request_current_trophies();

// Tells the library whether a JIT-enabling debugger is attached. When false,
// the library shows a persistent banner and refuses to launch games (guest
// execution needs writable-executable memory that only JIT provides).
void vita3k_ios_set_jit_available(bool available);

// Prompts the user to import a NoNpDrm work.bin license for a freshly
// installed retail title that has no `.rif` yet. If the user accepts and picks
// a file, an ImportLicense action is queued with its path.
void vita3k_ios_prompt_license_import(const std::string &title_id);

// Services the UIKit run loop for up to `seconds`. The frontend loops call
// this instead of sleeping so scrolling, sliders, and glass animations stay
// smooth while the SDL/UIKit thread is also polling emulator state.
void vita3k_ios_pump_runloop(double seconds);

// Configures and activates an AVAudioSession (Playback) before SDL opens the
// audio device. Without an active session iOS may not run the audio unit, so
// the SDL stream never drains and games that gate on audio playback (movie
// clocks, music sync) freeze. Safe to call once at startup.
void vita3k_ios_configure_audio_session();

// In-game performance HUD. Called ~once per second from the frontend loop
// with the guest frame rate; battery/RAM are sampled on the UIKit side. The
// HUD only appears when the user enabled at least one metric in settings.
void vita3k_ios_update_perf_overlay(float guest_fps, float frametime_ms);
void vita3k_ios_hide_perf_overlay();

// Dismisses the import-in-progress overlay and shows the outcome. Failures are
// surfaced as a dismissible alert (so the precise installer detail is readable)
// while successes use the transient status label.
void vita3k_ios_report_import_result(const std::string &message, bool success);

// Dismisses the "Booting…" overlay and shows a boot-failure alert so a failed
// launch returns to the library instead of taking the whole app down.
void vita3k_ios_show_boot_error(const std::string &message);

// Most recent formatted log lines (oldest first, newest last), fed by a
// spdlog callback sink registered at startup. Backs an optional in-game live
// log overlay: cheap to poll (a small in-memory ring buffer, no file IO), so
// the frontend can call it every frame or two while the overlay is visible.
std::vector<std::string> vita3k_ios_recent_log_lines();
